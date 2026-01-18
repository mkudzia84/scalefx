/*
 * Serial GunFX Protocol - Binary Protocol Master/Slave
 *
 * Binary COBS protocol master/slave for GunFX muzzle flash controller.
 *   - GunFxMaster: For HubFX (sends commands via USB)
 *   - GunFxSlave: For GunFX Pico (receives commands, implements ICommandHandler)
 *
 * Packet Types (0x01-0x2F range):
 *   TRIGGER_ON (0x01)      - [rpm:u16] Start firing
 *   TRIGGER_OFF (0x02)     - [fanDelayMs:u16] Stop firing
 *   SRV_SET (0x10)         - [id:u8][pulseUs:u16] Set servo position
 *   SRV_SETTINGS (0x11)    - [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
 *   SRV_RECOIL_JERK (0x12) - [id:u8][jerkUs:u16][varianceUs:u16]
 *   SMOKE_HEAT (0x20)      - [on:u8] Enable/disable heater
 *   SMOKE_SETTINGS (0x21)  - [pulsing:u8][speed:u8][high:u8][low:u8][pulseMs:u16][spindownMs:u16]
 */

#ifndef SERIAL_GUNFX_H
#define SERIAL_GUNFX_H

#include <Arduino.h>
#include <functional>
#include "serial_core.h"
#include "serial_bus.h"
#include "serial_error.h"
#include "serial_command_handler.h"

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
            case INVALID_RPM:           return "Invalid RPM (use 1-3000)";
            case ALREADY_FIRING:        return "Already firing";
            case NOT_FIRING:            return "Not firing";
            default:
                return SerialError::getMessage(code);
        }
    }
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
    uint16_t recoilJerkUs = 0;
    uint16_t recoilJerkVarianceUs = 0;
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
 * @brief Board information returned during init
 */
struct GunFxBoardInfo {
    char deviceName[32] = "";
    char firmwareVersion[16] = "";
    char platform[16] = "";
    uint16_t cpuFrequencyMHz = 0;
    uint32_t freeRamBytes = 0;
    bool versionCompatible = true;
};

/**
 * @brief Smoke generator configuration
 */
struct GunFxSmokeConfig {
    bool fanPulsing = false;
    uint8_t fanSpeed = 255;
    uint8_t fanPulseHigh = 255;
    uint8_t fanPulseLow = 80;
    uint16_t fanPulseMs = 50;
    uint16_t fanSpindownMs = 5000;
};

// ============================================================================
// Callback Types
// ============================================================================

// Master callbacks
using GunFxStatusCallback = std::function<void(const GunFxStatus& status)>;
using GunFxReadyCallback = std::function<void(const char* moduleName)>;
using GunFxErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;

// Slave callbacks - return error code (GunFxError::OK for success)
using GunFxTriggerOnCallback = std::function<uint8_t(uint16_t rpm)>;
using GunFxTriggerOffCallback = std::function<uint8_t(uint16_t fanDelayMs)>;
using GunFxServoSetCallback = std::function<uint8_t(uint8_t servoId, uint16_t pulseUs)>;
using GunFxServoSettingsCallback = std::function<uint8_t(const GunFxServoConfig& config)>;
using GunFxSmokeHeatCallback = std::function<uint8_t(bool on)>;
using GunFxSmokeSettingsCallback = std::function<uint8_t(const GunFxSmokeConfig& config)>;
using GunFxStatusRequestCallback = std::function<GunFxStatus()>;

// ============================================================================
// GunFxMaster Class (Binary Protocol)
// ============================================================================

/**
 * @brief Master-side GunFX serial communication (binary COBS protocol)
 * 
 * Used by HubFX to send commands to GunFX slave boards over USB.
 * Extends SerialBus with GunFX-specific commands.
 */
class GunFxMaster : public SerialBus {
public:
    GunFxMaster() = default;
    ~GunFxMaster() = default;

    GunFxMaster(const GunFxMaster&) = delete;
    GunFxMaster& operator=(const GunFxMaster&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================
    
    /**
     * @brief Initialize with USB host
     */
    bool begin(UsbHost* usbHost, int deviceIndex);
    
    /**
     * @brief Process incoming packets (call in loop)
     */
    int process();

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

    // ========================================================================
    // Status
    // ========================================================================
    
    CommandResult requestStatus();

    // ========================================================================
    // Connection Management
    // ========================================================================
    
    /**
     * @brief Send INIT command with optional keepalive
     */
    int sendInit(unsigned long keepaliveMs = 0);

    // ========================================================================
    // Configuration
    // ========================================================================
    
    void setCommandTimeout(unsigned long timeoutMs) { _commandTimeoutMs = timeoutMs; }
    void setBlockingMode(bool blocking) { _blockingMode = blocking; }
    void setCompatibleVersions(const char** versions, size_t count);

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onReady(GunFxReadyCallback cb) { _readyCallback = cb; }
    void onStatus(GunFxStatusCallback cb) { _statusCallback = cb; }
    void onError(GunFxErrorCallback cb) { _errorCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================
    
    bool isSlaveReady() const { return _slaveReady; }
    bool isVersionCompatible() const { return _boardInfo.versionCompatible; }
    const char* slaveName() const { return _slaveName; }
    const GunFxBoardInfo& boardInfo() const { return _boardInfo; }
    const GunFxStatus& lastStatus() const { return _lastStatus; }
    CommandResult lastCommandResult() const { return _lastCommandResult; }

private:
    void handlePacket(uint8_t type, const uint8_t* payload, size_t len);
    CommandResult sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len);
    CommandResult waitForAckNack();
    bool checkVersionCompatibility(const char* version);

    UsbHost* _usbHostRef = nullptr;
    bool _slaveReady = false;
    char _slaveName[65] = "";
    GunFxBoardInfo _boardInfo;
    GunFxStatus _lastStatus;

    unsigned long _commandTimeoutMs = 1000;
    bool _blockingMode = true;

    volatile bool _pendingAckNack = false;
    volatile bool _receivedAck = false;
    volatile bool _receivedNack = false;
    uint8_t _lastNackErrorCode = 0;
    char _lastNackReason[64] = "";
    CommandResult _lastCommandResult;

    const char** _compatibleVersions = nullptr;
    size_t _compatibleVersionCount = 0;

    GunFxReadyCallback _readyCallback;
    GunFxStatusCallback _statusCallback;
    GunFxErrorCallback _errorCallback;
};

// ============================================================================
// GunFxSlave Class (Binary Protocol)
// ============================================================================

/**
 * @brief Slave-side GunFX serial communication (binary COBS protocol)
 * 
 * Used by GunFX Pico to receive commands from HubFX master.
 * Implements ICommandHandler for use with CommandRouter.
 */
class GunFxSlave : public ICommandHandler {
public:
    GunFxSlave() = default;
    ~GunFxSlave() override = default;

    GunFxSlave(const GunFxSlave&) = delete;
    GunFxSlave& operator=(const GunFxSlave&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================
    
    bool begin(Stream* serial, const char* moduleName = "GunFX");
    void end();
    int process();

    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================
    
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override;
    const char* handlerName() const override { return "GunFxSlave"; }

    // ========================================================================
    // Response Methods
    // ========================================================================
    
    int sendAck();
    int sendNack(uint8_t errorCode, const char* reason = nullptr);
    int sendStatus(const GunFxStatus& status);
    int sendError(uint8_t errorCode, const char* message = nullptr);

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onTriggerOn(GunFxTriggerOnCallback cb) { _triggerOnCallback = cb; }
    void onTriggerOff(GunFxTriggerOffCallback cb) { _triggerOffCallback = cb; }
    void onServoSet(GunFxServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(GunFxServoSettingsCallback cb) { _servoSettingsCallback = cb; }
    void onSmokeHeat(GunFxSmokeHeatCallback cb) { _smokeHeatCallback = cb; }
    void onSmokeSettings(GunFxSmokeSettingsCallback cb) { _smokeSettingsCallback = cb; }
    void onStatusRequest(GunFxStatusRequestCallback cb) { _statusRequestCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================
    
    bool isInitialized() const { return _initialized; }
    bool isMasterConnected() const { return _masterConnected; }
    void setConnectionTimeout(unsigned long timeoutMs) { _connectionTimeoutMs = timeoutMs; }

private:
    CommandHandleResult handlePacket(uint8_t type, const uint8_t* payload, size_t len);
    void processFrame(const uint8_t* frame, size_t frameLen);
    int sendRawPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0);

    Stream* _serial = nullptr;
    bool _initialized = false;
    bool _masterConnected = false;
    char _moduleName[65] = "GunFX";

    uint8_t _rxBuffer[CoreProtocol::COBS_BUFFER_SIZE];
    size_t _rxIndex = 0;

    unsigned long _lastRxTimeMs = 0;
    unsigned long _connectionTimeoutMs = 5000;

    GunFxTriggerOnCallback _triggerOnCallback;
    GunFxTriggerOffCallback _triggerOffCallback;
    GunFxServoSetCallback _servoSetCallback;
    GunFxServoSettingsCallback _servoSettingsCallback;
    GunFxSmokeHeatCallback _smokeHeatCallback;
    GunFxSmokeSettingsCallback _smokeSettingsCallback;
    GunFxStatusRequestCallback _statusRequestCallback;
};

#endif // SERIAL_GUNFX_H
