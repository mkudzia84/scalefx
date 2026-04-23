/*
 * Multi Config Server — Path-Routed Multi-Store Config Handler
 *
 * Hubs (and any future controller that splits configuration across multiple
 * YAML files per Rule 26) own one ConfigStore<TSchema> per file. This server
 * routes the existing CONFIG_RELOAD (0x90) / CONFIG_STATUS (0x91) /
 * CONFIG_SAVE (0xAC) wire packets across all registered stores by matching
 * the path payload against each store's defaultPath().
 *
 * Wire protocol (unchanged — ConfigServerT extension, see config_server.ipp):
 *   CONFIG_RELOAD: [] or [pathLen:u8][path:str]
 *     - With path: reload that store only.
 *     - Without path: reload ALL stores; first failure aborts and NACKs.
 *   CONFIG_SAVE:   [] or [pathLen:u8][path:str]
 *     - With path: save that store.
 *     - Without path: NACK — multi-store save is ambiguous.
 *   CONFIG_STATUS: []
 *     - Aggregate: loaded=ALL stores loaded?, fileSize=sum, validOk=AND.
 *
 * Why this isn't ConfigServerT<TStore>:
 *   ConfigServerT is templated on a single store type. Trying to template
 *   it on N stores forces a tuple/typelist with virtual-dispatch tax at
 *   the call site anyway. Type-erasing each store via IConfigStoreFacade
 *   keeps the wire dispatch code path-routed and non-templated, while
 *   each ConfigStore stays statically typed for its onLoaded callback.
 *
 * onLoaded callbacks:
 *   Per-store. Register on each ConfigStore directly (typed Data&), not on
 *   this server. This server fires the load; the store fires the callback.
 *
 * Standalone slaves (LightFX, GearControl) keep using ConfigServerT<TStore>
 * unchanged — single-file controllers do not need this.
 */

#ifndef MULTI_CONFIG_SERVER_H
#define MULTI_CONFIG_SERVER_H

#include <cstring>

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include <platform/diag_log.h>

#include "../config/config_store.h"

// ============================================================================
// IConfigStoreFacade — type-erased ConfigStore handle
// ============================================================================
//
// MultiConfigServer dispatches by path, so it never needs the typed Data
// of the underlying store — only the "load / save / status" verbs. The
// facade hides TSchema/TPool behind a vtable. ConfigStoreFacadeT<TStore>
// is the templated implementation; each store gets one wrapper instance.

struct IConfigStoreFacade {
    virtual ~IConfigStoreFacade() = default;

    virtual const char* defaultPath() const = 0;

    virtual ConfigResult loadFromFile(const char* path) = 0;
    virtual ConfigResult saveToFile(const char* path)  = 0;

    virtual bool        isLoaded()    const = 0;
    virtual uint16_t    fileSize()    const = 0;
    virtual const char* lastError()   const = 0;
    virtual bool        hasRawYaml()  const = 0;
    virtual bool        validate(char* errBuf, size_t errBufSize) const = 0;
};

template<typename TStore>
class ConfigStoreFacadeT final : public IConfigStoreFacade {
public:
    explicit ConfigStoreFacadeT(TStore& store) : _store(&store) {}

    const char* defaultPath() const override { return TStore::defaultPath(); }

    ConfigResult loadFromFile(const char* path) override { return _store->loadFromFile(path); }
    ConfigResult saveToFile (const char* path) override { return _store->saveToFile(path); }

    bool        isLoaded()    const override { return _store->isLoaded(); }
    uint16_t    fileSize()    const override { return _store->fileSize(); }
    const char* lastError()   const override { return _store->lastError(); }
    bool        hasRawYaml()  const override { return _store->hasRawYaml(); }
    bool        validate(char* err, size_t len) const override { return _store->validate(err, len); }

private:
    TStore* _store;
};

// ============================================================================
// MultiConfigServer
// ============================================================================

class MultiConfigServer : public BusServer {
public:
    static constexpr size_t MAX_STORES = 8;

    MultiConfigServer() = default;

    const char* handlerName() const override { return "MultiConfigServer"; }

    /**
     * @brief Register a store facade. The facade must outlive the server.
     * @return true if added; false if MAX_STORES reached or path collides.
     */
    bool addStore(IConfigStoreFacade& facade) {
        if (_count >= MAX_STORES) return false;
        for (size_t i = 0; i < _count; ++i) {
            if (strcmp(_stores[i]->defaultPath(), facade.defaultPath()) == 0) {
                return false;  // duplicate path
            }
        }
        _stores[_count++] = &facade;
        return true;
    }

    /**
     * @brief Initial-load convenience: call loadFromFile() on every store.
     *
     * Used from setup(); per-store onLoaded callbacks fire as each one
     * succeeds. A failure on any one store is logged but does not abort
     * the rest — fresh boards may legitimately have only some YAMLs yet.
     */
    void loadAll() {
        for (size_t i = 0; i < _count; ++i) {
            const char* path = _stores[i]->defaultPath();
            auto r = _stores[i]->loadFromFile(nullptr);
            if (r.ok) {
                SFX_LOG_INFO("[Config] Loaded: %s (%u bytes)", path, _stores[i]->fileSize());
            } else {
                SFX_LOG_WARN("[Config] Load failed: %s (%s)", path, _stores[i]->lastError());
            }
        }
    }

protected:
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override {
        if (!this->isInitialized() || !this->serial()) {
            return CommandHandleResult::NotMyCommand;
        }
        if ((type >= HubFxPacket::CONFIG_RELOAD && type <= HubFxPacket::CONFIG_STATUS_RESP) ||
            type == HubFxPacket::CONFIG_SAVE) {
            return handleModulePacket(type, payload, len);
        }
        return CommandHandleResult::NotMyCommand;
    }

    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override {
        switch (type) {
            case HubFxPacket::CONFIG_RELOAD: handleReload(payload, len); return CommandHandleResult::Handled;
            case HubFxPacket::CONFIG_STATUS: handleStatus();             return CommandHandleResult::Handled;
            case HubFxPacket::CONFIG_SAVE:   handleSave(payload, len);   return CommandHandleResult::Handled;
            default: return CommandHandleResult::NotMyCommand;
        }
    }

    uint8_t moduleRangeLow()  const override { return HubFxPacket::CONFIG_RELOAD; }
    uint8_t moduleRangeHigh() const override { return HubFxPacket::CONFIG_SAVE; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

private:
    IConfigStoreFacade* _stores[MAX_STORES] = {};
    size_t              _count = 0;

    /// Look up a store by exact defaultPath() match. Returns nullptr if unknown.
    IConfigStoreFacade* find(const char* path) const {
        if (!path) return nullptr;
        for (size_t i = 0; i < _count; ++i) {
            if (strcmp(_stores[i]->defaultPath(), path) == 0) return _stores[i];
        }
        return nullptr;
    }

    /// Decode the optional [pathLen:u8][path:str] tail. Returns true if a
    /// path was present and copied into pathBuf; false means "no path".
    static bool extractPath(const uint8_t* payload, size_t len,
                            char* pathBuf, size_t pathBufSize) {
        if (len < 2) return false;
        uint8_t pathLen = payload[0];
        if (pathLen == 0 || (size_t)(1 + pathLen) > len || pathLen >= pathBufSize) {
            return false;
        }
        memcpy(pathBuf, &payload[1], pathLen);
        pathBuf[pathLen] = '\0';
        return true;
    }

    void handleReload(const uint8_t* payload, size_t len) {
        char pathBuf[128] = {};
        const bool havePath = extractPath(payload, len, pathBuf, sizeof(pathBuf));

        if (havePath) {
            IConfigStoreFacade* s = find(pathBuf);
            if (!s) {
                SFX_LOG_WARN("[Config] Reload: unknown path '%s'", pathBuf);
                this->sendNack(HubFxError::CONFIG_ERROR, "unknown config path");
                return;
            }
            auto r = s->loadFromFile(nullptr);  // facade always uses its own defaultPath
            if (r.ok || (r.populated && !r.validated)) {
                SFX_LOG_INFO("[Config] Reloaded: %s (%u bytes)", pathBuf, s->fileSize());
                this->sendAck();
            } else {
                SFX_LOG_WARN("[Config] Reload failed (%s): %s", pathBuf, s->lastError());
                this->sendNack(HubFxError::CONFIG_ERROR, s->lastError());
            }
            return;
        }

        // No path → reload every registered store. First hard failure NACKs.
        for (size_t i = 0; i < _count; ++i) {
            const char* p = _stores[i]->defaultPath();
            auto r = _stores[i]->loadFromFile(nullptr);
            if (!r.ok && !(r.populated && !r.validated)) {
                SFX_LOG_WARN("[Config] Reload-all failed at %s: %s", p, _stores[i]->lastError());
                this->sendNack(HubFxError::CONFIG_ERROR, _stores[i]->lastError());
                return;
            }
            SFX_LOG_INFO("[Config] Reloaded: %s (%u bytes)", p, _stores[i]->fileSize());
        }
        this->sendAck();
    }

    void handleSave(const uint8_t* payload, size_t len) {
        char pathBuf[128] = {};
        if (!extractPath(payload, len, pathBuf, sizeof(pathBuf))) {
            this->sendNack(HubFxError::CONFIG_ERROR, "save requires explicit path");
            return;
        }
        IConfigStoreFacade* s = find(pathBuf);
        if (!s) {
            SFX_LOG_WARN("[Config] Save: unknown path '%s'", pathBuf);
            this->sendNack(HubFxError::CONFIG_ERROR, "unknown config path");
            return;
        }
        if (!s->hasRawYaml()) {
            this->sendNack(HubFxError::CONFIG_ERROR, "no config loaded to save");
            return;
        }
        auto r = s->saveToFile(nullptr);  // facade always uses its own defaultPath
        if (r.ok) {
            SFX_LOG_INFO("[Config] Saved: %s (%u bytes)", pathBuf, s->fileSize());
            this->sendAck();
        } else {
            SFX_LOG_WARN("[Config] Save failed (%s): %s", pathBuf, s->lastError());
            this->sendNack(HubFxError::CONFIG_ERROR, s->lastError());
        }
    }

    /// Aggregate status across all stores.
    /// Wire shape (unchanged from ConfigServerT): [loaded:u8][totalSize:u16LE][validOk:u8]
    ///   loaded  = 1 iff every registered store is loaded
    ///   total   = sum of fileSize() across all stores (saturates at u16 max)
    ///   validOk = 1 iff every loaded store currently validates clean
    void handleStatus() {
        bool     allLoaded = (_count > 0);
        bool     allValid  = true;
        uint32_t total     = 0;
        char     scratch[64];

        for (size_t i = 0; i < _count; ++i) {
            const bool loaded = _stores[i]->isLoaded();
            allLoaded &= loaded;
            total     += _stores[i]->fileSize();
            if (loaded && !_stores[i]->validate(scratch, sizeof(scratch))) {
                allValid = false;
            }
        }
        if (total > 0xFFFFu) total = 0xFFFFu;

        uint8_t buf[4];
        buf[0] = allLoaded ? 1 : 0;
        CoreProtocol::putU16LE(&buf[1], (uint16_t)total);
        buf[3] = allValid ? 1 : 0;
        this->sendRawPacket(HubFxPacket::CONFIG_STATUS_RESP, this->currentTag(), buf, sizeof(buf));
    }
};

#endif // MULTI_CONFIG_SERVER_H
