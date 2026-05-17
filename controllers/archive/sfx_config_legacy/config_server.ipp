/*
 * Config Protocol Server — Template Implementation
 *
 * Included at the bottom of config_server.h.
 */

#ifndef CONFIG_SERVER_IPP
#define CONFIG_SERVER_IPP

#define CONFIG_SRV_LOG(fmt, ...) SFX_LOG_DEBUG("[ConfigSrv] " fmt, ##__VA_ARGS__)

// ============================================================================
// Packet Dispatch
// ============================================================================

template<typename TConfigStore>
CommandHandleResult ConfigServerT<TConfigStore>::handleModulePacket(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case HubFxPacket::CONFIG_RELOAD:
            handleReload(payload, len);
            return CommandHandleResult::Handled;
        case HubFxPacket::CONFIG_STATUS:
            handleStatus();
            return CommandHandleResult::Handled;
        case HubFxPacket::CONFIG_SAVE:
            handleSave(payload, len);
            return CommandHandleResult::Handled;
        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// CONFIG_RELOAD (0x90)
// Wire: [] (no payload) or [pathLen:u8][path:str] (custom path)
// Response: ACK on success, NACK + CONFIG_ERROR on failure
// ============================================================================

template<typename TConfigStore>
void ConfigServerT<TConfigStore>::handleReload(const uint8_t* payload, size_t len) {
    const char* path = nullptr;
    char pathBuf[128] = {};

    // Optional: custom file path in payload
    if (len >= 2) {
        uint8_t pathLen = payload[0];
        if (pathLen > 0 && (size_t)(1 + pathLen) <= len && pathLen < sizeof(pathBuf)) {
            memcpy(pathBuf, &payload[1], pathLen);
            pathBuf[pathLen] = '\0';
            path = pathBuf;
        }
    }

    CONFIG_SRV_LOG("Reload config: %s", path ? path : TConfigStore::defaultPath());

    auto result = _store.loadFromFile(path);
    if (result.ok) {
        SFX_LOG_INFO("[Config] Reloaded: %u bytes, valid",
                     _store.fileSize());
        if (_onReloaded) _onReloaded(_store.data());
        this->sendAck();
    } else if (result.populated && !result.validated) {
        // Loaded but validation failed — report as warning ACK with message
        SFX_LOG_WARN("[Config] Loaded with validation warnings: %s",
                     _store.lastError());
        // Send ACK since data IS loaded, but include validation note
        if (_onReloaded) _onReloaded(_store.data());
        this->sendAck();
    } else {
        // Parse or populate failed
        SFX_LOG_WARN("[Config] Reload failed: %s", _store.lastError());
        this->sendNack(HubFxError::CONFIG_ERROR, _store.lastError());
    }
}

// ============================================================================
// CONFIG_STATUS (0x91) → CONFIG_STATUS_RESP (0x92)
// Wire: [loaded:u8][fileSize:u16LE][validOk:u8]
// ============================================================================

template<typename TConfigStore>
void ConfigServerT<TConfigStore>::handleStatus() {
    CONFIG_SRV_LOG("Config status request");

    // Validate current config
    char validErr[64] = {};
    bool validOk = _store.isLoaded() ? _store.validate(validErr, sizeof(validErr)) : false;

    uint8_t buf[4];
    buf[0] = _store.isLoaded() ? 1 : 0;        // loaded
    SfxWire::putU16LE(&buf[1], _store.fileSize());  // fileSize
    buf[3] = validOk ? 1 : 0;                   // validOk

    this->sendRawPacket(HubFxPacket::CONFIG_STATUS_RESP, this->currentTag(), buf, sizeof(buf));
}

// ============================================================================
// CONFIG_SAVE (0xAC)
// Wire: [] (no payload) or [pathLen:u8][path:str] (custom path)
// Response: ACK on success, NACK + CONFIG_ERROR on failure
// ============================================================================

template<typename TConfigStore>
void ConfigServerT<TConfigStore>::handleSave(const uint8_t* payload, size_t len) {
    const char* path = nullptr;
    char pathBuf[128] = {};

    // Optional: custom file path in payload
    if (len >= 2) {
        uint8_t pathLen = payload[0];
        if (pathLen > 0 && (size_t)(1 + pathLen) <= len && pathLen < sizeof(pathBuf)) {
            memcpy(pathBuf, &payload[1], pathLen);
            pathBuf[pathLen] = '\0';
            path = pathBuf;
        }
    }

    CONFIG_SRV_LOG("Save config: %s", path ? path : TConfigStore::defaultPath());

    if (!_store.hasRawYaml()) {
        this->sendNack(HubFxError::CONFIG_ERROR, "No config loaded to save");
        return;
    }

    auto result = _store.saveToFile(path);
    if (result.ok) {
        SFX_LOG_INFO("[Config] Saved: %s (%u bytes)",
                     path ? path : TConfigStore::defaultPath(),
                     _store.fileSize());
        this->sendAck();
    } else {
        SFX_LOG_WARN("[Config] Save failed: %s", _store.lastError());
        this->sendNack(HubFxError::CONFIG_ERROR, _store.lastError());
    }
}

#endif // CONFIG_SERVER_IPP
