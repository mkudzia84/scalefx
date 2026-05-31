/*
 * SbusInputRole — Futaba S.Bus receiver on an `InputPort`.
 *
 * On `bind()` the role switches the port into SBUS mode
 * (`InputPort::configureSbus()` — 100 000 baud, 8E2, inverted UART),
 * then attaches a `SbusInput` decoder to the port's UART `sfx::Stream*`.
 *
 * SBUS gives 16 proportional channels (11-bit, mapped to ~988..2012 µs)
 * plus digital ch17/ch18 and failsafe / frame-lost flags.  The role
 * holds the decoder in-place; `tick()` calls `decoder.update()` to
 * pump bytes through the parser.  Channel reads are cheap (cached
 * inside the decoder).
 *
 * The role is ESP32-only — `SbusInput` is only built on ESP32 (SBUS
 * inversion is configured at the UART hardware level).
 */

#ifndef SFX_SBUS_INPUT_ROLE_H
#define SFX_SBUS_INPUT_ROLE_H

#include <platform/sfx_stream.h>   // sfx::Stream (was <Arduino.h>)
#include <cstdint>
#include <functional>

#include <platform/sfx_platform.h>
#include <ports/input_port.h>

#include "input_broadcaster.h"

#if SFX_PLATFORM_ESP32
#  include <rx_input/sbus_input.h>
#endif

namespace sfx_core {

class SbusInputRole {
public:
    /// Async broadcast callback: `(channel_count, valid, failsafe, frame_lost)`.
    using BroadcastCallback = std::function<void(uint8_t channels,
                                                 bool valid,
                                                 bool failsafe,
                                                 bool frameLost)>;

    SbusInputRole() = default;
    explicit SbusInputRole(sfx_peripherals::InputPort* port) { bind(port); }

    /// Switches port to SBUS mode, then attaches the decoder to the
    /// port's UART stream.  Returns false on capability mismatch or
    /// peripheral configure failure.
    bool bind(sfx_peripherals::InputPort* port);

    /// Channel value (1-based 1..16) in µs.  Returns 1500 (centre) when
    /// no frame received yet.
    uint16_t channel_us(uint8_t ch1based) const;

    /// Total channels reported in the last valid frame (always 16 once
    /// any frame has been seen, 0 before).
    uint8_t channelCount() const;

    /// True if a valid frame was received within the SBUS signal timeout.
    bool valid() const;

    bool failsafe()  const;
    bool frameLost() const;
    bool ch17()      const;
    bool ch18()      const;

    /// Total frames received since bind.
    uint32_t frameCount() const;
    /// Parse / sync errors since bind.
    uint32_t errorCount() const;

    /// Host subscribe/unsubscribe to the WIRE broadcast (hz!=0 / hz==0).  Does
    /// NOT affect the LOCAL effect feed (fixed firmware rate, always on).
    void setBroadcastHz(uint8_t hz);
    bool wireEnabled() const { return _bcast.wireEnabled(); }

    void onBroadcast(BroadcastCallback cb) { _onBroadcast = std::move(cb); }

    /// Tick — drives the decoder + broadcast timer.
    void tick();

private:
    sfx_peripherals::InputPort* _port = nullptr;
#if SFX_PLATFORM_ESP32
    SbusInput _decoder;
#endif

    InputBroadcaster  _bcast;        ///< shared cadence + wire-subscribe state
    BroadcastCallback _onBroadcast;
};

}  // namespace sfx_core

#endif  // SFX_SBUS_INPUT_ROLE_H
