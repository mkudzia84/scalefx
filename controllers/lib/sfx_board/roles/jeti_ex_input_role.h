/*
 * JetiExInputRole — Rx-facing Jeti EX Bus link (IN_1) handle.
 *
 * Thin marker/handle.  The actual I/O — channel decode, the multi-device
 * telemetry response, and the downstream (ESC) forward — is owned by the
 * board-unique JetiExpander (a Core-0 task), which the attach handler starts
 * with BOTH links (IN_1 = Rx, IN_2 = downstream).  This role validates the
 * port, carries the broadcast timer, and delegates channel + diagnostic reads
 * to the expander.  Effects read channels through `channel_us()` as for any
 * input role.  ESP32-only at runtime (the expander is ESP32-only).
 */

#ifndef SFX_JETI_EX_INPUT_ROLE_H
#define SFX_JETI_EX_INPUT_ROLE_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <platform/sfx_platform.h>
#include <ports/input_port.h>

#if SFX_PLATFORM_ESP32
#  include <jeti_ex/jeti_expander.h>
#endif

namespace sfx_core {

class JetiExInputRole {
public:
    /// Async broadcast callback: `(channel_count, valid, rxFrames, rxErrors)`.
    using BroadcastCallback = std::function<void(uint8_t channels,
                                                 bool valid,
                                                 uint32_t rxFrames,
                                                 uint32_t rxErrors)>;

    JetiExInputRole() = default;
    explicit JetiExInputRole(sfx_peripherals::InputPort* port) { bind(port); }

    /// Validate the JETI_EX capability and record the port.  The JetiExpander
    /// (started by the attach handler) does the actual configure + I/O.
    bool bind(sfx_peripherals::InputPort* port, uint32_t baud = 125000);

    /// Channel value (1-based 1..N) in µs — from the expander's IN_1 decode.
    uint16_t channel_us(uint8_t ch1based) const;
    uint8_t  channelCount() const;
    bool     valid()        const;

    uint32_t rxFrameCount()    const;
    uint32_t rxErrorCount()    const;
    uint32_t txResponseCount() const;
    uint32_t rxByteCount()     const;

    void setBroadcastHz(uint8_t hz);
    void onBroadcast(BroadcastCallback cb) { _onBroadcast = std::move(cb); }

    /// Tick — runs the optional broadcast timer only; the expander's task
    /// drives the UART, so the role must NOT touch it (no double-drive).
    void tick();

private:
    sfx_peripherals::InputPort* _port = nullptr;
    uint8_t  _broadcastHz          = 0;
    uint32_t _broadcastInterval_ms = 0;
    uint32_t _lastBroadcastMs      = 0;
    BroadcastCallback _onBroadcast;
};

}  // namespace sfx_core

#endif  // SFX_JETI_EX_INPUT_ROLE_H
