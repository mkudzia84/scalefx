/*
 * Serial GunFX - Binary Protocol Master/Slave Classes
 * 
 * Provides GunFX communication over COBS-encoded binary protocol:
 *   - GunFxSerialMaster: For HubFX Pico (sends commands, receives status)
 *   - GunFxSerialSlave: For GunFX Pico (receives commands, sends status)
 * 
 * Both classes implement the IGunFxMaster/IGunFxSlave interfaces from
 * serial_gunfx_types.h, allowing protocol-agnostic controller code.
 */

#ifndef SERIAL_GUNFX_H
#define SERIAL_GUNFX_H

#include "serial_bus.h"
#include "serial_gunfx_types.h"
#include "serial_command_handler.h"

// ============================================================================
// GunFxSerialMaster - Binary Protocol Master (HubFX Pico)
// ============================================================================

/**
 * @brief Master-side GunFX serial communication (binary protocol)
 * 
 * Used by HubFX Pico to send commands to GunFX slave boards.
 * Implements IGunFxMaster for protocol-agnostic usage.
 * Commands block until ACK/NACK or timeout by default.
 */
class GunFxSerialMaster : public SerialBus, public IGunFxMaster {
public:
    GunFxSerialMaster() = default;
    ~GunFxSerialMaster() override = default;

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
    // IGunFxMaster Implementation
    // ========================================================================

    void end() override { SerialBus::end(); }
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
    bool isConnected() const override { return SerialBus::isConnected(); }

    // ========================================================================
    // Additional Methods (Binary-specific)
    // ========================================================================

    /**
     * @brief Send text-based INIT command to slave for protocol negotiation
     * 
     * Sends "INIT protocol=binary keepalive=<interval>" as text.
     * The slave responds with INIT_READY, then binary commands can be used.
     * 
     * @param keepaliveMs Keepalive interval in ms (0 = disabled)
     * @return Bytes sent, or -1 on error
     */
    int sendInit(unsigned long keepaliveMs = 0);

    /**
     * @brief Send shutdown command to slave
     * @return Bytes sent, or -1 on error
     * @note Slave will ACK this command since device stays running
     */
    int sendShutdown() { return SerialBus::sendShutdown(); }

    /**
     * @brief Send reboot command to slave (fire-and-forget)
     * @return Bytes sent, or -1 on error
     * @note No ACK expected - device reboots immediately
     */
    int sendReboot() { return SerialBus::sendReboot(); }

    /**
     * @brief Send bootsel command to slave (fire-and-forget)
     * @return Bytes sent, or -1 on error
     * @note No ACK expected - device enters bootloader immediately
     */
    int sendBootsel() { return SerialBus::sendBootsel(); }

    /**
     * @brief Set compatible firmware versions for this slave
     * @param versions Array of version strings (e.g., {"0.1.0", "0.1.1"})
     * @param count Number of versions in array
     */
    void setCompatibleVersions(const char** versions, size_t count);

    /**
     * @brief Check if slave firmware version is compatible
     * @return true if version matches compatibility list or no list set
     */
    bool isVersionCompatible() const { return _boardInfo.versionCompatible; }

private:
    void handlePacket(uint8_t type, const uint8_t* payload, size_t len);
    CommandResult sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len);
    CommandResult waitForAckNack();

    GunFxStatusCallback _statusCallback;
    GunFxReadyCallback _readyCallback;
    GunFxErrorCallback _errorCallback;

    GunFxStatus _lastStatus;
    bool _slaveReady = false;
    char _slaveName[65] = "";
    GunFxBoardInfo _boardInfo;
    
    // ACK/NACK state
    unsigned long _commandTimeoutMs = 1000;
    bool _blockingMode = true;
    volatile bool _pendingAckNack = false;
    volatile bool _receivedAck = false;
    volatile bool _receivedNack = false;
    uint8_t _lastNackErrorCode = 0;
    char _lastNackReason[64] = "";
    CommandResult _lastCommandResult;
    
    // Version compatibility
    const char** _compatibleVersions = nullptr;
    size_t _compatibleVersionCount = 0;
    
    // USB host reference for text INIT
    UsbHost* _usbHostRef = nullptr;
    
    bool checkVersionCompatibility(const char* version);
};

// ============================================================================
// GunFxSerialSlave - Binary Protocol Slave (GunFX Pico)
// ============================================================================

/**
 * @brief Slave-side GunFX serial communication (binary protocol)
 * 
 * Used by GunFX Pico to receive commands from HubFX master
 * and send status/telemetry back. Implements both IGunFxSlave for
 * protocol-agnostic usage and IBinaryCommandHandler for use with
 * BinaryCommandRouter (Chain of Responsibility pattern).
 * 
 * Can be used standalone (with process()) or with BinaryCommandRouter:
 *   // Standalone:
 *   slave.process();  // Reads serial, decodes, handles packets
 *   
 *   // With router:
 *   router.addHandler(&slave);  // Router handles serial reading
 *   router.process();
 */
class GunFxSerialSlave : public IGunFxSlave, public IBinaryCommandHandler {
public:
    GunFxSerialSlave() = default;
    ~GunFxSerialSlave() override = default;

    // Delete copy operations
    GunFxSerialSlave(const GunFxSerialSlave&) = delete;
    GunFxSerialSlave& operator=(const GunFxSerialSlave&) = delete;

    // ========================================================================
    // IGunFxSlave Implementation
    // ========================================================================

    bool begin(Stream* serial, const char* moduleName = "GunFX") override;
    void end() override;
    int process() override;  // Standalone mode - reads serial internally

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

    // ========================================================================
    // IBinaryCommandHandler Implementation (for use with BinaryCommandRouter)
    // ========================================================================
    
    /**
     * @brief Try to process a binary packet (Chain of Responsibility)
     * @param type Packet type byte
     * @param payload Packet payload
     * @param len Payload length
     * @return Handled if this is a GunFX command, NotMyCommand otherwise
     */
    CommandHandleResult tryProcessPacket(uint8_t type, const uint8_t* payload, size_t len) override;
    
    const char* handlerName() const override { return "GunFxBinarySlave"; }

private:
    // Internal packet handler (called by both process() and tryProcessPacket())
    CommandHandleResult handlePacket(uint8_t type, const uint8_t* payload, size_t len);
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
    GunFxSmokeSettingsCallback _smokeSettingsCallback;
    GunFxStatusRequestCallback _statusRequestCallback;
};

#endif // SERIAL_GUNFX_H
