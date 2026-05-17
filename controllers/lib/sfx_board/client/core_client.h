/*
 * CoreClient — typed master-side client for the generic slave protocol.
 *
 * Lives master-side (HubFX firmware + tests).  Wraps `BusClient` with:
 *
 *   - typed methods for every wire packet — payload encoding hidden,
 *     return is `CommandResult` (NEVER bool — Rule 3 in CLAUDE.md)
 *   - typed structs for response payloads (ServoStatus, PwmStatus,
 *     BatteryInfo, SlaveStatus, etc.)
 *   - async-event observer chain — one callback registration per
 *     async packet type; multiple observers can register and they
 *     all fire
 *
 * Every async packet from `serial/slave/slave.h` has a corresponding
 * `on*Cb` registration here:
 *
 *   ┌──────────────────────────────┬─────────────────────────────┐
 *   │ Slave wire packet            │ CoreClient observer        │
 *   ├──────────────────────────────┼─────────────────────────────┤
 *   │ SERVO_TARGET_REACHED  (0x14) │ onServoTargetReached(...)   │
 *   │ SERVO_MOTION_UPDATE   (0x18) │ onServoMotionUpdate(...)    │
 *   │ PWM_STALL             (0x3C) │ onPwmStall(...)             │
 *   │ LED_QUEUE_DONE        (0x56) │ onLedQueueDone(...)         │
 *   │ BATTERY_ALERT         (0x0C) │ onBatteryAlert(...)         │
 *   │ COMPONENT_STATUS_BROADCAST(0x06) │ onStatusBroadcast(...)      │
 *   └──────────────────────────────┴─────────────────────────────┘
 *
 * Calibration progress is NOT a slave event — calibration state lives
 * master-side (per the architectural pivot).  The master orchestrator
 * that drives a calibration sequence emits its own progress events
 * upstream to Studio / CLI; CoreClient just sees the underlying
 * primitives (PWM_SET_MOTOR + PWM_STALL).
 *
 * History:
 *   - Originally landed at controllers/lib/sfx_serial/serial/client/.
 *   - Moved to controllers/lib/sfx_core/client/ 2026-05-06 to mirror
 *     the symmetric server/client layout used by sfx_storage,
 *     sfx_config, and sfx_peripherals/led — sfx_serial keeps the
 *     core protocol primitives (BusClient, CommandResult, packet
 *     headers) and per-module clients live under their own libraries.
 */

#ifndef SFX_CORE_CLIENT_H
#define SFX_CORE_CLIENT_H

#include <cstdint>
#include <functional>
#include <vector>

#include <serial/client/bus_client.h>
#include <serial/components/components.h>
#include <serial/components/led_status.h>
#include <serial/components/component_kind.h>
#include <power/battery_types.h>           // BatteryChemistry mirror

namespace sfx_core {

// ── Response payload structs ─────────────────────────────────────────

struct ServoStatus {
    uint8_t  idx;
    uint16_t pos_us;
    uint16_t target_us;
    int16_t  velocity_us_per_s;
    uint8_t  flags;
};

struct PwmStatus {
    uint8_t                       idx;
    sfx_peripherals::ComponentKind mode;
    uint16_t                      duty_thousandths;
    uint16_t                      freq_Hz;
    int32_t                       voltage_mV;
    int32_t                       current_mA;
};

struct PwmConfig {
    uint8_t                        idx;
    sfx_peripherals::ComponentKind mode;
    uint16_t                       freq_Hz;
    uint8_t                        cfgFlags;     // PwmConfigFlags::*
    uint16_t                       maxDuty;
    uint8_t                        hwFlags;      // PwmFlags::* (capability)
    uint8_t                        voltageSenseIdx;
    uint8_t                        currentSenseIdx;
    uint8_t                        pairedWith;
};

struct LedStatus {
    uint8_t addr;          // raw address byte (bit 7 = PWM-borrowed)
    uint8_t brightness;
    uint8_t queueState;    // 0 = idle, 1 = playing, 2 = paused
    uint8_t currentEvent;  // 0-based event index within the loaded queue
};

struct ServoCalibration {
    uint16_t min_us;
    uint16_t max_us;
    uint16_t center_us;
    uint16_t maxSpeed;
    uint16_t accel;
    uint16_t decel;
};

struct PwmRuntimeConfig {
    sfx_peripherals::ComponentKind mode;
    uint16_t                       freq_Hz;
    uint8_t                        cfgFlags;
    uint16_t                       maxDuty;
};

/// LedEvent (wire-format, 8 bytes) is defined in serial/slave/led_status.h
/// so both client and slave side share one definition.  Pulled into this
/// namespace for ergonomic call sites.
using LedEvent = ComponentPacket::LedEvent;

/// Decoded BATTERY_INFO_RESP.  `present == false` on boards without a
/// battery sensor (TBattery == NoBattery, or sensor below MIN_DETECT_mV).
/// All other fields are zero in that case.
struct BatteryInfo {
    bool             present          = false;
    BatteryChemistry chemistry        = BatteryChemistry::LIPO;
    uint8_t          cellCount        = 0;
    uint16_t         voltage_mV       = 0;
    uint16_t         cellVoltage_mV   = 0;
    uint8_t          percentage       = 0;
    uint8_t          flags            = 0;        ///< ComponentPacket::BatteryFlags::*
    uint16_t         profileLow_mV    = 0;        ///< per-cell low threshold
    uint16_t         profileCritical_mV = 0;      ///< per-cell critical threshold
};

/// Decoded BATTERY_ALERT async packet.
struct BatteryAlert {
    uint8_t  level;        ///< ComponentPacket::BatteryAlertLevel::OK / WARN / CRITICAL
    uint16_t voltage_mV;   ///< pack voltage at the transition
    uint8_t  cellCount;
};

/// Per-PWM stall mirror (carried in COMPONENT_STATUS_BROADCAST).  Decoded
/// here so master orchestrators can read motor health without a
/// PWM_GET_CONFIG round-trip.
struct PwmStallStatus {
    uint8_t  flags;        ///< low nibble = configured StallFlags; bit 0x40 = currently latched
    uint16_t peak_mA;
};

/// Decoded COMPONENT_STATUS_BROADCAST / response.  Sub-vectors are empty
/// when the slave's `kindsBitmask` filter excludes the section.
struct SlaveStatus {
    uint8_t                       boardState;       // BoardState::*
    uint8_t                       initMode;         // BoardMode::SLAVE / DIRECT / 0
    uint32_t                      uptime_ms;
    uint32_t                      freeRam_bytes;
    std::vector<ServoStatus>      servos;
    std::vector<PwmStatus>        pwms;
    std::vector<PwmStallStatus>   pwmStalls;        ///< parallel to pwms[]
    std::vector<LedStatus>        leds;
    BatteryInfo                   battery;          ///< BatteryInfo::present == false on boards w/o battery
};

// ── Observer typedefs ────────────────────────────────────────────────

using ServoTargetReachedCb = std::function<void(uint8_t idx, uint16_t pos_us)>;
using ServoMotionUpdateCb  = std::function<void(uint8_t idx, uint16_t pos, uint16_t target, int16_t vel)>;
using PwmStallCb           = std::function<void(uint8_t idx, uint16_t peak_mA, uint16_t duration_ms)>;
using LedQueueDoneCb       = std::function<void(uint8_t addr)>;
using BatteryAlertCb       = std::function<void(const BatteryAlert&)>;
using StatusBroadcastCb    = std::function<void(const SlaveStatus&)>;

// ── CoreClient ──────────────────────────────────────────────────────

class CoreClient : public BusClient {
public:
    CoreClient() = default;
    ~CoreClient() override = default;

    // ── Identity / enumeration ───────────────────────────────────────

    /// Re-enumerate components — returns the live runtime modes.
    /// `out` is filled with one entry per component on the slave.
    CommandResult requestComponentList(std::vector<sfx_peripherals::ComponentInfo>& out);

    CommandResult getIdentifier(uint8_t& out_boardType, char* out_name, size_t bufLen);
    CommandResult setIdentifier(const char* name);

    /// Synchronous status query — same payload shape as the
    /// broadcast.  `kindsMask` filters which sections appear
    /// (`ComponentPacket::StatusKinds::*`).
    CommandResult requestStatus(SlaveStatus& out, uint8_t kindsMask = 0);

    /// Configure periodic broadcast.  hz=0 disables; otherwise 1..10 Hz.
    CommandResult setStatusRate(uint8_t hz, uint8_t kindsMask = 0);

    // ── Battery (optional — boards without one still answer with present=0)

    /// Read the live battery snapshot.  ALWAYS returns success; the
    /// caller checks `out.present` to distinguish "battery monitor
    /// wired but no pack" from "board has no battery sensor at all".
    /// Master can also gate this on `CoreCapability::BATTERY` before
    /// calling — see `core/core.h`.
    CommandResult requestBatteryInfo(BatteryInfo& out);

    /// Reconfigure the slave's battery model.  `customLow_mV` and
    /// `customCritical_mV` are PER-CELL overrides; pass 0 for either
    /// to use the chemistry's profile default.
    CommandResult batteryReconfigure(BatteryChemistry chemistry,
                                     uint8_t          cellCount,
                                     uint16_t         customLow_mV     = 0,
                                     uint16_t         customCritical_mV = 0);

    // ── Servo ────────────────────────────────────────────────────────

    CommandResult servoSet         (uint8_t idx, uint16_t pulse_us);
    CommandResult servoConfig      (uint8_t idx, const ServoCalibration& cal);
    CommandResult servoSetMotion   (uint8_t idx, uint16_t maxSpeed, uint16_t accel, uint16_t decel);
    CommandResult servoApplyJerk   (uint8_t idx, int16_t offset_us, uint16_t duration_ms);
    CommandResult servoHold        (uint8_t idx, bool hold);
    CommandResult servoQuery       (uint8_t idx, ServoStatus& out);

    /// Toggle SERVO_MOTION_UPDATE emission slave-side.  Master enables
    /// before a sequence that benefits from progress tracking (gear
    /// cycle, recoil), disables after.
    CommandResult servoMotionUpdates(bool enable, uint8_t rate_hz = 0);

    // ── PWM ──────────────────────────────────────────────────────────

    CommandResult pwmSetMode       (uint8_t idx, sfx_peripherals::ComponentKind mode);
    CommandResult pwmSetDuty       (uint8_t idx, uint16_t duty_thousandths);
    CommandResult pwmSetMotor      (uint8_t idx, int16_t speed_signed);
    CommandResult pwmSetHeater     (uint8_t idx, uint16_t value_or_targetTemp);
    CommandResult pwmSetFrequency  (uint8_t idx, uint16_t freq_Hz);
    CommandResult pwmReconfigure   (uint8_t idx, const PwmRuntimeConfig& cfg);
    CommandResult pwmQuery         (uint8_t idx, PwmStatus& out);
    CommandResult pwmGetConfig     (uint8_t idx, PwmConfig& out);

    /// Stall guard — for `PwmMotor`-mode channels with current sensing.
    /// `flags` is `ComponentPacket::StallFlags::*` (must include ENABLED to
    /// activate the watchdog; AUTO_STOP / BRAKE_ON_STOP / LATCH for
    /// behaviour on trip).
    CommandResult pwmSetStallGuard (uint8_t idx, uint16_t threshold_mA,
                                    uint8_t debounce_ms, uint8_t flags);
    CommandResult pwmClearStall    (uint8_t idx);

    // ── LED ──────────────────────────────────────────────────────────
    //
    // Address byte uses bit 7 to address PWM-borrowed channels (bit
    // 7 = 1) vs dedicated LedDigital channels (bit 7 = 0).  Helpers
    // in `ComponentPacket::LedAddr` build / decode them.

    CommandResult ledSetBrightness     (uint8_t addr, uint8_t brightness);
    /// Replace the channel's event queue.  `flags` = `LedQueueFlags::*`
    /// (currently just REPEAT — auto-restart on completion).
    CommandResult ledLoadQueue         (uint8_t addr, uint8_t flags,
                                        const LedEvent* events, size_t count);
    CommandResult ledStartQueue        (uint8_t addr);
    CommandResult ledStopQueue         (uint8_t addr);
    CommandResult ledRestartQueue      (uint8_t addr);
    CommandResult ledResetChannel      (uint8_t addr);   // 0xFF = broadcast
    CommandResult ledEnableChannel     (uint8_t addr, bool enabled);
    CommandResult ledSetMasterBrightness(uint8_t pct);
    CommandResult ledQuery             (uint8_t addr, LedStatus& out);

    // ── Async event observer registration ────────────────────────────
    //
    // Multiple observers per event type are supported — every
    // registered callback fires.

    void onServoTargetReached (ServoTargetReachedCb cb) { _onTargetReached  .push_back(std::move(cb)); }
    void onServoMotionUpdate  (ServoMotionUpdateCb  cb) { _onMotionUpdate   .push_back(std::move(cb)); }
    void onPwmStall           (PwmStallCb           cb) { _onPwmStall       .push_back(std::move(cb)); }
    void onLedQueueDone       (LedQueueDoneCb       cb) { _onLedQueueDone   .push_back(std::move(cb)); }
    void onBatteryAlert       (BatteryAlertCb       cb) { _onBatteryAlert   .push_back(std::move(cb)); }
    void onStatusBroadcast    (StatusBroadcastCb    cb) { _onStatusBroadcast.push_back(std::move(cb)); }

protected:
    /// BusClient hook — called for each inbound non-core packet.  Routes
    /// async events (TAG_ASYNC) to the registered observers.  Solicited
    /// query responses are captured by BusClient::sendQuery() before
    /// reaching here, so this override only needs to decode async events.
    void onModulePacket(uint8_t type, uint8_t tag,
                        const uint8_t* payload, size_t len) override;

private:
    // Decoders — translate raw payload bytes into the typed structs.
    void decodeServoMotionUpdate (const uint8_t*, size_t);
    void decodeServoTargetReached(const uint8_t*, size_t);
    void decodePwmStall          (const uint8_t*, size_t);
    void decodeLedQueueDone    (const uint8_t*, size_t);
    void decodeBatteryAlert      (const uint8_t*, size_t);
    void decodeStatusBroadcast   (const uint8_t*, size_t);

    // Helpers shared between the BATTERY_INFO_REQ / status-broadcast paths.
    static void decodeBatteryInfoPayload (const uint8_t* p, size_t len, BatteryInfo& out);
    static void decodeBatterySection     (const uint8_t* p, size_t len, size_t& off, BatteryInfo& out);

    // Observer fanout
    std::vector<ServoTargetReachedCb> _onTargetReached;
    std::vector<ServoMotionUpdateCb>  _onMotionUpdate;
    std::vector<PwmStallCb>           _onPwmStall;
    std::vector<LedQueueDoneCb>     _onLedQueueDone;
    std::vector<BatteryAlertCb>       _onBatteryAlert;
    std::vector<StatusBroadcastCb>    _onStatusBroadcast;
};

}  // namespace sfx_core

#endif  // SFX_CORE_CLIENT_H
