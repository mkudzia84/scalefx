/*
 * Config Protocol Server — BusServer Handler for Config Commands
 *
 * ConfigServerT<TConfigStore> handles CONFIG_RELOAD (0x90) and
 * CONFIG_STATUS (0x91) commands. It delegates to a ConfigStore<TSchema>
 * instance for YAML parsing, population, and validation.
 *
 * The server does NOT depend on any storage module directly. Instead,
 * file reading is injected via a callback (FileReadFunc) which the
 * controller firmware sets up to bridge to SdCardModule, FlashModule,
 * or any other storage backend.
 *
 * Template parameter:
 *   TConfigStore — ConfigStore<SomeSchema> (or compatible type)
 *                  Must provide: loadFromFile(), loadFromString(),
 *                  isLoaded(), fileSize(), lastError(), data(),
 *                  defaultPath(), setFileReader(), validate()
 *
 * Usage:
 *   using MyConfigStore = ConfigStore<MyBoardSchema>;
 *   ConfigServerT<MyConfigStore> configServer;
 *   configServer.store().setFileReader(myFileReader);
 *   configServer.store().loadFromFile();  // Initial load
 *   server.addModuleHandler(&configServer);
 *
 * Protocol:
 *   CONFIG_RELOAD (0x90): [] or [pathLen:u8][path:str]
 *     → ACK on success
 *     → NACK CONFIG_ERROR + reason text on failure
 *
 *   CONFIG_STATUS (0x91): []
 *     → CONFIG_STATUS_RESP (0x92): [loaded:u8][fileSize:u16LE][validOk:u8]
 */

#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include <platform/diag_log.h>

// ============================================================================
// Config Protocol Server
// ============================================================================

/**
 * @brief Protocol server for configuration management commands
 *
 * Handles CONFIG_RELOAD (0x90), CONFIG_STATUS (0x91), and CONFIG_SAVE (0xAC).
 * CONFIG_SAVE is non-contiguous with the other config packets, so this
 * server overrides tryProcess() for multi-range routing.
 *
 * @tparam TConfigStore ConfigStore<TSchema> type
 */
template<typename TConfigStore>
class ConfigServerT : public BusServer {
public:
    ConfigServerT() = default;

    const char* handlerName() const override { return "ConfigServer"; }

    // ========================================================================
    // Config Store Access
    // ========================================================================

    /** @brief Get the config store (for initial load, reader setup, etc.) */
    TConfigStore& store() { return _store; }
    const TConfigStore& store() const { return _store; }

    // ========================================================================
    // Convenience: Load config (call during setup)
    // ========================================================================

    /**
     * @brief Load config from file via the store's file reader
     * @param path File path (nullptr = schema default)
     * @return true if loaded and validated
     */
    bool loadConfig(const char* path = nullptr) {
        auto result = _store.loadFromFile(path);
        if (result.ok) {
            SFX_LOG_INFO("[Config] Loaded: %s (%u bytes)",
                         path ? path : TConfigStore::defaultPath(),
                         _store.fileSize());
        } else {
            SFX_LOG_WARN("[Config] Load failed: %s", _store.lastError());
        }
        return result.ok;
    }

protected:
    // ========================================================================
    // BusServer Overrides
    // ========================================================================

    /**
     * @brief Override tryProcess for non-contiguous packet routing.
     *
     * Config packets span two ranges:
     *   - 0x90-0x92 (CONFIG_RELOAD, CONFIG_STATUS, CONFIG_STATUS_RESP)
     *   - 0xAC      (CONFIG_SAVE)
     *
     * The default BusServer::tryProcess() only checks a single
     * moduleRangeLow..moduleRangeHigh range, so we override to handle both.
     */
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload,
                                   size_t len) override {
        if (!this->isInitialized() || !this->serial()) {
            return CommandHandleResult::NotMyCommand;
        }
        if ((type >= HubFxPacket::CONFIG_RELOAD &&
             type <= HubFxPacket::CONFIG_STATUS_RESP) ||
            type == HubFxPacket::CONFIG_SAVE) {
            return handleModulePacket(type, payload, len);
        }
        return CommandHandleResult::NotMyCommand;
    }

    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;

    uint8_t moduleRangeLow()  const override { return HubFxPacket::CONFIG_RELOAD; }
    uint8_t moduleRangeHigh() const override { return HubFxPacket::CONFIG_SAVE; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    TConfigStore _store;

    void handleReload(const uint8_t* payload, size_t len);
    void handleStatus();
    void handleSave(const uint8_t* payload, size_t len);
};

// ============================================================================
// Template Implementation
// ============================================================================

#include "config_server.ipp"

#endif // CONFIG_SERVER_H
