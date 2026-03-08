/*
 * Serial GunFX Protocol - Binary Protocol Client/Server
 *
 * Binary COBS protocol client/server for GunFX muzzle flash controller.
 *   - GunFxClient: For HubFX (sends commands via USB)
 *   - GunFxServer: For GunFX Pico (receives commands, implements ICommandHandler)
 *
 * Packet Types (0x01-0x2F range):
 *   TRIGGER_ON (0x01)      - [rpm:u16] Start firing
 *   TRIGGER_OFF (0x02)     - [fanDelayMs:u16] Stop firing
 *   SRV_SET (0x10)         - [id:u8][pulseUs:u16] Set servo position
 *   SRV_SETTINGS (0x11)    - [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
 *   SRV_RECOIL_JERK (0x12) - [id:u8][jerkUs:u16][varianceUs:u16]
 *   SMOKE_HEAT (0x20)      - [on:u8] Enable/disable heater
 *   SMOKE_SETTINGS (0x21)  - [pulsing:u8][speed:u8][high:u8][low:u8][pulseMs:u16][spindownMs:u16]
 *   SMOKE_RESET (0x22)     - [] Clear smoke error states (heater/fan disconnect)
 *   SMOKE_CURRENT_LIMIT (0x23) - [channel:u8][limit_mA:u16] Set overcurrent protection limit
 */

#ifndef SERIAL_GUNFX_H
#define SERIAL_GUNFX_H

#include <Arduino.h>
#include <functional>
#include "serial_core.h"
#include "serial_bus_client.h"
#include "serial_bus_server.h"

// ============================================================================
// GunFX Binary Packet Types (0x01-0x2F range)
// ============================================================================

namespace GunFxPacket {
    // Trigger control
    constexpr uint8_t TRIGGER_ON      = 0x01;  // [rpm:u16]
    constexpr uint8_t TRIGGER_OFF     = 0x02;  // [fan_delay_ms:u16]
    
    // Servo control
    constexpr uint8_t SRV_SET         = 0x10;  // [id:u8][pulse_us:u16]
    constexpr uint8_t SRV_SETTINGS    = 0x11;  // [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
    constexpr uint8_t SRV_RECOIL_JERK = 0x12;  // [id:u8][jerk_us:u16][variance_us:u16]
    
    // Smoke control
    constexpr uint8_t SMOKE_HEAT      = 0x20;  // [on:u8]
    constexpr uint8_t SMOKE_SETTINGS  = 0x21;  // [pulsing:u8][speed:u8][high:u8][low:u8][pulse_ms:u16][spindown_ms:u16]
    constexpr uint8_t SMOKE_RESET     = 0x22;  // [] Clear smoke error states
    constexpr uint8_t SMOKE_CURRENT_LIMIT = 0x23;  // [channel:u8][limit_mA:u16LE] Set overcurrent limit
}

// ============================================================================
// GunFX Error Codes
// ============================================================================

namespace GunFxError {
    // Import generic error codes for convenience
    using namespace SerialError;
    
    // Servo errors (0x20-0x2F)
    constexpr uint8_t SERVO_INVALID_ID      = 0x20;  // Servo ID out of range (1-3)
    constexpr uint8_t SERVO_PULSE_RANGE     = 0x21;  // Pulse width outside 500-2500µs
    constexpr uint8_t SERVO_MIN_MAX         = 0x22;  // minUs >= maxUs
    constexpr uint8_t SERVO_NOT_CONFIGURED  = 0x23;  // Servo not configured
    
    // Smoke/heater errors (0x30-0x3F)
    constexpr uint8_t INVALID_FAN_SPEED     = 0x30;  // Invalid fan speed value
    constexpr uint8_t HEATER_DISCONNECTED   = 0x31;  // Heater drawing 0 current
    constexpr uint8_t FAN_DISCONNECTED      = 0x32;  // Fan motor drawing 0 current
    constexpr uint8_t HEATER_OVERCURRENT    = 0x33;  // Heater current exceeded limit
    constexpr uint8_t FAN_OVERCURRENT       = 0x34;  // Fan current exceeded limit
    
    // Trigger errors (0x40-0x4F)
    constexpr uint8_t INVALID_RPM           = 0x40;  // RPM out of range (1-3000)
    constexpr uint8_t ALREADY_FIRING        = 0x41;  // Already firing
    constexpr uint8_t NOT_FIRING            = 0x42;  // Not currently firing
    
    /**
     * @brief Get human-readable error message for GunFX errors
     */
    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case SERVO_INVALID_ID:      return "Invalid servo ID (use 1-3)";
            case SERVO_PULSE_RANGE:     return "Pulse width out of range (500-2500)";
            case SERVO_MIN_MAX:         return "minUs must be less than maxUs";
            case SERVO_NOT_CONFIGURED:  return "Servo not configured";
            case INVALID_FAN_SPEED:     return "Invalid fan speed";
            case HEATER_DISCONNECTED:   return "Heater disconnected (0 current)";
            case FAN_DISCONNECTED:      return "Fan disconnected (0 current)";
            case HEATER_OVERCURRENT:    return "Heater overcurrent protection triggered";
            case FAN_OVERCURRENT:       return "Fan overcurrent protection triggered";
            case INVALID_RPM:           return "Invalid RPM (use 1-3000)";
            case ALREADY_FIRING:        return "Already firing";
            case NOT_FIRING:            return "Not firing";
            default:
                return SerialError::getMessage(code);
        }
    }
}

// ============================================================================
// GunFX Hardware Specification Constants
// ============================================================================

namespace GunFxSpec {
    // Servo limits (standard PWM range)
    constexpr uint8_t SERVO_ID_MIN = 1;
    constexpr uint8_t SERVO_ID_MAX = 3;
    constexpr uint16_t SERVO_PULSE_MIN = 500;    // µs
    constexpr uint16_t SERVO_PULSE_MAX = 2500;   // µs
    
    // Trigger limits
    constexpr uint16_t TRIGGER_RPM_MIN = 1;
    constexpr uint16_t TRIGGER_RPM_MAX = 3000;
    
    // Fan speed limits
    constexpr uint8_t FAN_SPEED_MAX = 255;       // PWM value
    
    // Validation helpers
    inline bool isValidServoId(uint8_t id) {
        return id >= SERVO_ID_MIN && id <= SERVO_ID_MAX;
    }
    
    inline bool isValidServoPulse(uint16_t pulse) {
        return pulse >= SERVO_PULSE_MIN && pulse <= SERVO_PULSE_MAX;
    }
    
    inline bool isValidRpm(uint16_t rpm) {
        return rpm >= TRIGGER_RPM_MIN && rpm <= TRIGGER_RPM_MAX;
    }
}

// ============================================================================
// Smoke Error Reason Codes (STATUS diagnostic)
// ============================================================================

namespace SmokeErrorReason {
    constexpr uint8_t NONE                = 0x00;  // No error
    constexpr uint8_t HEATER_DISCONNECTED = 0x01;  // Heater drawing 0 current while ON
    constexpr uint8_t FAN_DISCONNECTED    = 0x02;  // Fan drawing 0 current while ON
    constexpr uint8_t HEATER_OVERCURRENT  = 0x03;  // Heater current exceeded limit
    constexpr uint8_t FAN_OVERCURRENT     = 0x04;  // Fan current exceeded limit
}

// ============================================================================
// GunFX Data Types
// ============================================================================

/**
 * @brief Servo configuration for motion profiling
 */
struct GunFxServoConfig {
    uint8_t servoId = 0;
    uint16_t minUs = 1000;
    uint16_t maxUs = 2000;
    uint16_t maxSpeedUsPerSec = 0;      // 0 = no limit
    uint16_t maxAccelUsPerSec2 = 0;     // 0 = no limit
    uint16_t maxDecelUsPerSec2 = 0;     // 0 = no limit
};

/**
 * @brief Status and metrics from GunFX slave
 */
struct GunFxStatus {
    // Operational state
    bool firing = false;
    bool flashActive = false;
    bool flashFading = false;
    bool heaterOn = false;
    bool fanOn = false;
    bool fanSpindown = false;
    uint8_t fanSpeed = 0;
    uint16_t fanOffRemainingMs = 0;
    uint16_t servoUs[3] = {0, 0, 0};
    uint16_t rateOfFireRpm = 0;
    
    // Session metrics
    uint32_t shotsFired = 0;
    uint32_t heaterOnTimeMs = 0;
    
    // System health
    uint32_t uptimeMs = 0;
    uint32_t freeRam = 0;
};

/**
 * @brief Board information returned during init (alias for BusClientBoardInfo)
 */
using GunFxBoardInfo = BusClientBoardInfo;

/**
 * @brief Smoke generator configuration
 */
struct GunFxSmokeConfig {
    bool fanPulsing = false;
    uint8_t fanSpeed = 255;
    uint8_t fanPulseHigh = 255;
    uint8_t fanPulseLow = 80;
    uint16_t fanPulseMs = 0;        // 0 = auto-calculate from RPM
    uint16_t fanSpindownMs = 5000;
};

// ============================================================================
// Callback Types
// ============================================================================

// Client callback
using GunFxStatusCallback = std::function<void(const GunFxStatus& status)>;

// Server callbacks - return error code (GunFxError::OK for success)
using GunFxTriggerOnCallback = std::function<uint8_t(uint16_t rpm)>;
using GunFxTriggerOffCallback = std::function<uint8_t(uint16_t fanDelayMs)>;
using GunFxServoSetCallback = std::function<uint8_t(uint8_t servoId, uint16_t pulseUs)>;
using GunFxServoSettingsCallback = std::function<uint8_t(const GunFxServoConfig& config)>;
using GunFxRecoilJerkCallback = std::function<uint8_t(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs)>;
using GunFxSmokeHeatCallback = std::function<uint8_t(bool on)>;
using GunFxSmokeSettingsCallback = std::function<uint8_t(const GunFxSmokeConfig& config)>;
using GunFxSmokeResetCallback = std::function<uint8_t()>;
using GunFxSmokeCurrentLimitCallback = std::function<uint8_t(uint8_t channel, uint16_t limit_mA)>;

// ============================================================================
// GunFxClient Class (Binary Protocol)
// ============================================================================

/**
 * @brief Client-side GunFX serial communication (binary COBS protocol)
 * 
 * Used by HubFX to send commands to GunFX server boards over USB.
 * Extends BusClient with GunFX-specific commands and STATUS parsing.
 */
class GunFxClient : public BusClient {
public:
    // ========================================================================
    // Trigger Control
    // ========================================================================
    
    CommandResult triggerOn(uint16_t rpm);
    CommandResult triggerOff(uint16_t fanDelayMs = 0);

    // ========================================================================
    // Servo Control
    // ========================================================================
    
    CommandResult setServoPosition(uint8_t servoId, uint16_t pulseUs);
    CommandResult setServoConfig(const GunFxServoConfig& config);
    CommandResult setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs = 0);

    // ========================================================================
    // Smoke Control
    // ========================================================================
    
    CommandResult setSmokeHeater(bool on);
    CommandResult setSmokeSettings(const GunFxSmokeConfig& config);
    CommandResult smokeReset();
    CommandResult smokeCurrentLimit(uint8_t channel, uint16_t limit_mA);

    // ========================================================================
    // Status
    // ========================================================================
    
    CommandResult requestStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onStatus(GunFxStatusCallback cb) { _statusCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================
    
    const GunFxStatus& lastStatus() const { return _lastStatus; }

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return GunFxError::getMessage(code); }

private:
    GunFxStatus _lastStatus;
    GunFxStatusCallback _statusCallback;
};

// ============================================================================
// GunFxServer Class (Binary Protocol)
// ============================================================================

/**
 * @brief Server-side GunFX serial communication (binary COBS protocol)
 * 
 * Used by GunFX Pico to receive commands from HubFX client.
 * Extends BusServer for use with PicoServer + CommandRouter.
 */
class GunFxServer : public BusServer {
public:
    const char* handlerName() const override { return "GunFxServer"; }

    // ========================================================================
    // Response Methods
    // ========================================================================
    
    int sendStatus(const GunFxStatus& status);

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onTriggerOn(GunFxTriggerOnCallback cb) { _triggerOnCallback = cb; }
    void onTriggerOff(GunFxTriggerOffCallback cb) { _triggerOffCallback = cb; }
    void onServoSet(GunFxServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(GunFxServoSettingsCallback cb) { _servoSettingsCallback = cb; }
    void onRecoilJerk(GunFxRecoilJerkCallback cb) { _recoilJerkCallback = cb; }
    void onSmokeHeat(GunFxSmokeHeatCallback cb) { _smokeHeatCallback = cb; }
    void onSmokeSettings(GunFxSmokeSettingsCallback cb) { _smokeSettingsCallback = cb; }
    void onSmokeReset(GunFxSmokeResetCallback cb) { _smokeResetCallback = cb; }
    void onSmokeCurrentLimit(GunFxSmokeCurrentLimitCallback cb) { _smokeCurrentLimitCallback = cb; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override { return 0x01; }
    uint8_t moduleRangeHigh() const override { return 0x2F; }
    const char* getModuleErrorMessage(uint8_t code) override { return GunFxError::getMessage(code); }

private:
    GunFxTriggerOnCallback _triggerOnCallback;
    GunFxTriggerOffCallback _triggerOffCallback;
    GunFxServoSetCallback _servoSetCallback;
    GunFxServoSettingsCallback _servoSettingsCallback;
    GunFxRecoilJerkCallback _recoilJerkCallback;
    GunFxSmokeHeatCallback _smokeHeatCallback;
    GunFxSmokeSettingsCallback _smokeSettingsCallback;
    GunFxSmokeResetCallback _smokeResetCallback;
    GunFxSmokeCurrentLimitCallback _smokeCurrentLimitCallback;
};

#endif // SERIAL_GUNFX_H
