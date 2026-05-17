/*
 * PwmCollection<N> — N PWM outputs with runtime-mutable mode + optional sensing.
 *
 * Each channel can act as one of:
 *   - PwmGeneric: raw duty 0..1.0
 *   - PwmLed:     channel is **claimed by the LedCollection's event-
 *                 sequence runtime** — this PwmCollection stops driving
 *                 the duty directly; the LED runtime emits brightness
 *                 ticks into the channel via an internal adapter.  See
 *                 instructions/15-GENERIC-EXPANDER-REFACTOR.md §
 *                 "LED runtime ownership".  PWM_SET_DUTY against a
 *                 PwmLed channel returns ComponentError::WRONG_COMPONENT_KIND;
 *                 use LED_SET_BRIGHTNESS / LED_QUEUE_START with the
 *                 PWM-borrowed addressing form (bit 7 of address byte
 *                 set, idx = PWM channel index) instead.
 *   - PwmMotor:   signed speed (-1.0..+1.0); requires either a single pin
 *                 (uni-directional) or a paired channel for H-bridge
 *   - PwmHeater:  duty + optional thermistor feedback (closed-loop temp)
 *
 * The mode is chosen at runtime via PWM_SET_MODE (subject to the
 * `MODE_MUTABLE` capability flag).  Switching INTO PwmLed registers
 * the channel with the bound LedCollection; switching OUT of PwmLed
 * stops any running program on that channel and returns it to direct
 * PWM control.  Channels with `IS_BIDIR_PAIR` are coupled to their
 * immediate neighbour so the firmware can drive an H-bridge without
 * exposing the pairing detail to the protocol.
 *
 * Optional sensing channels reference an INA226 (or analog-ADC) index
 * known to the board.  The collection holds the indices but doesn't own
 * the sensor instance — the board firmware passes a pointer to its
 * sensing array at attach() time.
 */

#ifndef SFX_PWM_COLLECTION_H
#define SFX_PWM_COLLECTION_H

#include <array>
#include <cstdint>

#include <functional>

#include <sfx_peripherals/motor/dc_motor.h>
#include <sfx_peripherals/motor/stall_detector.h>

#include "component_event.h"
#include <serial/components/component_kind.h>
#include "sense_policy.h"

namespace sfx_peripherals {

using sfx_peripherals::DcMotor;
using sfx_peripherals::MotorTopology;
using sfx_peripherals::StallDetector;

/// Per-channel HARDWARE descriptor.  Compile-time on the board firmware
/// side — describes wiring + capabilities the silicon was laid out for.
/// Never changes at runtime.  The values seed `PwmRuntimeConfig` at
/// attach time and feed `ComponentInfo::flags` for COMPONENT_LIST_RESP.
struct PwmSpec {
    uint8_t  pin;                ///< primary GPIO or expander pin (PWM output)
    bool     useExpander;
    uint16_t defaultFreq_Hz;     ///< 24..40000 typical — initial PwmRuntimeConfig.freq_Hz
    uint8_t  hwFlags;            ///< PwmFlags::* — capability bits (HW_PWM, MODE_MUTABLE, sensing, etc.)
    ComponentKind defaultMode;   ///< boot mode — what this channel does until master reconfigures it
    uint8_t  voltageSenseIdx;    ///< 0xFF = none; otherwise INA226 channel
    uint8_t  currentSenseIdx;    ///< 0xFF = none
    uint8_t  pairedWith;         ///< 0xFF = unpaired; otherwise PwmCollection idx (must be neighbour for H-bridge)

    // ── Optional motor wiring (PwmMotor mode) ─────────────────────────
    //
    // When the channel can switch to `PwmMotor`, this struct describes
    // the H-bridge topology to the embedded `DcMotor` driver.  Default-
    // initialised values (topology=SinglePinPwm, all-zero pins) yield a
    // uni-directional motor on `pin` only — the typical case for a
    // smoke heater or fan.  Boards with H-bridges fill in the relevant
    // fields.
    MotorTopology motorTopology = MotorTopology::SinglePinPwm;
    uint8_t       motorCwPin    = 0;   ///< HBridgeDualGpio + HBridgePwmDir (dir pin)
    uint8_t       motorCcwPin   = 0;   ///< HBridgeDualGpio only
    bool          motorInvertDir   = false;
    bool          motorBrakeCapable = false;
};

/// Per-channel RUNTIME descriptor.  Mutable via PWM_RECONFIGURE (atomic
/// full swap) or via individual PWM_SET_MODE / PWM_SET_FREQ commands.
/// Returned to the master verbatim by PWM_GET_CONFIG_RESP.
struct PwmRuntimeConfig {
    ComponentKind mode;          ///< current ComponentKind (PwmGeneric/PwmLed/PwmMotor/PwmHeater)
    uint16_t      freq_Hz;       ///< active PWM frequency
    uint8_t       cfgFlags;      ///< PwmConfigFlags::* — invert, brake-on-stop, bang-bang, etc.
    uint16_t      maxDuty;       ///< soft upper limit; clamps PWM_SET_DUTY / motor / heater output (0..1000)
};

/// `PwmCollection<N, TSense>` — N PWM outputs.  Templated on the
/// board-specific `SensePolicy` that owns the concrete sensor
/// instances and routes per-channel voltage / current reads to them.
/// Default `NoSensing` is a zero-overhead struct for boards without
/// any per-channel sensing.  No virtual dispatch.
///
/// PwmCollection exposes `currentMode(idx)` and `writeDuty(idx, duty)`
/// so that `PwmDutyAdapter<PwmCollection<...>>` can wrap it as a
/// `PwmOutput` backend.  That adapter is what `LedRuntime` templates on
/// to drive PWM-borrowed LED slots; the cross-coupling lives in the
/// adapter, not in this class.
template <size_t N, SensePolicy TSense = NoSensing>
class PwmCollection {
public:
    static constexpr size_t COUNT = N;

    void configure(const std::array<PwmSpec, N>& specs) { _specs = specs; }

    /// Mutable access to the embedded sense policy — the board
    /// firmware initialises sensors via this reference (e.g.,
    /// `pwms.sense().smokeHeaterSense.begin(Wire, 0x40);`).  No
    /// pointer-array binding needed; the policy struct lives inline
    /// in PwmCollection for cache-friendliness.
    TSense&       sense()       { return _sense; }
    const TSense& sense() const { return _sense; }

    bool attach();
    void detach();
    void update();

    /// Stop all outputs — duty 0, motors stopped, heaters off.  Mode
    /// flags are preserved (a channel in PwmLed mode stays in PwmLed
    /// mode, but its duty drops to 0).  Invoked from
    /// `CoreServer::enterSafeState()`.
    void allOff();

    // ── Mode + value setters (incremental) ────────────────────────────

    /// Switch a channel to a different ComponentKind.  Rejected if the
    /// channel's PwmFlags don't include MODE_MUTABLE, or if the requested
    /// kind is not a PWM-family kind (PwmGeneric/PwmLed/PwmMotor/PwmHeater).
    /// Side effects on transition:
    ///   - any prior duty drops to 0 (motor stops, heater off, generic 0%)
    ///   - leaving PwmLed: any running LED program on the channel is
    ///     stopped (no LED_QUEUE_DONE event — switch is master-initiated,
    ///     not natural completion); LedCollection drops the extension ref
    ///   - entering PwmLed: LedCollection adopts the channel; no program
    ///     is running on it until a subsequent LED_QUEUE_START
    bool setMode      (uint8_t idx, ComponentKind newKind);
    bool setDuty      (uint8_t idx, uint16_t duty_thousandths);   ///< 0..1000
    bool setMotor     (uint8_t idx, int16_t  speed_signed);       ///< -1000..1000
    bool setHeater    (uint8_t idx, uint16_t value_or_targetTemp);
    bool setFrequency (uint8_t idx, uint16_t freq_Hz);

    /// Atomic full-config swap.  Equivalent to setMode + setFrequency +
    /// runtime-flag update applied in one shot — the channel never
    /// observes a half-applied config (e.g., new mode running at the
    /// previous frequency for one tick).  Invoked by PWM_RECONFIGURE.
    /// Returns false on any of:
    ///   - idx out of range
    ///   - hw flags don't allow MODE_MUTABLE for the requested mode
    ///   - paired-mode constraints violated
    bool reconfigure(uint8_t idx, const PwmRuntimeConfig& cfg);

    // ── Stall guard (PwmMotor + current sensing) ──────────────────────
    //
    // Detection state machine is delegated to `StallDetector` (one
    // instance per channel — see _detectors below).  The collection
    // only owns the protocol-level policy: which flags are set, latch
    // state, peak observed.

    struct StallGuard {
        uint16_t threshold_mA  = 0;   ///< 0 = guard disabled (alongside !ENABLED flag)
        uint8_t  debounce_ms   = 50;  ///< maps to StallDetector::Config.stallConfirm_ms
        uint8_t  flags         = 0;   ///< StallFlags::* bitmask
        uint16_t peak_mA       = 0;
        bool     latched       = false;
    };

    /// Configure the stall guard for a PwmMotor channel.  When the
    /// channel's current draw stays above `threshold_mA` for at least
    /// `debounce_ms`, `update()` fires the registered StallCb (which
    /// CoreServer turns into a PWM_STALL async packet) and, if
    /// `StallFlags::AUTO_STOP` is set, drops the motor to 0.  If
    /// `LATCH` is set the channel rejects subsequent setDuty/setMotor
    /// commands until `clearStall(idx)`.
    bool setStallGuard(uint8_t idx, uint16_t threshold_mA,
                       uint8_t debounce_ms, uint8_t flags);

    /// Re-arm a latched stall guard.  Returns false if idx out of
    /// range; ACKs even when the channel wasn't latched (idempotent).
    bool clearStall(uint8_t idx);

    /// Read the current stall-guard mirror for a channel.  Used by the
    /// CoreServer's unified status payload — exposes (flags, peak_mA,
    /// latched) so the master can render motor health without polling
    /// PWM_GET_CONFIG.  Returns false on out-of-range idx; out-params
    /// are zeroed in that case.
    ///
    /// `flags` byte layout:
    ///   bits 0..3 → mirror of the configured `StallFlags::*`
    ///   bit  6    → 0x40 — channel currently latched (LATCH + tripped)
    bool getStallStatus(uint8_t idx,
                        uint8_t&  out_flags,
                        uint16_t& out_peak_mA) const {
        out_flags   = 0;
        out_peak_mA = 0;
        if (idx >= N) return false;
        const auto& g = _stallGuards[idx];
        out_flags   = (uint8_t)(g.flags & 0x0F);
        if (g.latched) out_flags |= 0x40;   ///< latched bit (distinct from the configured LATCH flag bit)
        out_peak_mA = g.peak_mA;
        return true;
    }

    /// Async stall callback — invoked from update() when a guard trips.
    using StallCb = std::function<void(uint8_t idx, uint16_t peak_mA, uint16_t duration_ms)>;
    void setStallCallback(StallCb cb) { _onStall = std::move(cb); }

    /// Board-local event callback — board firmware hooks component
    /// state transitions (mode change, motion start/end, stall, etc.)
    /// to indicator LEDs / buzzers / diagnostic logging.  See
    /// `component_event.h`.  Not part of the slave protocol.
    void setEventCallback(ComponentEventCb cb) { _onEvent = std::move(cb); }

    // ── Query ─────────────────────────────────────────────────────────

    ComponentKind currentMode  (uint8_t idx) const;

    /// Full runtime config readback — feeds PWM_GET_CONFIG_RESP.
    bool getRuntimeConfig(uint8_t idx, PwmRuntimeConfig& out) const;

    /// Hardware flags (compile-time PwmFlags::*).  Returned as the
    /// `hwFlags` byte in PWM_GET_CONFIG_RESP.
    uint8_t hwFlags(uint8_t idx) const { return idx < N ? _specs[idx].hwFlags : 0; }

    bool query (uint8_t idx,
                ComponentKind& out_mode,
                uint16_t&      out_duty,
                uint16_t&      out_freq_Hz,
                int32_t&       out_voltage_mV,
                int32_t&       out_current_mA) const;

    // ── PwmDutyAdapter surface ────────────────────────────────────────
    //
    // `PwmDutyAdapter<PwmCollection<N, TSense>>` wraps this class as a
    // `PwmOutput` backend, calling `writeDuty()` to drive PWM-borrowed
    // LED slots.  `currentMode()` lets the wire dispatcher gate LED
    // commands on the channel actually being in `PwmLed` mode.

    uint8_t pwmChannelCount() const { return (uint8_t)N; }
    void    writeDuty(uint8_t idx, uint16_t duty_thousandths);

    // ── Component enumeration ─────────────────────────────────────────

    ComponentInfo describe(size_t i) const {
        return ComponentInfo{
            .index    = (uint8_t)i,
            .kind     = _runtime[i].mode,
            .flags    = _specs[i].hwFlags,
            .reserved = 0,
        };
    }

private:
    std::array<PwmSpec, N>          _specs{};
    std::array<PwmRuntimeConfig, N> _runtime{};      ///< runtime-mutable per-channel config
    std::array<uint16_t, N>         _duties{};       ///< last-written duty (thousandths)
    std::array<int16_t,  N>         _motorSpeeds{};
    std::array<StallGuard, N>       _stallGuards{};   ///< per-channel protocol policy (flags, latch, peak)
    std::array<StallDetector, N>    _stallDetectors{};///< per-channel detection state machine (delegated)
    std::array<DcMotor, N>          _motors{};        ///< per-channel DC motor driver (active when channel mode is PwmMotor)
    StallCb                         _onStall;         ///< fires on guard trip (CoreServer → PWM_STALL packet)
    ComponentEventCb                _onEvent;         ///< board-local state-transition hook (indicator LEDs, etc.)
    TSense                          _sense{};         ///< embedded sense policy (zero-size for NoSensing)
    bool                            _attached = false;

    /// Apply runtime config delta + handle side effects (LED runtime
    /// hand-off, motor stop, frequency reconfig).  Used by setMode(),
    /// setFrequency(), reconfigure().
    bool applyRuntimeConfig(uint8_t idx, const PwmRuntimeConfig& newCfg);
};

}  // namespace sfx_peripherals

#endif  // SFX_PWM_COLLECTION_H
