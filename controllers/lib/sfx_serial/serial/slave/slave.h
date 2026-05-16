/*
 * Generic Slave Protocol — packet types, error codes, callback signatures.
 *
 * A slave board exposes ONE protocol now: the generic component-collection
 * commands defined in this header.  All boards share these packet IDs;
 * the difference between a "GunFX" and a "LightFX" board is a runtime-
 * discovered component fingerprint returned by COMPONENT_LIST_RESP —
 * not a wire-format difference.
 *
 * Packet-range allocation (legacy per-board ranges 0x01-0x2F /
 * 0x40-0x5F / 0x60-0x7F are RETIRED — boards are migrating to the
 * generic protocol with no compatibility window, so we re-use the
 * cleared space with growth room):
 *
 *     0x01..0x0F   Identity / enumeration / status broadcast      (15 IDs)
 *     0x10..0x2F   Servo control                                  (32 IDs)
 *     0x30..0x4F   PWM control (incl. mode-mutability)            (32 IDs)
 *     0x50..0x7F   LED control (incl. event-sequence runtime)     (48 IDs)
 *     0x80..0xAF   HubFX (master) — unchanged
 *     0xA4..0xA6   Streaming                                      (existing)
 *     0xB0..0xEE   Available
 *     0xEF..0xFF   Core
 */

#ifndef SFX_SLAVE_PROTOCOL_H
#define SFX_SLAVE_PROTOCOL_H

#include <cstdint>

namespace SlavePacket {
    // ── Identity / enumeration / status broadcast ─────────────────────
    // 0x01..0x0F — fits at the start of the available space.
    //
    // **Re-enumeration:** `COMPONENT_LIST_REQ` is the canonical re-
    // enumeration mechanism — its response carries the LIVE current
    // mode of every component (PWM channels report their current
    // ComponentKind, which may differ from the boot-time default if
    // the master has reconfigured them via PWM_SET_MODE).  Master
    // re-queries whenever it needs the fresh fingerprint.
    constexpr uint8_t COMPONENT_LIST_REQ      = 0x01;  ///< query → COMPONENT_LIST_RESP (live runtime modes)
    constexpr uint8_t COMPONENT_LIST_RESP     = 0x02;  ///< [count:u8][ComponentInfo×N] grouped by kind
    constexpr uint8_t IDENT_GET_REQ           = 0x03;  ///< query → IDENT_GET_RESP
    constexpr uint8_t IDENT_GET_RESP          = 0x04;  ///< [boardType:u8][len:u8][utf8 name…]
    constexpr uint8_t IDENT_SET               = 0x05;  ///< [len:u8][utf8 name…] — persisted to YAML, ACK'd
    constexpr uint8_t SLAVE_STATUS_BROADCAST  = 0x06;  ///< slave → master/PC, async (TAG_ASYNC)
    constexpr uint8_t SLAVE_STATUS_RATE       = 0x07;  ///< [hz:u8][kindsBitmask:u8] — see below
    /// Synchronous status request — master polls on demand without
    /// waiting for the next broadcast.  Returns the SAME payload as
    /// `SLAVE_STATUS_BROADCAST` but with the master's current command
    /// tag (so it correlates as a normal query response, not async).
    /// `kindsBitmask` follows the same semantics as
    /// `SLAVE_STATUS_RATE` — filters which sections are included.
    constexpr uint8_t SLAVE_STATUS_REQ        = 0x08;  ///< [kindsBitmask:u8] → status payload tagged with the request tag

    // ── Battery monitoring (optional — see BatteryPolicy in sfx_peripherals/power/)
    //
    // A slave board MAY expose a battery sensor (ADC + resistor divider
    // or INA226 channel).  Boards that do bind a `TBattery` policy at
    // SlaveServer template instantiation; boards that don't get the
    // `NoBattery` stub which advertises `present = 0` here.  The
    // master discovers presence + chemistry via BATTERY_INFO_REQ — the
    // `CoreCapability::BATTERY` bit in INIT_READY is the cheap
    // pre-check.  Mirrors the existing core-range BATTERY_CONFIG
    // (0xEE) but extends it with explicit query + alert + reconfigure
    // packets so battery-equipped slaves are first-class enumerable.
    constexpr uint8_t BATTERY_INFO_REQ        = 0x09;  ///< query → BATTERY_INFO_RESP
    constexpr uint8_t BATTERY_INFO_RESP       = 0x0A;  ///< present + chemistry + cells + voltage + percentage + flags + thresholds
    constexpr uint8_t BATTERY_RECONFIGURE     = 0x0B;  ///< [chemistry:u8][cellCount:u8][customLow_mV:u16LE][customCritical_mV:u16LE]
    /// Async (TAG_ASYNC) — emitted on every low / critical transition
    /// (and on re-arm above the hysteresis margin).  The slave's
    /// BatteryStateMachine supplies hysteresis so the master never sees
    /// chatter at the threshold edge.  See BatteryAlertLevel below.
    constexpr uint8_t BATTERY_ALERT           = 0x0C;  ///< [level:u8][voltage_mV:u16LE][cellCount:u8]
    // 0x0D..0x0F reserved for future identity / lifecycle commands

    /// Kinds bitmask shared by SLAVE_STATUS_RATE and SLAVE_STATUS_REQ.
    /// 0 = include all kinds (default).  Set bits = include only those
    /// kinds; cleared kind sections are emitted with `count = 0`.
    namespace StatusKinds {
        constexpr uint8_t ALL     = 0x00;
        constexpr uint8_t SERVO   = 1u << 0;
        constexpr uint8_t PWM     = 1u << 1;
        constexpr uint8_t LED     = 1u << 2;
        constexpr uint8_t BATTERY = 1u << 3;
        constexpr uint8_t HEADER_ONLY = 1u << 7;   ///< skip per-component sections entirely
    }

    /// BATTERY_ALERT level byte — same values latched in BATTERY_INFO_RESP flags.
    namespace BatteryAlertLevel {
        constexpr uint8_t OK       = 0x00;  ///< re-armed (voltage rose back above hysteresis margin)
        constexpr uint8_t LOW      = 0x01;
        constexpr uint8_t CRITICAL = 0x02;
    }

    /// BATTERY_INFO_RESP flag bits.
    namespace BatteryFlags {
        constexpr uint8_t PRESENT          = 0x01;  ///< board has a battery sensor + reading is plausible
        constexpr uint8_t LOW_TRIGGERED    = 0x02;
        constexpr uint8_t CRITICAL_TRIGGERED = 0x04;
        constexpr uint8_t MANUAL_CELL_COUNT = 0x08;  ///< cellCount was explicitly set vs auto-detected
        constexpr uint8_t USB_POWERED       = 0x10;  ///< only meaningful on AdcDividerBatteryT-equipped boards
    }

    /// Universal port ID — `(kind:3 << 5) | (idx:5)` byte.  Lets a
    /// single byte identify any component on the slave for
    /// cross-cutting use (cross-collection events, stall traces,
    /// log entries) without a separate `(kind, idx)` tuple.  Limits:
    /// 8 component kinds, 32 channels per kind.
    namespace PortId {
        enum Kind : uint8_t {
            None      = 0,
            Servo     = 1,
            Pwm       = 2,
            LedDed    = 3,   ///< dedicated LED channel
            LedPwm    = 4,   ///< PWM-borrowed LED channel
            // 5..7 reserved
        };
        constexpr uint8_t KIND_MASK   = 0xE0;
        constexpr uint8_t INDEX_MASK  = 0x1F;
        constexpr uint8_t make(Kind k, uint8_t idx) { return (uint8_t)((k << 5) | (idx & INDEX_MASK)); }
        constexpr Kind    kind  (uint8_t pid) { return (Kind)((pid & KIND_MASK) >> 5); }
        constexpr uint8_t index (uint8_t pid) { return pid & INDEX_MASK; }
        constexpr bool    isLed (uint8_t pid) {
            const Kind k = kind(pid);
            return k == LedDed || k == LedPwm;
        }
    }

    // ── Servo collection ──────────────────────────────────────────────
    //
    // Full intelligent-servo control surface — the slave's local
    // motion-profile engine (`ServoControl` in sfx_peripherals/servo/)
    // owns trapezoidal profiling, soft limits, and a "jerk offset"
    // mechanism for transient position shocks (recoil kicks, vibration
    // effects).  Master commands a target; slave drives the smooth
    // curve to it and emits SERVO_TARGET_REACHED on convergence.
    //
    //                         pulse_us
    //                            ▲
    //           target───────────│───────────────────
    //                           ╱│
    //                         ╱  │── decel phase
    //                       ╱    │
    //                cruise─     │
    //              ╱             │
    //            ╱── accel phase │
    //  start ──╱                 │
    //          └──────────────────────────────► time
    //
    // Jerk = transient offset added to the current target for a bounded
    // duration (e.g. +200 µs for 80 ms = recoil kick, then decays back
    // through the normal profile).  Multiple jerks compose additively.
    // 0x10..0x2F — 32 IDs of growth room
    constexpr uint8_t SERVO_SET               = 0x10;  ///< [idx:u8][position_us:u16LE]
    constexpr uint8_t SERVO_CONFIG            = 0x11;  ///< [idx:u8][min_us:u16][max_us:u16][center_us:u16][maxSpeed:u16][accel:u16][decel:u16]
    constexpr uint8_t SERVO_QUERY             = 0x12;  ///< [idx:u8] → SERVO_QUERY_RESP
    constexpr uint8_t SERVO_QUERY_RESP        = 0x13;  ///< [idx:u8][position_us:u16][target_us:u16][velocity:i16][flags:u8]
    /// Async (TAG_ASYNC) — emitted exactly once when a servo's
    /// trapezoidal profile converges on the target last set by
    /// SERVO_SET.  See refactor plan §"Async completion events".
    constexpr uint8_t SERVO_TARGET_REACHED    = 0x14;  ///< [idx:u8][position_us:u16LE]
    /// Apply a jerk offset to the current target.  The slave adds
    /// `offset_us` (signed) to the running target for `duration_ms`
    /// then unwinds it back through the trapezoidal profile.  Use for
    /// recoil kicks, idle vibration, brief lurches.  Multiple
    /// in-flight jerks compose additively.
    constexpr uint8_t SERVO_APPLY_JERK        = 0x15;  ///< [idx:u8][offset_us:i16LE][duration_ms:u16LE]
    /// Set just the motion-profile parameters (max speed / accel /
    /// decel) without touching range — cheaper than full SERVO_CONFIG
    /// when only the dynamics need tuning at runtime.
    constexpr uint8_t SERVO_SET_MOTION        = 0x16;  ///< [idx:u8][maxSpeed:u16LE][accel:u16LE][decel:u16LE]
    /// Soft-disable / re-enable PWM output — slave keeps internal
    /// target tracking but stops driving the pin.  Useful for
    /// low-power hold (gear deployed + locked, brake engaged) and
    /// for diagnostics / hand-positioning during bench setup.
    constexpr uint8_t SERVO_HOLD              = 0x17;  ///< [idx:u8][hold:u8 (0=release, 1=hold off)]
    /// Async (TAG_ASYNC) — emitted periodically while one or more
    /// servos are actively profiling (velocity ≠ 0, target not yet
    /// reached).  Default rate when enabled = 10 Hz; emission stops
    /// per-channel as each profile converges (then SERVO_TARGET_REACHED
    /// fires).  Master uses these for live position tracking during
    /// gear-cycle or recoil sequences without polling SERVO_QUERY.
    constexpr uint8_t SERVO_MOTION_UPDATE     = 0x18;  ///< [idx:u8][pos:u16LE][target:u16LE][vel:i16LE]
    /// Toggle SERVO_MOTION_UPDATE emission.  Disabled by default
    /// (master enables before sequences that benefit from progress
    /// tracking, disables after).  `rate_hz` 0 = use default 10 Hz.
    constexpr uint8_t SERVO_MOTION_UPDATES    = 0x19;  ///< [enable:u8][rate_hz:u8]
    // 0x1A..0x2F reserved for servo growth

    // ── PWM collection (mode-mutable channels) ────────────────────────
    //
    // Two complementary tools for reconfiguring a port at runtime:
    //
    //   PWM_SET_MODE / PWM_SET_FREQ  — incremental, "change one thing".
    //                                  Cheap, atomic per call.
    //   PWM_RECONFIGURE              — atomic full-config swap; pass
    //                                  mode + frequency + polarity +
    //                                  soft duty limit in a single
    //                                  packet so the channel never sees
    //                                  an inconsistent mid-state when
    //                                  multiple parameters change at
    //                                  once (e.g., flipping a generic
    //                                  PWM LED-pin into a motor pin
    //                                  requires both mode and freq).
    //
    // Switching INTO PwmLed:  any prior duty drops to 0; LedCollection
    //                         adopts the channel; no LED program is
    //                         running on it until LED_PROGRAM_RUN.
    // Switching OUT of PwmLed: any running LED program is stopped (no
    //                         LED_PROGRAM_DONE event — switch is
    //                         master-initiated, not natural completion);
    //                         duty drops to 0; LedCollection drops the
    //                         extension output reference.
    // 0x30..0x4F — 32 IDs
    constexpr uint8_t PWM_SET_MODE            = 0x30;  ///< [idx:u8][mode:u8 ComponentKind]
    constexpr uint8_t PWM_SET_DUTY            = 0x31;  ///< [idx:u8][duty:u16LE]   (0..1000 = 0..100.0%)
    constexpr uint8_t PWM_SET_MOTOR           = 0x32;  ///< [idx:u8][speed:i16LE]  (-1000..1000)
    constexpr uint8_t PWM_SET_HEATER          = 0x33;  ///< [idx:u8][duty:u16LE | targetTempC:u16LE based on flags]
    constexpr uint8_t PWM_SET_FREQ            = 0x34;  ///< [idx:u8][freq_Hz:u16LE]
    constexpr uint8_t PWM_QUERY               = 0x35;  ///< [idx:u8] → PWM_QUERY_RESP
    constexpr uint8_t PWM_QUERY_RESP          = 0x36;  ///< [idx:u8][mode:u8][duty:u16][freq_Hz:u16][voltage_mV:i32][current_mA:i32]
    constexpr uint8_t PWM_RECONFIGURE         = 0x37;  ///< [idx:u8][mode:u8][freq_Hz:u16LE][cfgFlags:u8][maxDuty:u16LE]   (atomic)
    constexpr uint8_t PWM_GET_CONFIG          = 0x38;  ///< [idx:u8] → PWM_GET_CONFIG_RESP
    constexpr uint8_t PWM_GET_CONFIG_RESP     = 0x39;  ///< [idx:u8][mode:u8][freq_Hz:u16LE][cfgFlags:u8][maxDuty:u16LE][hwFlags:u8][vSense:u8][cSense:u8][pairedWith:u8]

    // Stall guard — for PwmMotor channels with current sensing.  The
    // slave watches the channel's current draw on its update tick;
    // when it exceeds `threshold_mA` continuously for `debounce_ms`,
    // the slave optionally stops the motor (per StallFlags) and emits
    // PWM_STALL as an async event.  Combined with PWM_SET_MOTOR (back
    // and forth via signed speed) this lets the master implement
    // open-loop endpoint calibration: drive at a constant speed until
    // PWM_STALL fires; the wall-clock between command and stall is
    // the travel time for that direction.  See refactor plan
    // §"Motor calibration via stall detection".
    constexpr uint8_t PWM_SET_STALL_GUARD     = 0x3A;  ///< [idx:u8][threshold_mA:u16LE][debounce_ms:u8][stallFlags:u8]
    constexpr uint8_t PWM_CLEAR_STALL         = 0x3B;  ///< [idx:u8]   — re-arm latched channel
    /// Async (TAG_ASYNC) — emitted when a stall guard trips.
    constexpr uint8_t PWM_STALL               = 0x3C;  ///< [idx:u8][peak_current_mA:u16LE][duration_ms:u16LE]
    // 0x3D..0x4F reserved for PWM growth

    /// PWM_SET_STALL_GUARD `stallFlags` byte.
    namespace StallFlags {
        /// Master switch.  When clear, the slave does not run the
        /// stall watchdog at all on this channel — current is still
        /// sampled for PWM_QUERY but never compared against a
        /// threshold.  Allows the master to keep the threshold +
        /// debounce config loaded (so re-enable doesn't need a
        /// fresh PWM_SET_STALL_GUARD round-trip) while the actuator
        /// is in a state where stall is expected (e.g. mid-
        /// calibration during the second-direction sweep, where the
        /// first sweep's safety margin doesn't yet apply).
        constexpr uint8_t ENABLED      = 0x01;
        /// On stall, slave commands PWM_SET_MOTOR(0) immediately.
        /// Independent of LATCH — auto-stopped channels can be
        /// re-driven without clearing if LATCH isn't also set.
        constexpr uint8_t AUTO_STOP    = 0x02;
        /// If AUTO_STOP, brake (both H-bridge halves LOW) instead of
        /// coast.  No-op without AUTO_STOP.
        constexpr uint8_t BRAKE_ON_STOP= 0x04;
        /// Channel ignores motor / duty commands (other than 0)
        /// until PWM_CLEAR_STALL.  Use for deliberate "stop and
        /// wait for human / supervisor decision" behaviour.
        constexpr uint8_t LATCH        = 0x08;
        // 0x10..0x80 reserved
    }

    /// PWM_RECONFIGURE / PWM_GET_CONFIG_RESP `cfgFlags` byte.  Runtime-
    /// mutable per-channel behaviour; distinct from PwmFlags (which are
    /// hardware capability bits set at compile time and never change).
    namespace PwmConfigFlags {
        constexpr uint8_t INVERT_OUTPUT      = 0x01;  ///< invert the PWM signal at the pin (inverted polarity, e.g., for active-low MOSFET driver topologies)
        constexpr uint8_t DC_BRAKE_ON_STOP   = 0x02;  ///< PwmMotor only: brake (both bridge halves LOW) on speed=0; default = coast
        constexpr uint8_t HEATER_BANG_BANG   = 0x04;  ///< PwmHeater only: thermistor closed-loop bang-bang; default = open-loop duty
        // bits 0x08..0x80 reserved
    }

    // ── LED collection (event-sequencer runtime) ──────────────────────
    //
    // LED-runtime addressing — one byte spans both dedicated and PWM-
    // borrowed outputs.  See instructions/15-GENERIC-SLAVE-REFACTOR.md
    // § "LED runtime ownership":
    //
    //     bit 7 = 0  →  dedicated LED (LedCollection)  idx 0..(K-1)
    //     bit 7 = 1  →  PWM-borrowed LED (PwmCollection in PwmLed mode)
    //                   idx 0..(M-1) within the PwmCollection
    //
    // A single LED-protocol command set targets both — the master
    // doesn't need to know where the physical output sits.
    // 0x50..0x7F — 48 IDs (LED block has the most growth headroom)
    constexpr uint8_t LED_SET_BRIGHTNESS      = 0x50;  ///< [addr:u8][brightness:u8]   (0=off, 255=full)
    constexpr uint8_t LED_PROGRAM_LOAD        = 0x51;  ///< [addr:u8][progId:u8][eventCount:u8][LedEvent×N]
    constexpr uint8_t LED_PROGRAM_RUN         = 0x52;  ///< [addr:u8][progId:u8][flags:u8]   (flags = repeat | sync-start)
    constexpr uint8_t LED_PROGRAM_STOP        = 0x53;  ///< [addr:u8]
    constexpr uint8_t LED_QUERY               = 0x54;  ///< [addr:u8] → LED_QUERY_RESP
    constexpr uint8_t LED_QUERY_RESP          = 0x55;  ///< [addr:u8][brightness:u8][progId:u8][progState:u8]
    /// Async (TAG_ASYNC) — emitted when a non-repeating program
    /// finishes its last event.  Looped programs (REPEAT bit set in
    /// LED_PROGRAM_RUN flags) never emit this.
    constexpr uint8_t LED_PROGRAM_DONE        = 0x56;  ///< [addr:u8][progId:u8]
    /// Restart the currently-loaded program from event 0 — equivalent
    /// to LED_PROGRAM_STOP + LED_PROGRAM_RUN with the same flags, but
    /// avoids the round-trip and the brief darkness between stop+run.
    constexpr uint8_t LED_PROGRAM_RESTART     = 0x57;  ///< [addr:u8]
    /// Full channel reset — stop + clear queue + brightness 0 +
    /// re-enable.  Hard-resets the channel to its post-attach state.
    /// Address byte addresses dedicated/PWM-borrowed identically; use
    /// addr=0xFF to broadcast to every channel on the slave.
    constexpr uint8_t LED_RESET_CHANNEL       = 0x58;  ///< [addr:u8]
    /// Enable / disable a channel.  Disabled channels:
    ///   - ignore LED_SET_BRIGHTNESS / LED_PROGRAM_RUN (return ACK
    ///     with no effect; status byte reflects disabled flag)
    ///   - emit no output (forced LOW)
    /// Use to gate channels off without losing their loaded program.
    constexpr uint8_t LED_ENABLE_CHANNEL      = 0x59;  ///< [addr:u8][enable:u8]
    /// Master brightness percentage 0..100 — multiplicative scaler
    /// applied to every channel's emitted brightness.  Affects
    /// dedicated and PWM-borrowed channels uniformly.  Useful for
    /// dim/bright modes without re-uploading programs.
    constexpr uint8_t LED_SET_MASTER_BRIGHTNESS = 0x5A;  ///< [percent:u8]
    /// Detailed sequence-status query — returns the LedManager's
    /// LightFxSeqStatus structure (current event index, total events,
    /// current event type, event start_ms, repeat count, flags).
    /// Useful for Studio to render "program: event 3 of 7, fade-in
    /// in progress, 250ms remaining".
    constexpr uint8_t LED_SEQ_STATUS_REQ      = 0x5B;  ///< [addr:u8] → LED_SEQ_STATUS_RESP
    constexpr uint8_t LED_SEQ_STATUS_RESP     = 0x5C;  ///< [addr:u8][LightFxSeqStatus]
    // 0x5D..0x7F reserved for LED growth (48 IDs total in this block)

    /// Helpers for assembling/decoding the LED address byte.
    namespace LedAddr {
        constexpr uint8_t PWM_BORROWED_BIT = 0x80;
        constexpr uint8_t INDEX_MASK       = 0x7F;
        constexpr uint8_t dedicated   (uint8_t idx) { return idx & INDEX_MASK; }
        constexpr uint8_t pwmBorrowed (uint8_t idx) { return PWM_BORROWED_BIT | (idx & INDEX_MASK); }
        constexpr bool    isPwmBorrowed(uint8_t addr) { return (addr & PWM_BORROWED_BIT) != 0; }
        constexpr uint8_t indexOf      (uint8_t addr) { return addr & INDEX_MASK; }
    }

    /// LED_PROGRAM_RUN flag bits.
    namespace LedProgramFlags {
        constexpr uint8_t REPEAT       = 0x01;  ///< loop forever; never emits LED_PROGRAM_DONE
        constexpr uint8_t SYNC_START   = 0x02;  ///< defer start until next RUN — lock-step multi-channel start
        // bits 0x04..0x80 reserved
    }

    /// Per-event wire format for LED_PROGRAM_LOAD.  An LED program is
    /// a sequence of these events; the slave's LedEventSeq runtime
    /// walks them in order, executing each before advancing.  When
    /// the last event finishes:
    ///   - if the program was started with `LedProgramFlags::REPEAT`,
    ///     the runtime restarts at event 0 (loops forever)
    ///   - if NOT repeating, the runtime emits LED_PROGRAM_DONE and
    ///     leaves the channel at the final brightness
    /// A "terminal hold" effect — keep the channel at brightness X
    /// after the program — is achieved by ending with an `ON` event
    /// with `duration_ms = 0`, which never completes (sequence stops
    /// advancing without looping; LedManager treats duration=0 as
    /// indefinite).  Mirrors the behaviour of the existing
    /// `sfx_peripherals/led/led_events.h` event classes (LedOn,
    /// LedOff, LedFlashing, LedFadeIn, LedFadeOut, LedFading,
    /// LedBeacon).  Wire size: 8 bytes per event.
    ///
    ///     [type:u8][p1:u16LE][p2:u16LE][p3:u8][p4:u8][p5:u8]
    ///
    /// The meaning of p1..p5 depends on `type` — see LedEventType.
    namespace LedEventType {
        /// ON — solid brightness for a duration (0 = indefinite hold).
        ///     p1 = duration_ms low,    p2 = duration_ms high
        ///     p3 = brightness 0..100,  p4 = power-saving flag,
        ///     p5 = power-saving PWM duty 0..100
        constexpr uint8_t ON       = 0;
        /// OFF — fully off for a duration.
        ///     p1 = duration_ms low, p2 = duration_ms high
        constexpr uint8_t OFF      = 1;
        /// FLASHING — square-wave on/off at a period for a duration.
        ///     p1 = period_ms,
        ///     p2 = duration_ms (0 = indefinite),
        ///     p3 = brightness 0..100
        constexpr uint8_t FLASHING = 2;
        /// FADE_IN — ramp 0 → brightness over duration.
        ///     p1 = duration_ms low, p2 = duration_ms high,
        ///     p3 = target brightness 0..100
        constexpr uint8_t FADE_IN  = 3;
        /// FADE_OUT — ramp current → 0 over duration.
        ///     p1 = duration_ms low, p2 = duration_ms high
        constexpr uint8_t FADE_OUT = 4;
        /// FADING — sinusoidal breath at a period for a duration.
        ///     p1 = period_ms,
        ///     p2 = duration_ms (0 = indefinite),
        ///     p3 = peak brightness 0..100,
        ///     p4 = trough brightness 0..100
        constexpr uint8_t FADING   = 5;
        /// BEACON — short flash + extended-off cycle (rotating beacon).
        ///     p1 = flash_ms,
        ///     p2 = off_ms,
        ///     p3 = brightness 0..100,
        ///     p4 = repeat-count (0 = forever)
        constexpr uint8_t BEACON   = 6;
        // 7..255 reserved for future event types
    }

    // ── Status broadcast (slave → master/PC) ──────────────────────────
    //
    // Periodic unified status — one packet bundles the live state of
    // every servo, every PWM channel, every LED, plus board-level
    // counters.  Saves the master from polling N per-component query
    // packets just to refresh its mirror.  Emitted on a schedule
    // configurable via `SLAVE_STATUS_RATE` (default 1 Hz; up to 10 Hz
    // for fast-motion debugging).  Tag = TAG_ASYNC.
    //
    //   [hdr: boardState:u8, mode:u8, uptime_ms:u32LE, freeRam:u32LE]
    //   [servoCount:u8] × { port_id:u8, pos:u16LE, target:u16LE, vel:i16LE, flags:u8 }
    //   [pwmCount:u8]   × { port_id:u8, mode:u8, duty:u16LE, voltage_mV:i16LE, current_mA:i16LE,
    //                       stallFlags:u8, peak_mA:u16LE }
    //   [ledCount:u8]   × { port_id:u8, brightness:u8, progState:u8, progId:u8 }
    //   [batteryPresent:u8]  // 0 = no battery on this board → no further bytes
    //     if present:  { chemistry:u8, cellCount:u8, voltage_mV:u16LE,
    //                    cellVoltage_mV:u16LE, percentage:u8, flags:u8 }
    //
    // Per-component sub-blocks shrink to size 0 if the slave has no
    // components of that kind (LED-only board emits empty servo and
    // PWM blocks; servo-only board emits empty LED block).  Battery
    // section is one byte (`0`) on boards without a battery.  Total
    // size is bounded by the COBS payload limit (512 bytes), which
    // comfortably covers the 6+8+8 worst case.
}   // namespace SlavePacket

namespace SlaveError {
    // Generic / addressing
    constexpr uint8_t NONE                    = 0x00;
    constexpr uint8_t INVALID_INDEX           = 0xA0;  ///< component idx out of range for the board's collection
    constexpr uint8_t WRONG_COMPONENT_KIND    = 0xA1;  ///< e.g. SERVO_SET against a PWM index
    constexpr uint8_t MODE_NOT_SUPPORTED      = 0xA2;  ///< PWM mode change rejected (flags don't allow)
    constexpr uint8_t SENSING_UNAVAILABLE     = 0xA3;  ///< query for sense data on a channel without sensing
    // Identifier
    constexpr uint8_t IDENT_TOO_LONG          = 0xA4;  ///< IDENT_SET payload exceeds MAX_LEN
    constexpr uint8_t IDENT_PERSIST_FAILED    = 0xA5;  ///< flash write failed (out of space / FS error)
    constexpr uint8_t IDENT_INVALID_CHARS     = 0xA6;  ///< only printable ASCII allowed
    // Lifecycle
    constexpr uint8_t NOT_INITIALISED         = 0xA7;  ///< action attempted before INIT(SLAVE)/INIT(DIRECT)
    // LED programs
    constexpr uint8_t PROGRAM_TOO_LARGE       = 0xA8;  ///< LED_PROGRAM_LOAD exceeds per-channel slot
    constexpr uint8_t INVALID_PROGRAM_ID      = 0xA9;
    // Battery
    constexpr uint8_t BATTERY_NOT_PRESENT     = 0xAA;  ///< BATTERY_INFO_REQ / BATTERY_RECONFIGURE on a board with NoBattery
    constexpr uint8_t BATTERY_INVALID_CHEMISTRY = 0xAB; ///< chemistry byte outside the BatteryChemistry enum
}

#endif  // SFX_SLAVE_PROTOCOL_H
