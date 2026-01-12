/*
 * GunFX Common Types - Shared types for binary and text protocols
 * 
 * This file contains all shared types, structures, and callback definitions
 * used by both GunFxSerialSlave/Master (binary) and GunFxSerialSlaveText/MasterText.
 * 
 * Using these common types allows controller code to be protocol-agnostic.
 */

#ifndef SERIAL_GUNFX_TYPES_H
#define SERIAL_GUNFX_TYPES_H

#include <functional>
#include <cstdint>
#include "serial_error.h"  // Generic error codes and CommandResult

// ============================================================================
// GunFX-Specific Error Codes
// ============================================================================

/**
 * @brief GunFX-specific error codes for NACK responses
 * 
 * These extend the generic SerialError codes with GunFX-specific errors.
 * Uses error code range 0x20-0x4F as allocated for GunFX.
 * 
 * For generic errors (0x00-0x1F, 0xF0-0xFF), use SerialError namespace.
 */
namespace GunFxError {
    // Import generic error codes for convenience
    using namespace SerialError;
    
    // Servo errors (0x20-0x2F)
    constexpr uint8_t SERVO_INVALID_ID      = 0x20;  // Servo ID out of range (1-3)
    constexpr uint8_t SERVO_PULSE_RANGE     = 0x21;  // Pulse width outside 500-2500µs
    constexpr uint8_t SERVO_MIN_MAX         = 0x22;  // minUs >= maxUs
    constexpr uint8_t SERVO_NOT_CONFIGURED  = 0x23;  // Servo not configured
    
    // Smoke/heater errors (0x30-0x3F)
    constexpr uint8_t HEATER_SAFETY         = 0x30;  // Heater safety interlock
    constexpr uint8_t FAN_NOT_RUNNING       = 0x31;  // Fan must be running for heater
    constexpr uint8_t INVALID_FAN_SPEED     = 0x32;  // Invalid fan speed value
    
    // Trigger errors (0x40-0x4F)
    constexpr uint8_t INVALID_RPM           = 0x40;  // RPM out of range (1-3000)
    constexpr uint8_t ALREADY_FIRING        = 0x41;  // Already firing
    constexpr uint8_t NOT_FIRING            = 0x42;  // Not currently firing
    
    /**
     * @brief Get human-readable error message for GunFX errors
     * @param code Error code
     * @return Error message string
     * 
     * Falls back to SerialError::getMessage() for generic errors.
     */
    inline const char* getMessage(uint8_t code) {
        // Check GunFX-specific errors first
        switch (code) {
            case SERVO_INVALID_ID:      return "Invalid servo ID (use 1-3)";
            case SERVO_PULSE_RANGE:     return "Pulse width out of range (500-2500)";
            case SERVO_MIN_MAX:         return "minUs must be less than maxUs";
            case SERVO_NOT_CONFIGURED:  return "Servo not configured";
            case HEATER_SAFETY:         return "Heater safety interlock";
            case FAN_NOT_RUNNING:       return "Fan must be running for heater";
            case INVALID_FAN_SPEED:     return "Invalid fan speed";
            case INVALID_RPM:           return "Invalid RPM (use 1-3000)";
            case ALREADY_FIRING:        return "Already firing";
            case NOT_FIRING:            return "Not firing";
            default:
                // Fall back to generic error messages
                return SerialError::getMessage(code);
        }
    }
}

// ============================================================================
// GunFX Data Structures
// ============================================================================

/**
 * @brief Board and firmware information from slave
 */
struct GunFxBoardInfo {
    char deviceName[32] = "";
    char firmwareVersion[16] = "";
    char platform[32] = "";
    uint32_t cpuFrequencyMHz = 0;
    uint32_t freeRamBytes = 0;
    uint32_t buildNumber = 0;
    bool versionCompatible = false;  // True if version matches compatibility list
};

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
    uint16_t recoilJerkUs = 0;
    uint16_t recoilJerkVarianceUs = 0;
};

/**
 * @brief Status and metrics from GunFX slave
 * 
 * Contains both current operational state and cumulative session metrics.
 * Sent in response to STATUS_REQ from master.
 */
struct GunFxStatus {
    // Operational state
    bool firing = false;
    bool flashActive = false;
    bool flashFading = false;
    bool heaterOn = false;
    bool fanOn = false;
    bool fanSpindown = false;
    uint8_t fanSpeed = 0;               // Current fan PWM (0-255)
    uint16_t fanOffRemainingMs = 0;
    uint16_t servoUs[3] = {0, 0, 0};
    uint16_t rateOfFireRpm = 0;
    
    // Session metrics (cumulative since boot)
    uint32_t shotsFired = 0;            // Total shots fired
    uint32_t heaterOnTimeMs = 0;        // Total heater on-time in ms
    
    // System health
    uint32_t uptimeMs = 0;              // Time since boot
    uint32_t freeRam = 0;               // Free heap memory in bytes
};

/**
 * @brief Smoke generator configuration
 */
struct GunFxSmokeConfig {
    bool fanPulsing = false;          // true = pulse with RPM, false = constant speed
    uint8_t fanSpeed = 255;           // Fan speed when on (0-255)
    uint8_t fanPulseHigh = 255;       // Fan speed during shot pulse (pulsing mode)
    uint8_t fanPulseLow = 80;         // Fan speed between shots (pulsing mode)
    uint16_t fanPulseMs = 50;         // Duration of high-speed pulse per shot
    uint16_t fanSpindownMs = 5000;    // Default fan spindown delay after firing stops
};

// ============================================================================
// Callback Types - Used by both binary and text implementations
// ============================================================================

// Master callbacks (receiving from slave)
using GunFxStatusCallback = std::function<void(const GunFxStatus& status)>;
using GunFxReadyCallback = std::function<void(const char* moduleName)>;
using GunFxErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;

// Slave callbacks (receiving from master)
// Return SerialError::OK for success, or error code for NACK
using GunFxTriggerOnCallback = std::function<uint8_t(uint16_t rpm)>;
using GunFxTriggerOffCallback = std::function<uint8_t(uint16_t fanDelayMs)>;
using GunFxServoSetCallback = std::function<uint8_t(uint8_t servoId, uint16_t pulseUs)>;
using GunFxServoSettingsCallback = std::function<uint8_t(const GunFxServoConfig& config)>;
using GunFxSmokeHeatCallback = std::function<uint8_t(bool on)>;
using GunFxSmokeSettingsCallback = std::function<uint8_t(const GunFxSmokeConfig& config)>;

// Status request callback - slave should populate status and return it
using GunFxStatusRequestCallback = std::function<GunFxStatus()>;

// ============================================================================
// Abstract Interface - IGunFxSlave
// ============================================================================

/**
 * @brief Abstract interface for GunFX slave implementations
 * 
 * Both GunFxSerialSlave (binary) and GunFxSerialSlaveText implement this interface,
 * allowing controller code to be protocol-agnostic.
 * 
 * Commands received from master trigger callbacks. The slave sends ACK on success
 * or NACK with error code on failure.
 */
class IGunFxSlave {
public:
    virtual ~IGunFxSlave() = default;

    // Lifecycle
    virtual bool begin(Stream* serial, const char* moduleName = "GunFX") = 0;
    virtual void end() = 0;

    // Processing
    virtual int process() = 0;

    // Response transmission
    virtual int sendStatus(const GunFxStatus& status) = 0;
    virtual int sendError(uint8_t errorCode, const char* message = nullptr) = 0;
    virtual int sendAck() = 0;
    virtual int sendNack(uint8_t errorCode, const char* reason = nullptr) = 0;

    // Callbacks - return uint8_t error code (GunFxError::OK for success)
    virtual void onTriggerOn(GunFxTriggerOnCallback callback) = 0;
    virtual void onTriggerOff(GunFxTriggerOffCallback callback) = 0;
    virtual void onServoSet(GunFxServoSetCallback callback) = 0;
    virtual void onServoSettings(GunFxServoSettingsCallback callback) = 0;
    virtual void onSmokeHeat(GunFxSmokeHeatCallback callback) = 0;
    virtual void onSmokeSettings(GunFxSmokeSettingsCallback callback) = 0;
    
    // Status request callback - called when master sends STATUS_REQ
    virtual void onStatusRequest(GunFxStatusRequestCallback callback) = 0;

    // State
    virtual bool isInitialized() const = 0;
    virtual bool isMasterConnected() const = 0;
    virtual void setConnectionTimeout(unsigned long timeoutMs) = 0;
};

// ============================================================================
// Abstract Interface - IGunFxMaster
// ============================================================================

/**
 * @brief Abstract interface for GunFX master implementations
 * 
 * Both GunFxSerialMaster (binary) and GunFxSerialMasterText implement this interface,
 * allowing controller code to be protocol-agnostic.
 * 
 * Commands are blocking by default and wait for ACK/NACK response.
 * If blocking is disabled, commands return immediately after sending.
 */
class IGunFxMaster {
public:
    virtual ~IGunFxMaster() = default;

    // Lifecycle
    virtual void end() = 0;

    // Processing - must be called in loop for async responses
    virtual int process() = 0;

    // ========================================================================
    // Command Methods - Block until ACK/NACK or timeout
    // Returns CommandResult with success/error status
    // ========================================================================

    // Trigger control
    virtual CommandResult triggerOn(uint16_t rpm) = 0;
    virtual CommandResult triggerOff(uint16_t fanDelayMs = 0) = 0;

    // Servo control
    virtual CommandResult setServoPosition(uint8_t servoId, uint16_t pulseUs) = 0;
    virtual CommandResult setServoConfig(const GunFxServoConfig& config) = 0;
    virtual CommandResult setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs = 0) = 0;

    // Smoke control
    virtual CommandResult setSmokeHeater(bool on) = 0;
    virtual CommandResult setSmokeSettings(const GunFxSmokeConfig& config) = 0;

    // Status request - slave responds with current status/metrics
    virtual CommandResult requestStatus() = 0;

    // ========================================================================
    // Configuration
    // ========================================================================
    
    /**
     * @brief Set command timeout for ACK/NACK response
     * @param timeoutMs Timeout in milliseconds (default 1000ms)
     */
    virtual void setCommandTimeout(unsigned long timeoutMs) = 0;
    
    /**
     * @brief Enable/disable blocking mode for commands
     * @param blocking If true, commands block until ACK/NACK (default true)
     */
    virtual void setBlockingMode(bool blocking) = 0;
    
    /**
     * @brief Get last command result (useful in non-blocking mode)
     */
    virtual CommandResult lastCommandResult() const = 0;

    // ========================================================================
    // Callbacks - For async/non-blocking mode
    // ========================================================================
    
    virtual void onStatus(GunFxStatusCallback callback) = 0;
    virtual void onReady(GunFxReadyCallback callback) = 0;
    virtual void onError(GunFxErrorCallback callback) = 0;

    // ========================================================================
    // State
    // ========================================================================
    
    virtual const GunFxStatus& lastStatus() const = 0;
    virtual bool isSlaveReady() const = 0;
    virtual const char* slaveName() const = 0;
    virtual const GunFxBoardInfo& boardInfo() const = 0;
    virtual bool isConnected() const = 0;
};

#endif // SERIAL_GUNFX_TYPES_H