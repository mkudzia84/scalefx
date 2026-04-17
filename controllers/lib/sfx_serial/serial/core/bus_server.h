/*
 * Bus Server — Base Class for Pico Server Command Handlers
 *
 * Provides BusServer (base for module handlers) and CoreCommandServer
 * (system command handler). Both extend ICommandHandler for use with
 * CommandRouter's Chain of Responsibility pattern.
 *
 * BusServer — Common boilerplate shared by all ScaleFX server handlers
 * (GunFxServer, LightFxServer, GearControlServer, CoreCommandServer).
 *
 * Common functionality:
 *   - begin()/end() lifecycle with Stream* serial
 *   - sendRawPacket() COBS-encodes and writes to serial
 *   - sendAck()/sendNack()/sendError() response helpers
 *   - ICommandHandler interface (tryProcess, handlerName, _currentTag)
 *
 * CoreCommandServer — Handles core system commands (INIT, SHUTDOWN, REBOOT,
 * BOOTSEL, KEEPALIVE, STATUS_REQ, I2C_SCAN) on server devices. Extends
 * BusServer for the core packet range (0xF0-0xFF).
 *
 * Module Subclasses add:
 *   - Module-specific handleModulePacket() switch cases
 *   - Module-specific callback registrations
 *   - Module-specific response methods (sendStatus, sendCalibStatus, etc.)
 *
 * Usage:
 *   class GunFxServer : public BusServer {
 *   public:
 *       const char* handlerName() const override { return "GunFxServer"; }
 *   protected:
 *       CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
 *       const char* getModuleErrorMessage(uint8_t code) override;
 *       uint8_t moduleRangeLow() const override { return 0x01; }
 *       uint8_t moduleRangeHigh() const override { return 0x2F; }
 *   };
 */

#ifndef SERIAL_BUS_SERVER_H
#define SERIAL_BUS_SERVER_H

#include <Arduino.h>
#include "core.h"

// ============================================================================
// BusServer Class
// ============================================================================

/**
 * @brief Base class for Pico server command handlers
 *
 * Extends ICommandHandler with common serial I/O (COBS encode, ACK/NACK/ERROR).
 * Module-specific servers (GunFxServer, LightFxServer, etc.) extend this.
 *
 * When used with SfxServer + CommandRouter (the normal pattern), only
 * tryProcess() is called — the base handles packet range checking and
 * delegates to the subclass's handleModulePacket().
 */
class BusServer : public ICommandHandler {
public:
    BusServer() = default;
    ~BusServer() override = default;

    BusServer(const BusServer&) = delete;
    BusServer& operator=(const BusServer&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize with serial stream
     * @param serial The Stream to read/write packets on
     * @return true if initialized successfully
     */
    bool begin(Stream* serial);

    /**
     * @brief Shut down and release serial
     */
    void end();

    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================

    /**
     * @brief Route packet to module handler if type is in range
     *
     * Checks moduleRangeLow()..moduleRangeHigh(). If in range, delegates
     * to handleModulePacket(). Otherwise returns NotMyCommand.
     */
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override;

    // handlerName() must be overridden by subclass

    // ========================================================================
    // Response Methods
    // ========================================================================

    /**
     * @brief Send ACK with current tag
     */
    int sendAck();

    /**
     * @brief Send NACK with error code and optional reason string
     *
     * If reason is empty/null, looks up module error message via
     * getModuleErrorMessage().
     */
    int sendNack(uint8_t errorCode, const char* reason = nullptr);

    /**
     * @brief Send ERROR (unsolicited, uses TAG_ASYNC)
     */
    int sendError(uint8_t errorCode, const char* message = nullptr);

    /**
     * @brief Send a raw COBS-encoded packet
     */
    int sendRawPacket(uint8_t type, uint8_t tag, const uint8_t* payload = nullptr, size_t len = 0);

    // ========================================================================
    // State
    // ========================================================================

    bool isInitialized() const { return _initialized; }
    Stream* serial() const { return _serial; }

protected:
    // ========================================================================
    // Virtual Hooks — Override in subclasses
    // ========================================================================

    /**
     * @brief Handle a module-specific packet type
     *
     * Called by tryProcess() after confirming the type is in the module's range.
     * Override to implement the switch/case dispatch for module commands.
     *
     * @param type Packet type (guaranteed to be in moduleRangeLow..moduleRangeHigh)
     * @param payload Payload data
     * @param len Payload length
     * @return CommandHandleResult (Handled, NotMyCommand, Error)
     */
    virtual CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) = 0;

    /**
     * @brief Get the low bound of this module's packet type range (inclusive)
     */
    virtual uint8_t moduleRangeLow() const = 0;

    /**
     * @brief Get the high bound of this module's packet type range (inclusive)
     */
    virtual uint8_t moduleRangeHigh() const = 0;

    /**
     * @brief Get module-specific error message for a NACK error code
     *
     * Called by sendNack when no explicit reason string is provided.
     * Override to return module-specific error text.
     */
    virtual const char* getModuleErrorMessage(uint8_t code) {
        return SerialError::getMessage(code);
    }

    // ========================================================================
    // Protected State — Accessible to subclasses
    // ========================================================================

    Stream* _serial = nullptr;
    bool _initialized = false;
};

// ============================================================================
// CoreCommandServer — System Command Handler (extends BusServer)
// ============================================================================

/**
 * @brief Handles core system commands on server devices
 *
 * Processes INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE, STATUS_REQ,
 * and I2C_SCAN commands. Extends BusServer for the core packet range
 * (0xF0-0xFF) and reuses BusServer's sendAck/sendNack/sendRawPacket.
 *
 * Used by SfxServer as the first handler in the CommandRouter chain.
 */
class CoreCommandServer : public BusServer {
public:
    CoreCommandServer() = default;
    ~CoreCommandServer() override = default;

    /**
     * @brief Initialize the handler
     * @param serial Serial port for sending responses (server side uses Serial)
     */
    void begin(Stream* serial);

    /**
     * @brief Set board information for INIT_READY response
     */
    void setBoardInfo(const char* deviceName, const char* firmwareVersion,
                      const char* platform, uint32_t cpuMHz, uint32_t freeRam,
                      uint32_t buildNumber = 0);

    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================

    const char* handlerName() const override { return "CoreCommandServer"; }

    /**
     * @brief Override tryProcess to bypass BusServer's _initialized check.
     *
     * Core commands (especially INIT) must be processed even before the
     * INIT handshake is complete. BusServer's default tryProcess() checks
     * _initialized which means "serial ready", but we need to process
     * packets as soon as the serial port is set.
     */
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override;

    /**
     * @brief Update last activity timestamp
     */
    void updateActivity() { _lastActivityMs = millis(); }

    /**
     * @brief Check for connection timeout
     * @param timeoutMs Timeout in milliseconds (0 to disable)
     * @return true if timed out
     */
    bool checkTimeout(unsigned long timeoutMs);

    /**
     * @brief Get time since last activity
     */
    unsigned long lastActivityMs() const { return _lastActivityMs; }

    /**
     * @brief Check if INIT has been received (connection established)
     */
    bool isInitialized() const { return _initReceived; }

    /**
     * @brief Check if verbose status updates are enabled
     */
    bool isVerbose() const { return (_initFlags & InitFlags::VERBOSE) != 0; }

    /**
     * @brief Get the current board operational state
     */
    uint8_t boardState() const { return _boardState; }

    /**
     * @brief Set the board operational state
     *
     * Called by SfxServer to manage state transitions (IDLE → STANDALONE,
     * IDLE/STANDALONE → SLAVE/DIRECT, SLAVE/DIRECT → IDLE/STANDALONE).
     */
    void setBoardState(uint8_t state) { _boardState = state; }

    /**
     * @brief Mark a file transfer as active/inactive
     *
     * While a transfer is active, sendStatusUpdate() is suppressed to
     * avoid contention on the serial channel. Call with true when any
     * file operation begins (upload, download, list, tree) and false
     * when it completes or is cancelled.
     */
    void setTransferActive(bool active) { _transferActive = active; }

    /**
     * @brief Check if a file transfer is currently active
     */
    bool isTransferActive() const { return _transferActive; }

    /**
     * @brief Reset state (on connection loss or new INIT)
     */
    void reset();

    // Callbacks
    void onInit(CoreInitCallback callback) { _initCallback = callback; }
    void onShutdown(CoreShutdownCallback callback) { _shutdownCallback = callback; }
    void onReboot(CoreRebootCallback callback) { _rebootCallback = callback; }
    void onBootsel(CoreBootselCallback callback) { _bootselCallback = callback; }
    void onKeepalive(CoreKeepaliveCallback callback) { _keepaliveCallback = callback; }

    /**
     * @brief Register callback for appending module-specific data to STATUS response
     *
     * The callback receives a buffer pointer and max length, and should write
     * module-specific status bytes. The bytes are appended after the 22-byte
     * core header: [counter:u32LE][uptime:u32LE][freeRam:u32LE]
     *              [lastActivity_ms:u32LE][keepaliveCount:u32LE]
     *              [boardState:u8][initFlags:u8].
     */
    void onStatusData(StatusDataCallback callback) { _statusDataCallback = callback; }

    /**
     * @brief Register callback for I2C bus scan
     *
     * When I2C_SCAN packet is received, the callback is invoked to perform
     * the actual bus scan. The result is sent back as I2C_SCAN_RESULT.
     */
    void onI2CScan(I2CScanCallback callback) { _i2cScanCallback = callback; }

    // Statistics
    uint32_t commandCounter() const { return _commandCounter; }
    uint32_t keepaliveCounter() const { return _keepaliveCounter; }

    /**
     * @brief Update free RAM value for STATUS response (call periodically)
     *
     * The core STATUS header includes freeRam. Since CoreCommandServer
     * stores this from setBoardInfo(), call this to keep it current.
     * Example: coreServer.updateFreeRam(rp2040.getFreeHeap());
     */
    void updateFreeRam(uint32_t freeRam) { _boardInfo.freeRamBytes = freeRam; }

    /**
     * @brief Send I2C_SCAN_RESULT packet
     */
    void sendI2CScanResult(const I2CScanResult& result);

    /**
     * @brief Send a STATUS_UPDATE async packet (verbose mode)
     *
     * Only sends if verbose flag was set in INIT. Uses TAG_ASYNC.
     *
     * @param source Module source ID (StatusUpdateSource::*)
     * @param updateType Update type code (StatusUpdateType::* or module-specific)
     * @param data Update-specific data
     * @param dataLen Length of data
     */
    void sendStatusUpdate(uint8_t source, uint8_t updateType,
                          const uint8_t* data = nullptr, size_t dataLen = 0);

    /**
     * @brief Set periodic status broadcast interval (verbose mode)
     *
     * When verbose flag is set and interval > 0, tickStatusBroadcast() will
     * periodically call the registered onStatusData callback and emit the
     * module data as a STATUS_UPDATE(STATUS_BROADCAST) packet.
     *
     * @param interval_ms Broadcast interval in ms (0 = disabled, default 200)
     */
    void setStatusBroadcastInterval(uint32_t interval_ms) { _statusBroadcastInterval_ms = interval_ms; }

    /**
     * @brief Set the source module ID for status broadcast packets
     * @param source StatusUpdateSource::* constant (e.g., GEARCONTROL)
     */
    void setStatusBroadcastSource(uint8_t source) { _statusBroadcastSource = source; }

    /**
     * @brief Tick the periodic status broadcast (call from SfxServer::loop())
     *
     * Checks if verbose mode is active, interval has elapsed, and no transfer
     * is in progress. If all conditions met, calls the status data callback
     * and emits the result as STATUS_UPDATE(STATUS_BROADCAST).
     */
    void tickStatusBroadcast();

protected:
    // ========================================================================
    // BusServer Virtual Hooks
    // ========================================================================

    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override { return 0xEF; }  // Includes STATUS_UPDATE (0xEF)
    uint8_t moduleRangeHigh() const override { return 0xFF; }
    const char* getModuleErrorMessage(uint8_t code) override {
        return CoreError::getMessage(code);
    }

private:
    void sendInitReady();
    void sendIdentify();
    void handleInit(const uint8_t* payload, size_t len);
    void sendStatus();

    CoreBoardInfo _boardInfo;
    bool _initReceived = false;
    uint8_t _initFlags = InitFlags::NONE;
    uint8_t _boardState = BoardState::IDLE;
    bool _transferActive = false;
    unsigned long _lastActivityMs = 0;
    unsigned long _prevActivityMs = 0;
    uint32_t _commandCounter = 0;
    uint32_t _keepaliveCounter = 0;

    // Periodic status broadcast (verbose mode)
    uint32_t _statusBroadcastInterval_ms = 200;  // Default 200ms (5 Hz)
    unsigned long _lastStatusBroadcast_ms = 0;
    uint8_t _statusBroadcastSource = StatusUpdateSource::CORE;  // Default source

    CoreInitCallback _initCallback;
    CoreShutdownCallback _shutdownCallback;
    CoreRebootCallback _rebootCallback;
    CoreBootselCallback _bootselCallback;
    CoreKeepaliveCallback _keepaliveCallback;
    StatusDataCallback _statusDataCallback;
    I2CScanCallback _i2cScanCallback;
};

#endif // SERIAL_BUS_SERVER_H
