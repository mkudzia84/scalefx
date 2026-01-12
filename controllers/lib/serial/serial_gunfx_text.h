/*
 * Serial GunFX Text - Human-Readable Protocol Master/Slave Classes
 * 
 * Provides GunFX communication over human-readable text protocol:
 *   - GunFxSerialMasterText: For HubFX Pico (sends commands, receives status)
 *   - GunFxSerialSlaveText: For GunFX Pico (receives commands, sends status)
 * 
 * Both classes implement the IGunFxMaster/IGunFxSlave interfaces from
 * serial_gunfx_types.h, allowing protocol-agnostic controller code.
 * 
 * Text Command Format:
 *   COMMAND_NAME key=value key2=value2\n
 * 
 * Example Commands:
 *   TRIGGER_ON rpm=600
 *   TRIGGER_OFF fanDelayMs=5000
 *   SERVO_SET id=1 pulseUs=1500
 *   SMOKE_HEAT on=1
 */

#ifndef SERIAL_GUNFX_TEXT_H
#define SERIAL_GUNFX_TEXT_H

#include "serial_bus_text.h"
#include "serial_protocol.h"
#include "serial_gunfx_types.h"
#include "serial_command_handler.h"
#include <Stream.h>

// ============================================================================
// GunFxSerialMasterText - Text Protocol Master (HubFX Pico)
// ============================================================================

/**
 * @brief Master-side GunFX serial communication (text protocol)
 * 
 * Used by HubFX Pico to send commands to GunFX slave boards.
 * Implements IGunFxMaster for protocol-agnostic usage.
 * Uses human-readable text commands for testing and debugging.
 * 
 * Commands block until ACK/NACK or timeout by default.
 */
class GunFxSerialMasterText : public IGunFxMaster {
public:
    GunFxSerialMasterText() = default;
    ~GunFxSerialMasterText() override = default;

    GunFxSerialMasterText(const GunFxSerialMasterText&) = delete;
    GunFxSerialMasterText& operator=(const GunFxSerialMasterText&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================
    
    /**
     * @brief Initialize with USB host (for compatibility with binary master)
     */
    bool begin(UsbHost* usbHost, int deviceIndex);
    
    /**
     * @brief Initialize with a Stream (Serial, mock, etc.) - preferred for testing
     */
    bool begin(Stream* stream);

    // ========================================================================
    // IGunFxMaster Implementation
    // ========================================================================

    void end() override;
    int process() override;

    // Trigger Control - blocking until ACK/NACK
    CommandResult triggerOn(uint16_t rpm) override;
    CommandResult triggerOff(uint16_t fanDelayMs = 0) override;

    // Servo Control - blocking until ACK/NACK
    CommandResult setServoPosition(uint8_t servoId, uint16_t pulseUs) override;
    CommandResult setServoConfig(const GunFxServoConfig& config) override;
    CommandResult setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs = 0) override;

    // Smoke Control - blocking until ACK/NACK
    CommandResult setSmokeHeater(bool on) override;
    CommandResult setSmokeSettings(const GunFxSmokeConfig& config) override;

    // Status request - slave responds with current status/metrics
    CommandResult requestStatus() override;

    // Configuration
    void setCommandTimeout(unsigned long timeoutMs) override { _commandTimeoutMs = timeoutMs; }
    void setBlockingMode(bool blocking) override { _blockingMode = blocking; }
    CommandResult lastCommandResult() const override { return _lastCommandResult; }

    // Callbacks
    void onStatus(GunFxStatusCallback callback) override { _statusCallback = callback; }
    void onReady(GunFxReadyCallback callback) override { _readyCallback = callback; }
    void onError(GunFxErrorCallback callback) override { _errorCallback = callback; }

    // State
    const GunFxStatus& lastStatus() const override { return _lastStatus; }
    bool isSlaveReady() const override { return _slaveReady; }
    const char* slaveName() const override { return _slaveName; }
    const GunFxBoardInfo& boardInfo() const override { return _boardInfo; }
    bool isConnected() const override { return _connected; }

    // ========================================================================
    // Additional Methods (Text-specific)
    // ========================================================================

    /**
     * @brief Send initialization command to slave
     * @return Bytes sent, or -1 on error
     */
    int sendInit();

    /**
     * @brief Send shutdown command to slave
     * @return Bytes sent, or -1 on error
     * @note Slave will ACK this command since device stays running
     */
    int sendShutdown();

    /**
     * @brief Send reboot command to slave (fire-and-forget)
     * @return Bytes sent, or -1 on error
     * @note No ACK expected - device reboots immediately
     */
    int sendReboot();

    /**
     * @brief Send bootsel command to slave (fire-and-forget)
     * @return Bytes sent, or -1 on error
     * @note No ACK expected - device enters bootloader immediately
     */
    int sendBootsel();

    /**
     * @brief Send keepalive to slave
     * @return Bytes sent, or -1 on error
     */
    int sendKeepalive();

    /**
     * @brief Set compatible firmware versions
     */
    void setCompatibleVersions(const char** versions, size_t count) {
        _compatibleVersions = versions;
        _compatibleVersionCount = count;
    }

    bool isVersionCompatible() const { return _boardInfo.versionCompatible; }

    /**
     * @brief Set connection timeout
     */
    void setConnectionTimeout(unsigned long timeoutMs) { _connectionTimeoutMs = timeoutMs; }

private:
    int sendCommand(const char* command);
    CommandResult sendCommandBlocking(const char* command);
    CommandResult waitForAckNack();
    void processLine(const char* line);
    void handleInitReady(const char* args);
    void handleStatus(const char* args);
    void handleError(const char* args);
    void handleAck();
    void handleNack(const char* args);
    bool checkVersionCompatibility(const char* version);

    Stream* _serial = nullptr;
    bool _initialized = false;
    bool _connected = false;
    bool _slaveReady = false;
    char _slaveName[65] = "";
    GunFxBoardInfo _boardInfo;
    GunFxStatus _lastStatus;

    char _rxBuffer[512];
    size_t _rxIndex = 0;

    unsigned long _lastRxTimeMs = 0;
    unsigned long _connectionTimeoutMs = 5000;
    unsigned long _commandTimeoutMs = 1000;
    bool _blockingMode = true;

    // ACK/NACK state
    volatile bool _pendingAckNack = false;
    volatile bool _receivedAck = false;
    volatile bool _receivedNack = false;
    uint8_t _lastNackErrorCode = 0;
    char _lastNackReason[64] = "";
    CommandResult _lastCommandResult;

    const char** _compatibleVersions = nullptr;
    size_t _compatibleVersionCount = 0;

    GunFxStatusCallback _statusCallback;
    GunFxReadyCallback _readyCallback;
    GunFxErrorCallback _errorCallback;
};

// ============================================================================
// GunFxSerialSlaveText - Text Protocol Slave (GunFX Pico)
// ============================================================================

/**
 * @brief Slave-side GunFX serial communication (text protocol)
 * 
 * Used by GunFX Pico to receive commands from HubFX master
 * and send status/telemetry back. Implements IGunFxSlave for
 * protocol-agnostic usage and ITextCommandHandler for use with
 * TextCommandRouter (Chain of Responsibility pattern).
 * 
 * Commands handled:
 * - TRIGGER_ON rpm=N
 * - TRIGGER_OFF fanDelayMs=N
 * - SERVO_SET id=N pulseUs=N
 * - SERVO_CONFIG id=N minUs=N maxUs=N ...
 * - SERVO_RECOIL_JERK id=N jerkUs=N varianceUs=N
 * - SMOKE_HEAT on=0|1
 * - SMOKE_SETTINGS ...
 * 
 * Note: KEEPALIVE is handled by SerialInitHandler, not here.
 */
class GunFxSerialSlaveText : public IGunFxSlave, public ITextCommandHandler {
public:
    GunFxSerialSlaveText() = default;
    ~GunFxSerialSlaveText() override = default;

    GunFxSerialSlaveText(const GunFxSerialSlaveText&) = delete;
    GunFxSerialSlaveText& operator=(const GunFxSerialSlaveText&) = delete;

    // ========================================================================
    // ITextCommandHandler Implementation
    // ========================================================================

    /**
     * @brief Try to process a GunFX protocol command
     * @param line The command line to process
     * @return Handled if recognized, NotMyCommand otherwise
     */
    CommandHandleResult tryProcessCommand(const char* line) override;

    /**
     * @brief Get handler name for debugging
     */
    const char* handlerName() const override { return "GunFxSlaveText"; }

    // ========================================================================
    // IGunFxSlave Implementation
    // ========================================================================

    bool begin(Stream* serial, const char* moduleName = "GunFX") override;
    void end() override;
    int process() override;

    // Status Transmission
    int sendStatus(const GunFxStatus& status) override;
    int sendError(uint8_t errorCode, const char* message = nullptr) override;
    int sendAck() override;
    int sendNack(uint8_t errorCode, const char* reason = nullptr) override;

    // Callbacks
    void onTriggerOn(GunFxTriggerOnCallback callback) override { _triggerOnCallback = callback; }
    void onTriggerOff(GunFxTriggerOffCallback callback) override { _triggerOffCallback = callback; }
    void onServoSet(GunFxServoSetCallback callback) override { _servoSetCallback = callback; }
    void onServoSettings(GunFxServoSettingsCallback callback) override { _servoSettingsCallback = callback; }
    void onSmokeHeat(GunFxSmokeHeatCallback callback) override { _smokeHeatCallback = callback; }
    void onSmokeSettings(GunFxSmokeSettingsCallback callback) override { _smokeSettingsCallback = callback; }
    void onStatusRequest(GunFxStatusRequestCallback callback) override { _statusRequestCallback = callback; }

    // State
    bool isInitialized() const override { return _initialized; }
    bool isMasterConnected() const override { return _masterConnected; }
    void setConnectionTimeout(unsigned long timeoutMs) override { _connectionTimeoutMs = timeoutMs; }

    /**
     * @brief Send raw text response
     */
    int sendResponse(const char* response);

private:
    void processLine(const char* line);
    void handleTriggerOn(const char* args);
    void handleTriggerOff(const char* args);
    void handleServoSet(const char* args);
    void handleServoConfig(const char* args);
    void handleRecoilJerk(const char* args);
    void handleSmokeHeat(const char* args);
    void handleSmokeSettings(const char* args);

    Stream* _serial = nullptr;
    bool _initialized = false;
    bool _masterConnected = false;
    char _moduleName[65] = "GunFX";

    char _rxBuffer[512];
    size_t _rxIndex = 0;

    unsigned long _lastRxTimeMs = 0;
    unsigned long _connectionTimeoutMs = 5000;

    // Callbacks - same types as binary version
    GunFxTriggerOnCallback _triggerOnCallback;
    GunFxTriggerOffCallback _triggerOffCallback;
    GunFxServoSetCallback _servoSetCallback;
    GunFxServoSettingsCallback _servoSettingsCallback;
    GunFxSmokeHeatCallback _smokeHeatCallback;
    GunFxSmokeSettingsCallback _smokeSettingsCallback;
    GunFxStatusRequestCallback _statusRequestCallback;
};

#endif // SERIAL_GUNFX_TEXT_H
