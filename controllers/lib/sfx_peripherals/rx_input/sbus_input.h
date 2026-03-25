/*
 * SbusInput — Futaba S.Bus receiver decoder
 *
 * Decodes the S.Bus serial protocol: 100000 baud, 8E2, inverted UART.
 * Extracts 16 proportional channels (11-bit → µs) plus digital CH17/CH18
 * and failsafe/frame-lost flags.
 *
 * Uses Arduino Stream* — works on any platform.  The user is responsible
 * for configuring the HardwareSerial with correct baud, parity, and
 * signal inversion before passing it to begin().
 *
 * Platform setup examples:
 *
 *   // ESP32 (inversion via begin() parameter):
 *   Serial1.begin(100000, SERIAL_8E2, RX_PIN, -1, true);
 *   SbusInput sbus;
 *   sbus.begin(&Serial1);
 *
 *   // Pico (inversion via setInvert()):
 *   Serial1.setRX(RX_PIN);
 *   Serial1.setInvert(true, false, false);
 *   Serial1.begin(100000, SERIAL_8E2);
 *   SbusInput sbus;
 *   sbus.begin(&Serial1);
 *
 * Implements the ChannelSource concept for use with RxInputs<SbusInput>.
 */

#ifndef SFX_SBUS_INPUT_H
#define SFX_SBUS_INPUT_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include "rx_common.h"

class SbusInput {
public:
    SbusInput() = default;
    ~SbusInput() { end(); }

    SbusInput(const SbusInput&) = delete;
    SbusInput& operator=(const SbusInput&) = delete;

    // ════════════════════════════════════════════════════════════
    //  Lifecycle
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Attach to an already-configured serial port
     *
     * The caller must have already called serial->begin(100000, SERIAL_8E2)
     * with appropriate inversion for their platform.
     *
     * @param serial  Pointer to a configured HardwareSerial (or any Stream)
     * @return true (always succeeds if serial is non-null)
     */
    bool begin(Stream* serial);

    /**
     * @brief Detach from the serial port (does NOT close the port)
     */
    void end();

    /**
     * @brief Read available bytes and parse any complete SBUS frames
     *
     * Non-blocking.  Call every loop iteration.
     * Uses timing-based frame synchronization (>2.5 ms gap = new frame).
     */
    void update();

    // ════════════════════════════════════════════════════════════
    //  ChannelSource interface
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Channel value in µs (1-based, 1..16)
     * @return Pulse width 800–2200 µs, or CENTER_US if unavailable
     */
    uint16_t channel_us(uint8_t ch) const;

    /** @brief Always 16 once a valid frame has been received */
    uint8_t channelCount() const { return _hasFrame ? SbusConfig::NUM_CHANNELS : 0; }

    /** @brief true if a valid frame was received within SIGNAL_TIMEOUT_MS */
    bool isValid() const;

    /** @brief true after begin() */
    bool isInitialized() const { return _serial != nullptr; }

    // ════════════════════════════════════════════════════════════
    //  SBUS-specific accessors
    // ════════════════════════════════════════════════════════════

    /** @brief Digital channel 17 (true = high) */
    bool ch17() const { return _flags & SbusConfig::FLAG_CH17; }

    /** @brief Digital channel 18 (true = high) */
    bool ch18() const { return _flags & SbusConfig::FLAG_CH18; }

    /** @brief Receiver reported frame lost */
    bool frameLost() const { return _flags & SbusConfig::FLAG_FRAME_LOST; }

    /** @brief Receiver is in failsafe mode */
    bool failsafe() const { return _flags & SbusConfig::FLAG_FAILSAFE; }

    /** @brief Raw flag byte from last frame */
    uint8_t flags() const { return _flags; }

    /** @brief Total valid frames received */
    uint32_t frameCount() const { return _frameCount; }

    /** @brief Total parse/sync errors */
    uint32_t errorCount() const { return _errorCount; }

private:
    Stream*  _serial = nullptr;

    // Frame buffer
    uint8_t  _buf[SbusConfig::FRAME_SIZE] = {};
    uint8_t  _bufIdx = 0;
    uint32_t _lastByte_us = 0;

    // Decoded channels
    uint16_t _channels_us[SbusConfig::NUM_CHANNELS] = {};
    uint8_t  _flags = 0;
    uint32_t _lastFrameMs = 0;
    bool     _hasFrame = false;

    // Stats
    uint32_t _frameCount = 0;
    uint32_t _errorCount = 0;

    /** Parse a complete 25-byte frame from _buf */
    void parseFrame();
};

#endif // SFX_PLATFORM_ESP32
#endif // SFX_SBUS_INPUT_H
