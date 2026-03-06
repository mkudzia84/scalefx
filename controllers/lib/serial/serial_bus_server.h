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
#include "serial_core.h"

// ============================================================================
// BusServer Class
// ============================================================================

/**
 * @brief Base class for Pico server command handlers
 *
 * Extends ICommandHandler with common serial I/O (COBS encode, ACK/NACK/ERROR).
 * Module-specific servers (GunFxServer, LightFxServer, etc.) extend this.
 *
 * When used with PicoServer + CommandRouter (the normal pattern), only
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
 * Used by PicoServer as the first handler in the CommandRouter chain.
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
    bool isInitReceived() const { return _initReceived; }

    /**
     * @brief Backward-compatible alias for isInitReceived()
     */
    bool isInitialized() const { return _initReceived; }

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
     * module-specific status bytes. The bytes are appended after the 12-byte
     * core header: [counter:u32LE][uptime:u32LE][freeRam:u32LE].
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

protected:
    // ========================================================================
    // BusServer Virtual Hooks
    // ========================================================================

    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override { return 0xF0; }
    uint8_t moduleRangeHigh() const override { return 0xFF; }
    const char* getModuleErrorMessage(uint8_t code) override {
        return CoreError::getMessage(code);
    }

private:
    void sendInitReady();
    void handleInit(const uint8_t* payload, size_t len);
    void sendStatus();

    CoreBoardInfo _boardInfo;
    bool _initReceived = false;
    unsigned long _lastActivityMs = 0;
    uint32_t _commandCounter = 0;
    uint32_t _keepaliveCounter = 0;

    CoreInitCallback _initCallback;
    CoreShutdownCallback _shutdownCallback;
    CoreRebootCallback _rebootCallback;
    CoreBootselCallback _bootselCallback;
    CoreKeepaliveCallback _keepaliveCallback;
    StatusDataCallback _statusDataCallback;
    I2CScanCallback _i2cScanCallback;
};

#endif // SERIAL_BUS_SERVER_H
