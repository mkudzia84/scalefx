/**
 * @file pcm5102a_codec.h
 * @brief TI PCM5102A Stereo DAC Driver
 *
 * Driver for the Texas Instruments PCM5102A 32-bit stereo audio DAC.
 * Unlike the TAS5825M, the PCM5102A has NO I2C/SPI control interface —
 * all configuration is via hardware pins.
 *
 * Key features:
 *  - 32-bit I2S input, up to 384 kHz sample rate
 *  - Hardware soft-mute via XSMT pin (LOW = mute, HIGH = unmute)
 *  - SCK auto-mode: tie SCK to GND removes need for system clock
 *  - 2.1 Vrms line-level output
 *  - Low-power standby via XSMT
 *
 * Pin connections (active pins only):
 *  - XSMT  — Soft mute control (REQUIRED: has internal pull-down, floats muted!)
 *  - FMT   — Audio format: LOW = I2S (default), HIGH = Left-Justified
 *  - DEMP  — De-emphasis: LOW = off (default), HIGH = 44.1 kHz
 *  - FLT   — Digital filter: LOW = normal latency (default), HIGH = low latency
 *  - SCK   — System clock: tie to GND for auto-mode (recommended)
 *
 * Typical wiring for ESP32-S3 (minimum):
 *  - BCK  → I2S BCLK (shared with I2S output)
 *  - DIN  → I2S DOUT (shared with I2S output)
 *  - LCK  → I2S LRCLK (shared with I2S output)
 *  - XSMT → Any GPIO (configured via setXsmtPin())
 *  - FMT  → GND (I2S format)
 *  - SCK  → GND (auto clock)
 *  - VCC  → 3.3V
 *  - GND  → GND
 *
 * @see https://www.ti.com/product/PCM5102A
 */

#ifndef PCM5102A_CODEC_H
#define PCM5102A_CODEC_H

#include <Arduino.h>
#include "../audio/audio_config.h"

/**
 * @class PCM5102ACodec
 * @brief TI PCM5102A audio DAC driver (pin-controlled, no I2C)
 *
 * Singleton driver for the PCM5102A DAC. All control is via GPIO pins:
 *  - XSMT for mute/unmute (mandatory for audio output)
 *  - FMT, FLT, DEMP for optional format/filter/de-emphasis control
 *
 * If no XSMT pin is configured, the codec reports initialized but cannot
 * control mute state — the hardware default depends on external wiring
 * (internal pull-down keeps XSMT LOW = muted if floating).
 */
class PCM5102ACodec {
public:
    static PCM5102ACodec& instance() {
        static PCM5102ACodec inst;
        return inst;
    }

    // Delete copy/move (singleton)
    PCM5102ACodec(const PCM5102ACodec&) = delete;
    PCM5102ACodec& operator=(const PCM5102ACodec&) = delete;
    PCM5102ACodec(PCM5102ACodec&&) = delete;
    PCM5102ACodec& operator=(PCM5102ACodec&&) = delete;

    // ---- Pin configuration (call BEFORE begin()) ----

    /** @brief Set the XSMT (soft mute) GPIO. LOW = mute, HIGH = unmute. */
    void setXsmtPin(int8_t pin) { _xsmtPin = pin; }

    /** @brief Set the FMT (format) GPIO. LOW = I2S, HIGH = Left-Justified. */
    void setFmtPin(int8_t pin) { _fmtPin = pin; }

    /** @brief Set the FLT (filter) GPIO. LOW = normal latency, HIGH = low latency. */
    void setFltPin(int8_t pin) { _fltPin = pin; }

    /** @brief Set the DEMP (de-emphasis) GPIO. LOW = off, HIGH = on (44.1 kHz). */
    void setDempPin(int8_t pin) { _dempPin = pin; }

    // ---- Codec interface (matches TAS5825Codec / SimpleI2SCodec) ----

    /**
     * @brief Initialize the PCM5102A codec
     * @param sample_rate Sample rate in Hz (informational only — PCM5102A auto-detects)
     * @return true always (no communication to fail)
     */
    bool begin(uint32_t sample_rate = 44100);

    /** @brief Reset to default state (unmuted, normal filter, no de-emphasis). */
    void reset();

    /**
     * @brief Set volume (0.0–1.0)
     * PCM5102A has no hardware volume control — this is handled by the mixer.
     * Values below a threshold trigger hardware mute via XSMT.
     */
    void setVolume(float volume);

    /** @brief Hardware mute via XSMT pin. */
    void setMute(bool mute);

    bool isInitialized() const { return _initialized; }
    const char* getModelName() const { return "PCM5102A"; }

    // ---- PCM5102A-specific control ----

    /** @brief Set audio format via FMT pin. true = Left-Justified, false = I2S. */
    void setLeftJustified(bool lj);

    /** @brief Set digital filter via FLT pin. true = low latency, false = normal. */
    void setLowLatencyFilter(bool lowLatency);

    /** @brief Set de-emphasis via DEMP pin. true = on (44.1 kHz optimized). */
    void setDeEmphasis(bool enabled);

    // ---- Status queries (CODEC_STATUS protocol) ----
    uint8_t getCodecType() const { return 2; }  // 2 = PCM5102A
    bool    getMuted() const { return _muted; }
    uint8_t getVolumeRegister() const { return 0; }  // No register-based volume
    uint8_t getSupplyVoltage() const { return 0; }   // No supply config
    int     getSdaPin() const { return -1; }          // No I2C
    int     getSclPin() const { return -1; }          // No I2C
    uint8_t getDeviceControlRegister() { return _getControlBits(); }
    uint8_t getFaultRegister() { return 0; }          // No fault register
    bool    testI2CConnection() { return false; }     // No I2C

    /** @brief Get XSMT pin number (-1 if not configured). */
    int8_t getXsmtPin() const { return _xsmtPin; }

private:
    PCM5102ACodec();  // Private constructor (singleton)

    /**
     * @brief Pack current pin states into a synthetic "control" byte for status queries.
     * @return Bits: [0]=XSMT, [1]=FMT, [2]=FLT, [3]=DEMP, [7]=initialized
     */
    uint8_t _getControlBits() const;

    // Pin assignments (-1 = not configured)
    int8_t _xsmtPin;
    int8_t _fmtPin;
    int8_t _fltPin;
    int8_t _dempPin;

    // State
    bool _initialized;
    bool _muted;
    bool _leftJustified;
    bool _lowLatencyFilter;
    bool _deEmphasis;
    uint32_t _sampleRate;
};

#endif // PCM5102A_CODEC_H
