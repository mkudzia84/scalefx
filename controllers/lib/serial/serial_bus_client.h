/*
 * Bus Client — Base Class for USB Host Client Controllers
 *
 * Extracts the common boilerplate shared by all ScaleFX client controllers
 * (GunFxClient, LightFxClient, GearControlClient). These run on HubFX and
 * communicate with Pico server boards over USB CDC via SerialBus.
 *
 * Common functionality:
 *   - begin()/process() lifecycle delegating to SerialBus
 *   - sendInit() text-mode INIT handshake
 *   - sendCommand() with tag correlation and blocking wait
 *   - handlePacket() core cases: INIT_READY, ACK, NACK, ERROR
 *   - Version compatibility checking
 *   - ResultQueue integration
 *   - Blocking/non-blocking mode
 *
 * Subclasses add:
 *   - Protocol-specific command methods (triggerOn, ledSet, gearDeploy, etc.)
 *   - Module-specific STATUS parsing (override onModulePacket)
 *   - Module-specific NACK error text (override getModuleErrorMessage)
 *   - Module-specific callbacks (onStatus, onCalibStatus, etc.)
 *
 * Usage:
 *   class GunFxClient : public BusClient {
 *       CommandResult triggerOn(uint16_t rpm) {
 *           uint8_t payload[2];
 *           putU16LE(payload, rpm);
 *           return sendCommand(GunFxPacket::TRIGGER_ON, payload, 2);
 *       }
 *   protected:
 *       void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override {
 *           // Handle STATUS, module-specific response types
 *       }
 *       const char* getModuleErrorMessage(uint8_t code) override {
 *           return GunFxError::getMessage(code);
 *       }
 *   };
 */

#ifndef SERIAL_BUS_CLIENT_H
#define SERIAL_BUS_CLIENT_H

#include <Arduino.h>
#include <functional>
#include "serial_core.h"
#include "serial_bus.h"
#include "serial_result_queue.h"

// Forward declaration
class UsbHost;

// ============================================================================
// Board Info — Common structure for all client controllers
// ============================================================================

/**
 * @brief Board information returned during INIT_READY handshake
 *
 * Parsed from pipe-delimited payload: name|version|platform|cpuMHz|ramBytes
 * Common across all client controller types.
 */
struct BusClientBoardInfo {
    char deviceName[32] = "";
    char firmwareVersion[16] = "";
    char platform[16] = "";
    uint16_t cpuFrequencyMHz = 0;
    uint32_t freeRamBytes = 0;
    bool versionCompatible = true;
};

// ============================================================================
// Callback Types
// ============================================================================

using BusClientReadyCallback = std::function<void(const char* deviceName)>;
using BusClientErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;
using BusClientLogCallback = std::function<void(uint8_t level, uint32_t timestamp_ms, const char* message)>;

// ============================================================================
// BusClient Class
// ============================================================================

/**
 * @brief Base class for USB host client controllers
 *
 * Extends SerialBus with tag-correlated command/response, INIT handshake,
 * version compatibility, and blocking/non-blocking command execution.
 * Module-specific clients (GunFxClient, LightFxClient, etc.) extend this.
 */
class BusClient : public SerialBus {
public:
    BusClient() = default;
    virtual ~BusClient() = default;

    BusClient(const BusClient&) = delete;
    BusClient& operator=(const BusClient&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize with USB host connection
     *
     * Sets up SerialBus, clears state, and registers the internal
     * packet handler that routes to handleCorePacket/onModulePacket.
     */
    bool begin(UsbHost* usbHost, int deviceIndex);

    /**
     * @brief Process incoming packets (call in loop)
     */
    int process();

    // ========================================================================
    // Connection Management
    // ========================================================================

    /**
     * @brief Send INIT command with optional keepalive interval
     * @param keepaliveMs Keepalive interval in ms (0 = disabled)
     * @return Bytes written, -1 on error
     */
    int sendInit(unsigned long keepaliveMs = 0);

    // ========================================================================
    // Command Execution
    // ========================================================================

    /**
     * @brief Send a tagged command and wait for ACK/NACK (if blocking mode)
     *
     * Assigns a correlation tag, sends the packet, and either blocks
     * waiting for a response or returns immediately (non-blocking mode).
     *
     * @param type Packet type byte
     * @param payload Payload data
     * @param len Payload length
     * @return CommandResult (Ack, Nack, Timeout, SendFailed, NotConnected)
     */
    CommandResult sendCommand(uint8_t type, const uint8_t* payload, size_t len);

    // ========================================================================
    // Configuration
    // ========================================================================

    void setCommandTimeout(unsigned long timeoutMs) { _resultQueue.setCommandTimeout_ms(timeoutMs); }
    void setBlockingMode(bool blocking) { _blockingMode = blocking; }
    void setCompatibleVersions(const char** versions, size_t count);

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onReady(BusClientReadyCallback cb) { _readyCallback = cb; }
    void onError(BusClientErrorCallback cb) { _errorCallback = cb; }
    void onLogMessage(BusClientLogCallback cb) { _logCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================

    bool isServerReady() const { return _serverReady; }
    bool isVersionCompatible() const { return _boardInfo.versionCompatible; }
    const char* serverName() const { return _serverName; }
    const BusClientBoardInfo& boardInfo() const { return _boardInfo; }
    CommandResult lastCommandResult() const { return _lastCommandResult; }
    ResultQueue& resultQueue() { return _resultQueue; }

protected:
    // ========================================================================
    // Virtual Hooks — Override in subclasses
    // ========================================================================

    /**
     * @brief Handle module-specific packets (STATUS, custom response types)
     *
     * Called for any packet type NOT handled by the base class
     * (i.e., not INIT_READY, ACK, NACK, ERROR).
     * Override to parse STATUS data, calibration responses, etc.
     *
     * @param type Packet type
     * @param tag Correlation tag
     * @param payload Payload data
     * @param len Payload length
     */
    virtual void onModulePacket(uint8_t type, uint8_t tag,
                                const uint8_t* payload, size_t len) {}

    /**
     * @brief Get module-specific error message for a NACK error code
     *
     * Called when a NACK is received without an inline reason string.
     * Override to provide module-specific error text (e.g., GunFxError::getMessage).
     *
     * @param code Error code from NACK payload
     * @return Human-readable error message
     */
    virtual const char* getModuleErrorMessage(uint8_t code) {
        return SerialError::getMessage(code);
    }

    /**
     * @brief Called after INIT_READY is parsed — override for module init
     *
     * The base class has already populated _boardInfo and _serverName.
     * Override if the module needs to do something on connect (rare).
     */
    virtual void onServerReady() {}

    // ========================================================================
    // Protected State — Accessible to subclasses
    // ========================================================================

    UsbHost* _usbHostRef = nullptr;
    bool _serverReady = false;
    char _serverName[65] = "";
    BusClientBoardInfo _boardInfo;

    bool _blockingMode = true;
    ResultQueue _resultQueue;
    CommandResult _lastCommandResult;

private:
    void handlePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len);
    bool checkVersionCompatibility(const char* version);

    const char** _compatibleVersions = nullptr;
    size_t _compatibleVersionCount = 0;

    BusClientReadyCallback _readyCallback;
    BusClientErrorCallback _errorCallback;
    BusClientLogCallback _logCallback;
};

#endif // SERIAL_BUS_CLIENT_H
