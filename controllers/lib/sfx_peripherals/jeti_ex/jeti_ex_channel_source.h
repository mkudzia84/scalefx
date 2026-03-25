/*
 * JetiExChannelSource — ChannelSource adapter for JetiExBus
 *
 * Thin read-only wrapper that bridges a JetiExBus instance into the
 * RxInputs<TSource> system.  The JetiExBus peripheral is owned and
 * updated externally; this adapter just delegates channel queries.
 *
 * Usage:
 *
 *   // 1. Create and configure the Jeti peripheral:
 *   JetiExBus jeti;
 *   jeti.begin(&Serial2);
 *   jeti.setSensorInfo(0xA4CB, 0x0001, "ScaleFX");
 *   // ... register sensors, params ...
 *
 *   // 2. Wire channel data into RxInputs:
 *   RxInputs<JetiExChannelSource> rx;
 *   rx.source().attach(&jeti);
 *
 *   // 3. In loop:
 *   jeti.update();   // full protocol (channels + telemetry + config)
 *   rx.update();     // signal-loss detection + convenience helpers
 *   float aileron = rx.channelNorm(1);
 *
 * Note: JetiExChannelSource::update() delegates to JetiExBus::update(),
 * so calling rx.update() alone is sufficient if you don't need to call
 * jeti.update() separately.  Double-calling is safe (no-op on empty buffer).
 */

#ifndef SFX_JETI_EX_CHANNEL_SOURCE_H
#define SFX_JETI_EX_CHANNEL_SOURCE_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include "../rx_input/rx_common.h"

// Forward declaration — full header included by the user
class JetiExBus;

class JetiExChannelSource {
public:
    JetiExChannelSource() = default;
    ~JetiExChannelSource() = default;

    // Non-copyable (pointer semantics)
    JetiExChannelSource(const JetiExChannelSource&) = delete;
    JetiExChannelSource& operator=(const JetiExChannelSource&) = delete;

    // ════════════════════════════════════════════════════════════
    //  Attachment
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Connect to an externally-owned JetiExBus instance
     * @param bus  Pointer to a JetiExBus that has been begin()-ed
     */
    void attach(JetiExBus* bus) { _bus = bus; }

    /** @brief Disconnect (does NOT end the bus) */
    void detach() { _bus = nullptr; }

    /** @brief Get the attached bus (or nullptr) */
    JetiExBus* bus() const { return _bus; }

    // ════════════════════════════════════════════════════════════
    //  ChannelSource interface
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Delegates to JetiExBus::update() if attached
     *
     * Safe to call even if jeti.update() was already called this loop
     * (reads from an empty serial buffer are no-ops).
     */
    void update();

    /** @brief Stop — detaches but does NOT end the bus */
    void end() { detach(); }

    /** @brief Channel value in µs (1-based) */
    uint16_t channel_us(uint8_t ch) const;

    /** @brief Number of channels from the bus */
    uint8_t channelCount() const;

    /** @brief true if bus has valid channel data */
    bool isValid() const;

    /** @brief true if a bus is attached and initialized */
    bool isInitialized() const;

private:
    JetiExBus* _bus = nullptr;
};

#endif // SFX_PLATFORM_ESP32
#endif // SFX_JETI_EX_CHANNEL_SOURCE_H
