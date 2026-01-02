/*
 * Serial GunFX - Master/Slave Communication Classes
 * 
 * Provides specialized SerialBus subclasses for GunFX communication:
 *   - GunFxSerialMaster: For HubFX Pico (sends commands, receives status)
 *   - GunFxSerialSlave: For GunFX Pico (receives commands, sends status)
 * 
 * These classes abstract the packet format details and provide a clean API
 * for GunFX-specific functionality like trigger control, servo management,
 * and smoke effects.
 */

#ifndef SERIAL_GUNFX_H
#define SERIAL_GUNFX_H

#include <serial_common.h>
#include <functional>

// ============================================================================
// GunFX Data Structures
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
 * @brief Status flags from GunFX slave
 */
struct GunFxStatus {
    bool firing = false;
    bool flashActive = false;
    bool flashFading = false;
    bool heaterOn = false;
    bool fanOn = false;
    bool fanSpindown = false;
    uint16_t fanOffRemainingMs = 0;
    uint16_t servoUs[3] = {0, 0, 0};
    uint16_t rateOfFireRpm = 0;
};

// ============================================================================
// Master Callback Types
// ============================================================================

using GunFxStatusCallback = std::function<void(const GunFxStatus& status)>;
using GunFxReadyCallback = std::function<void(const char* moduleName)>;
using GunFxErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;

// ============================================================================
// Slave Callback Types
// ============================================================================

using GunFxTriggerOnCallback = std::function<void(uint16_t rpm)>;
using GunFxTriggerOffCallback = std::function<void(uint16_t fanDelayMs)>;
using GunFxServoSetCallback = std::function<void(uint8_t servoId, uint16_t pulseUs)>;
using GunFxServoSettingsCallback = std::function<void(const GunFxServoConfig& config)>;
using GunFxSmokeHeatCallback = std::function<void(bool on)>;
using GunFxInitCallback = std::function<void()>;
using GunFxShutdownCallback = std::function<void()>;

// ============================================================================
// GunFxSerialMaster - For HubFX Pico
// ============================================================================

/**
 * @brief Master-side GunFX serial communication
 * 
 * Used by HubFX Pico to send commands to GunFX slave boards.
 * Provides high-level API for trigger control, servo management,
 * and smoke effects.
 */
class GunFxSerialMaster : public SerialBus {
public:
    GunFxSerialMaster() = default;
    ~GunFxSerialMaster() = default;

    // Delete copy operations
    GunFxSerialMaster(const GunFxSerialMaster&) = delete;
    GunFxSerialMaster& operator=(const GunFxSerialMaster&) = delete;

    /**
     * @brief Initialize the master serial bus
     * @param usbHost Pointer to USB host
     * @param deviceIndex CDC device index
     * @return true if successful
     */
    bool begin(UsbHost* usbHost, int deviceIndex);

    // ========================================================================
    // Trigger Control
    // ========================================================================

    /**
     * @brief Start firing at specified rate
     * @param rpm Rounds per minute
     * @return Bytes sent, or -1 on error
     */
    int triggerOn(uint16_t rpm);

    /**
     * @brief Stop firing
     * @param fanDelayMs Delay before fan stops (ms)
     * @return Bytes sent, or -1 on error
     */
    int triggerOff(uint16_t fanDelayMs = 0);

    // ========================================================================
    // Servo Control
    // ========================================================================

    /**
     * @brief Set servo position
     * @param servoId Servo ID (1-3)
     * @param pulseUs Pulse width in microseconds
     * @return Bytes sent, or -1 on error
     */
    int setServoPosition(uint8_t servoId, uint16_t pulseUs);

    /**
     * @brief Configure servo motion profile
     * @param config Servo configuration
     * @return Bytes sent, or -1 on error
     */
    int setServoConfig(const GunFxServoConfig& config);

    /**
     * @brief Configure servo recoil jerk effect
     * @param servoId Servo ID (1-3)
     * @param jerkUs Jerk amount in microseconds
     * @param varianceUs Random variance in microseconds
     * @return Bytes sent, or -1 on error
     */
    int setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs = 0);

    // ========================================================================
    // Smoke Control
    // ========================================================================

    /**
     * @brief Control smoke heater
     * @param on true to enable heater, false to disable
     * @return Bytes sent, or -1 on error
     */
    int setSmokeHeater(bool on);

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Send initialization command to slave
     * @return Bytes sent, or -1 on error
     */
    int sendInit() { return SerialBus::sendInit(); }

    /**
     * @brief Send shutdown command to slave
     * @return Bytes sent, or -1 on error
     */
    int sendShutdown() { return SerialBus::sendShutdown(); }

    // ========================================================================
    // Callbacks
    // ========================================================================

    /**
     * @brief Set callback for status updates from slave
     */
    void onStatus(GunFxStatusCallback callback) { _statusCallback = callback; }

    /**
     * @brief Set callback for slave ready notification
     */
    void onReady(GunFxReadyCallback callback) { _readyCallback = callback; }

    /**
     * @brief Set callback for error notifications
     */
    void onError(GunFxErrorCallback callback) { _errorCallback = callback; }

    // ========================================================================
    // Status
    // ========================================================================

    /**
     * @brief Get last received status
     */
    const GunFxStatus& lastStatus() const { return _lastStatus; }

    /**
     * @brief Check if slave is ready
     */
    bool isSlaveReady() const { return _slaveReady; }

    /**
     * @brief Get slave module name
     */
    const char* slaveName() const { return _slaveName; }

private:
    void handlePacket(uint8_t type, const uint8_t* payload, size_t len);

    GunFxStatusCallback _statusCallback;
    GunFxReadyCallback _readyCallback;
    GunFxErrorCallback _errorCallback;

    GunFxStatus _lastStatus;
    bool _slaveReady = false;
    char _slaveName[65] = "";
};

// ============================================================================
// GunFxSerialSlave - For GunFX Pico
// ============================================================================

/**
 * @brief Slave-side GunFX serial communication
 * 
 * Used by GunFX Pico to receive commands from HubFX master
 * and send status/telemetry back.
 */
class GunFxSerialSlave {
public:
    GunFxSerialSlave() = default;
    ~GunFxSerialSlave() = default;

    // Delete copy operations
    GunFxSerialSlave(const GunFxSerialSlave&) = delete;
    GunFxSerialSlave& operator=(const GunFxSerialSlave&) = delete;

    /**
     * @brief Initialize the slave serial communication
     * @param serial Pointer to Stream object (e.g., &Serial, &Serial1)
     * @param moduleName Name to send in INIT_READY response
     * @return true if successful
     */
    bool begin(Stream* serial, const char* moduleName = "GunFX");

    /**
     * @brief End communication
     */
    void end();

    /**
     * @brief Process incoming data - call regularly from loop()
     * @return Number of packets processed
     */
    int process();

    // ========================================================================
    // Status Transmission
    // ========================================================================

    /**
     * @brief Send status update to master
     * @param status Current status
     * @return Bytes sent, or -1 on error
     */
    int sendStatus(const GunFxStatus& status);

    /**
     * @brief Send INIT_READY response to master
     * @return Bytes sent, or -1 on error
     */
    int sendInitReady();

    /**
     * @brief Send error to master
     * @param errorCode Error code
     * @param message Optional error message
     * @return Bytes sent, or -1 on error
     */
    int sendError(uint8_t errorCode, const char* message = nullptr);

    /**
     * @brief Send acknowledgment to master
     * @return Bytes sent, or -1 on error
     */
    int sendAck();

    /**
     * @brief Send negative acknowledgment to master
     * @param reason Optional reason string
     * @return Bytes sent, or -1 on error
     */
    int sendNack(const char* reason = nullptr);

    // ========================================================================
    // Callbacks - Command Reception
    // ========================================================================

    void onTriggerOn(GunFxTriggerOnCallback callback) { _triggerOnCallback = callback; }
    void onTriggerOff(GunFxTriggerOffCallback callback) { _triggerOffCallback = callback; }
    void onServoSet(GunFxServoSetCallback callback) { _servoSetCallback = callback; }
    void onServoSettings(GunFxServoSettingsCallback callback) { _servoSettingsCallback = callback; }
    void onSmokeHeat(GunFxSmokeHeatCallback callback) { _smokeHeatCallback = callback; }
    void onInit(GunFxInitCallback callback) { _initCallback = callback; }
    void onShutdown(GunFxShutdownCallback callback) { _shutdownCallback = callback; }

    // ========================================================================
    // Status
    // ========================================================================

    bool isInitialized() const { return _initialized; }
    bool isMasterConnected() const { return _masterConnected; }

    /**
     * @brief Set timeout for master connection (keepalive)
     * @param timeoutMs Timeout in milliseconds (0 to disable)
     */
    void setConnectionTimeout(unsigned long timeoutMs) { _connectionTimeoutMs = timeoutMs; }

private:
    void handlePacket(uint8_t type, const uint8_t* payload, size_t len);
    void processFrame(const uint8_t* frame, size_t frameLen);
    int sendRawPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0);

    Stream* _serial = nullptr;
    bool _initialized = false;
    bool _masterConnected = false;
    char _moduleName[65] = "GunFX";

    uint8_t _rxBuffer[SerialProtocol::COBS_BUFFER_SIZE];
    size_t _rxIndex = 0;

    unsigned long _lastRxTimeMs = 0;
    unsigned long _connectionTimeoutMs = 5000;  // 5 second default timeout

    // Callbacks
    GunFxTriggerOnCallback _triggerOnCallback;
    GunFxTriggerOffCallback _triggerOffCallback;
    GunFxServoSetCallback _servoSetCallback;
    GunFxServoSettingsCallback _servoSettingsCallback;
    GunFxSmokeHeatCallback _smokeHeatCallback;
    GunFxInitCallback _initCallback;
    GunFxShutdownCallback _shutdownCallback;
};

#endif // SERIAL_GUNFX_H
