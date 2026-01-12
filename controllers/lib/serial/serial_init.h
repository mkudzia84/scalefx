/*
 * SerialInitHandler - Protocol Negotiation & System Command Handler
 * 
 * Handles the INIT/INIT_READY handshake and system commands.
 * Implements ITextCommandHandler for use with TextCommandRouter (Chain of Responsibility pattern).
 * 
 * Commands handled:
 * - INIT [protocol=text|binary] [keepalive=N] - Protocol negotiation
 * - SHUTDOWN - Safe shutdown
 * - REBOOT - System reboot  
 * - BOOTSEL - Enter bootloader
 * - KEEPALIVE - Connection keep-alive
 * 
 * Design Pattern: Chain of Responsibility
 * - This handler processes system commands
 * - Returns NotMyCommand for protocol-specific commands
 * 
 * Usage:
 *   TextCommandRouter router;
 *   SerialInitHandler initHandler;
 *   
 *   router.begin(&Serial, [&](uint8_t code, const char* cmd) {
 *       slave.sendNack(code, cmd);  // Send NACK for unknown commands
 *   });
 *   router.addHandler(&initHandler);
 *   router.addHandler(&protocolSlave);
 *   
 *   initHandler.begin(&Serial, "GunFX-1234");
 *   initHandler.onInitComplete([](ProtocolMode mode) { ... });
 *   
 *   void loop() {
 *       router.process();
 *   }
 */

#ifndef SERIAL_INIT_H
#define SERIAL_INIT_H

#include <Arduino.h>
#include <functional>
#include "serial_protocol.h"
#include "serial_bus_text.h"
#include "serial_gunfx_types.h"
#include "serial_command_handler.h"

// ============================================================================
//  BOARD INFO - Use GunFxBoardInfo from serial_gunfx_types.h
// ============================================================================

using BoardInfo = GunFxBoardInfo;

// ============================================================================
//  CALLBACK TYPES
// ============================================================================

using InitCompleteCallback = std::function<void(ProtocolMode mode)>;
using InitResetCallback = std::function<void()>;
using ConnectionLossCallback = std::function<void()>;
using ShutdownCallback = std::function<void()>;
using RebootCallback = std::function<void()>;
using BootselCallback = std::function<void()>;

// ============================================================================
//  SERIAL INIT HANDLER (SLAVE SIDE)
// ============================================================================

/**
 * SerialInitHandler - Handles protocol negotiation and system commands
 * 
 * Implements ITextCommandHandler for use in Chain of Responsibility pattern
 * with TextCommandRouter. Always operates in text mode for system commands.
 */
class SerialInitHandler : public ITextCommandHandler {
public:
    SerialInitHandler() = default;
    ~SerialInitHandler() override = default;

    // ========================================================================
    // ITextCommandHandler Implementation
    // ========================================================================

    /**
     * @brief Try to process a system command
     * @param line The command line to process
     * @return Handled if recognized (INIT/SHUTDOWN/REBOOT/BOOTSEL/KEEPALIVE), NotMyCommand otherwise
     */
    CommandHandleResult tryProcessCommand(const char* line) override;

    /**
     * @brief Get handler name for debugging
     */
    const char* handlerName() const override { return "InitHandler"; }

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * Initialize the handler
     * @param serial Stream to use for communication
     * @param moduleName Device name (e.g., "GunFX-1234")
     * @return true if successful
     */
    bool begin(Stream* serial, const char* moduleName);

    /**
     * Set board information for INIT_READY response
     * @param firmwareVersion Version string WITHOUT "v" prefix (e.g., "0.1.0")
     * @param buildNumber Build number (incremented with each build)
     * @param platform Platform name (e.g., "RP2040")
     * @param cpuFrequencyMHz CPU frequency in MHz
     * @param freeRamBytes Free RAM in bytes
     */
    void setBoardInfo(const char* firmwareVersion, uint32_t buildNumber,
                      const char* platform, uint32_t cpuFrequencyMHz, uint32_t freeRamBytes);

    /**
     * Reset handler state (called automatically on new INIT)
     */
    void reset();

    // ========================================================================
    // Connection Management
    // ========================================================================

    /**
     * Update last activity timestamp
     * Called automatically when tryProcessCommand handles a command.
     * Call manually when other handlers process commands.
     */
    void updateActivity() { _lastActivityMs = millis(); }

    /**
     * Check for connection timeout
     * @param timeoutMs Timeout in milliseconds (0 to disable)
     * @return true if connection timed out
     */
    bool checkTimeout(unsigned long timeoutMs);

    // ------------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------------

    bool isInitialized() const { return _initialized; }
    ProtocolMode protocolMode() const { return _protocolMode; }
    const char* moduleName() const { return _moduleName; }
    const BoardInfo& boardInfo() const { return _boardInfo; }
    unsigned long lastActivityMs() const { return _lastActivityMs; }
    
    /**
     * @brief Get negotiated keepalive interval (0 = disabled)
     * @return Interval in milliseconds, or 0 if keepalive monitoring disabled
     */
    unsigned long keepaliveIntervalMs() const { return _keepaliveIntervalMs; }
    
    /**
     * @brief Get recommended timeout for keepalive monitoring (1.5x interval)
     * @return Timeout in milliseconds, or 0 if keepalive disabled
     */
    unsigned long keepaliveTimeoutMs() const { return _keepaliveIntervalMs > 0 ? (_keepaliveIntervalMs * 3 / 2) : 0; }

    // ------------------------------------------------------------------------
    // Callbacks
    // ------------------------------------------------------------------------

    /**
     * Called when INIT is complete and protocol mode is determined
     * Use this to set up the appropriate protocol handler
     */
    void onInitComplete(InitCompleteCallback callback) { _initCompleteCallback = callback; }

    /**
     * Called when a new INIT is received (reconnection)
     * Use this to clean up the previous protocol handler
     */
    void onInitReset(InitResetCallback callback) { _initResetCallback = callback; }

    /**
     * Called when SHUTDOWN command is received
     */
    void onShutdown(ShutdownCallback callback) { _shutdownCallback = callback; }

    /**
     * Called when REBOOT command is received
     */
    void onReboot(RebootCallback callback) { _rebootCallback = callback; }

    /**
     * Called when BOOTSEL command is received
     */
    void onBootsel(BootselCallback callback) { _bootselCallback = callback; }

    /**
     * Called when connection timeout is detected (no messages within timeout period)
     * Use this to implement custom behavior on connection loss (e.g., safe shutdown)
     */
    void onConnectionLoss(ConnectionLossCallback callback) { _connectionLossCallback = callback; }

private:
    // Stream for sending responses
    Stream* _serial = nullptr;

    // Module info
    char _moduleName[32] = "";
    BoardInfo _boardInfo;

    // State
    bool _initialized = false;
    ProtocolMode _protocolMode = ProtocolMode::Text;
    unsigned long _lastActivityMs = 0;
    unsigned long _keepaliveIntervalMs = 0;

    // Callbacks
    InitCompleteCallback _initCompleteCallback = nullptr;
    InitResetCallback _initResetCallback = nullptr;
    ConnectionLossCallback _connectionLossCallback = nullptr;
    ShutdownCallback _shutdownCallback = nullptr;
    RebootCallback _rebootCallback = nullptr;
    BootselCallback _bootselCallback = nullptr;

    // Internal methods
    void handleInit(const char* args);
    void sendInitReady();
};

// ============================================================================
//  SERIAL INIT SENDER (MASTER SIDE)
// ============================================================================

/**
 * SerialInitSender - Sends INIT and parses INIT_READY on master side
 * 
 * Simple helper for master-side protocol negotiation.
 */
class SerialInitSender {
public:
    SerialInitSender() = default;
    ~SerialInitSender() = default;

    /**
     * Send INIT command to slave
     * @param serial Stream to send on
     * @param mode Protocol mode to request
     * @param keepaliveMs Keepalive interval in ms (0 = disabled/off)
     * @return Number of bytes written
     */
    static int sendInit(Stream* serial, ProtocolMode mode, unsigned long keepaliveMs = 0);

    /**
     * Parse INIT_READY response
     * @param line The received line (without newline)
     * @param info Output: parsed board info
     * @return true if successfully parsed
     */
    static bool parseInitReady(const char* line, BoardInfo& info);

    /**
     * Check if a line is an INIT_READY response
     */
    static bool isInitReady(const char* line);
};

#endif // SERIAL_INIT_H
