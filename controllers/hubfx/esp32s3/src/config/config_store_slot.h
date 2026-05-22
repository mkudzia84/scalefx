/*
 * config_store_slot.h — typed helper that bundles a ConfigStore + its
 * facade + the boot-time wiring boilerplate (register with
 * ConfigServicePolicy, set onLoaded callback, load-from-file with
 * schema-defaults fallback).
 *
 * Without this helper, each `/foo.yaml` store needs ~6 lines of static
 * declaration + ~3 lines of wiring + ~4 lines of boot-chain fallback.
 * With it, each store is one declaration + one `wire()` + one
 * `loadOrFallback()` call.
 *
 * Usage:
 *
 *     // namespace-scope statics — one per config file.
 *     static ConfigStoreSlot<HubFxConfigSchema, HubFxYamlPool>     kHubFx;
 *     static ConfigStoreSlot<AlertsConfigSchema, AlertsYamlPool>   kAlerts;
 *
 *     // apply callbacks defined separately (they may reference other slots).
 *     static void applyHubFxConfigCallback(const HubFxConfig& cfg) { … }
 *
 *     // in setup():
 *     auto& cfgPolicy = board.policy<ConfigServicePolicy>();
 *     kHubFx.wire (cfgPolicy, applyHubFxConfigCallback);
 *     kAlerts.wire(cfgPolicy, applyAlertsConfigCallback);
 *
 *     // boot chain:
 *     kHubFx.loadOrFallback();
 *     kAlerts.loadOrFallback();
 *
 * Templates: `TSchema` provides DataType + populate() + validate() +
 * defaultPath() (the ConfigSchema concept).  `TPool` is the YamlParser
 * pool size class.  The slot is move-suppressed (holds the facade with
 * a back-reference to the store) so it must live at namespace / static
 * scope; do not stuff it in a vector.
 */

#ifndef HUBFX_CONFIG_STORE_SLOT_H
#define HUBFX_CONFIG_STORE_SLOT_H

#include <config/config_store.h>
#include <serial/diag_log.h>
#include <server/multi_config_server.h>     // ConfigServicePolicy, ConfigStoreFacadeT
#include <storage/storage_config_bridge.h>  // wireConfigStore<FlashModule>
#include <storage/flash.h>

namespace hubfx::config {

template <typename TSchema, typename TPool>
class ConfigStoreSlot {
public:
    using Store    = ConfigStore<TSchema, TPool>;
    using DataType = typename TSchema::DataType;
    using ApplyFn  = void (*)(const DataType&);

    ConfigStoreSlot() : _facade(_store) {}

    // Non-copyable, non-movable: _facade holds a reference to _store.
    ConfigStoreSlot(const ConfigStoreSlot&)            = delete;
    ConfigStoreSlot& operator=(const ConfigStoreSlot&) = delete;

    Store&                    store()  { return _store; }
    ConfigStoreFacadeT<Store>& facade() { return _facade; }
    const DataType&           data() const { return _store.data(); }
    bool                      isLoaded() const { return _store.isLoaded(); }

    /// Register the store + facade against the ConfigServicePolicy and
    /// wire the storage bridge + onLoaded callback.  Call once at boot
    /// (before `loadOrFallback`).  `applyFn` may be nullptr if the
    /// caller drives apply manually.
    void wire(ConfigServicePolicy& policy, ApplyFn applyFn) {
        _applyFn = applyFn;
        if (applyFn) _store.onLoaded(applyFn);
        wireConfigStore<FlashModule>(_store);
        policy.addStore(_facade);
    }

    /// Load from `TSchema::defaultPath()`; on failure, invoke the
    /// apply callback with the struct-default data so a bare board
    /// still boots functional.
    void loadOrFallback() {
        if (!_store.loadFromFile(nullptr).ok) {
            SFX_LOG_WARN("[config] %s not loaded — applying schema defaults",
                         TSchema::defaultPath());
            if (_applyFn) _applyFn(_store.data());
        }
    }

private:
    Store                       _store;
    ConfigStoreFacadeT<Store>   _facade;
    ApplyFn                     _applyFn = nullptr;
};

}  // namespace hubfx::config

#endif  // HUBFX_CONFIG_STORE_SLOT_H
