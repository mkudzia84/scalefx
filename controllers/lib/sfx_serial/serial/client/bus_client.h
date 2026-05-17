/*
 * Bus Client — Base Class for USB Host Client Controllers
 *
 * Extracts the common boilerplate shared by all ScaleFX client controllers
 * (GunFxClient, LightFxClient, GearControlClient). These run on HubFX and
 * communicate with Pico server boards over USB CDC via SerialBus.
 *
 * Common functionality:
 *   - begin()/process() lifecycle delegating to SerialBus
 *   - sendInit() binary INIT handshake
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
#include "serial/core/core.h"
#include "bus.h"
#include "result_queue.h"

// ============================================================================
// Board Info — Common structure for all client controllers
// ============================================================================

/**
 * @brief Board information returned during INIT_READY handshake
 *
 * Extends CoreBoardInfo (from core.h) with client-side version compatibility
 * tracking. Decoded from binary length-prefixed INIT_READY payload via
 * CorePayload::decodeInitReady().
 */
struct BusClientBoardInfo : public CoreBoardInfo {
    bool versionCompatible = true;
};

// ============================================================================
// Callback Types
// ============================================================================

using BusClientReadyCallback = std::function<void(const char* deviceName)>;
using BusClientErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;
using BusClientLogCallback = std::function<void(uint8_t level, uint32_t timestamp_ms, const char* message)>;

/// Fires for any unsolicited inbound packet (tag == TAG_ASYNC). Solicited
/// responses to sendCommand() are matched by tag inside the result queue and
/// do NOT fire this — used by HubFX auto-routing to forward slave-range async
/// packets (LANDING_LIGHT_STATUS, GEAR_DOOR_STATUS, ...) upstream verbatim.
using BusClientAsyncCallback = std::function<void(uint8_t type, const uint8_t* payload, size_t len)>;

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
     * @brief Initialize with USB device index
     *
     * Sets up SerialBus (which uses UsbHost::instance() singleton),
     * clears state, and registers the internal packet handler that
     * routes to handleCorePacket/onModulePacket.
     */
    bool begin(int deviceIndex);

    /**
     * @brief Process incoming packets (call in loop)
     */
    int process();

    // ========================================================================
    // Connection Management
    // ========================================================================

    /**
     * @brief Send binary INIT packet to start handshake
     * @return Bytes written, -1 on error
     *
     * Sends a COBS-framed INIT packet. The server responds with INIT_READY.
     * Resets _serverReady so awaitSlaveReady() properly waits for the response.
     * Keepalive interval is configured client-side via setKeepaliveInterval().
     */
    int sendInit();

    /**
     * @brief Send IDENTIFY packet to query board info without triggering init
     * @return Bytes written, -1 on error
     *
     * Non-destructive: returns the same payload as INIT_READY but does NOT
     * trigger the server's onInit() callback or change its state.
     * Use for slave discovery (identify type by name prefix).
     */
    int sendIdentify();

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
    void onAsyncPacket(BusClientAsyncCallback cb) { _asyncCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================

    bool isServerReady() const { return _serverReady; }
    bool isVersionCompatible() const { return _boardInfo.versionCompatible; }
    const char* serverName() const { return _serverName; }
    const BusClientBoardInfo& boardInfo() const { return _boardInfo; }
    CommandResult lastCommandResult() const { return _lastCommandResult; }
    ResultQueue& resultQueue() { return _resultQueue; }

    /**
     * @brief Raw response capture — last non-ACK/NACK packet received
     *
     * Populated in handlePacket() for packets that go to onModulePacket()
     * (STATUS, module-specific responses). Useful for forwarding opaque
     * response data through a routing layer (e.g., CoreServer STATUS routing).
     */
    uint8_t lastResponseType() const { return _lastResponseType; }
    const uint8_t* lastResponsePayload() const { return _lastResponsePayload; }
    size_t lastResponseLen() const { return _lastResponseLen; }

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
    BusClientAsyncCallback _asyncCallback;

    // Raw response capture (for routing layers that forward opaque packets)
    static constexpr size_t RAW_RESPONSE_MAX = 128;  // STATUS payloads are ~40-60 bytes
    uint8_t _lastResponseType = 0;
    uint8_t _lastResponsePayload[RAW_RESPONSE_MAX] = {};
    size_t _lastResponseLen = 0;
};

#endif // SERIAL_BUS_CLIENT_H
