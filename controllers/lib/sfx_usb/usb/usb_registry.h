/*
 * USB Device Registry — Tracks connected USB slave controllers
 *
 * Platform-independent template that maintains a registry of connected
 * USB slave devices (identified by SlaveType). Each entry tracks:
 *   - USB CDC device index (into UsbHost)
 *   - Connection state (USB physically attached)
 *   - Ready state (INIT handshake completed)
 *   - BusClient pointer for protocol communication
 *
 * The registry does NOT own or allocate BusClient instances — those are
 * created and managed by the controller firmware. The registry simply
 * provides lookup and state management.
 *
 * Template parameter MaxSlaves controls the maximum number of tracked
 * slave devices. Default is 4 (matches current ScaleFX topology).
 *
 * Lifecycle callbacks (per slave type):
 *   - onReady(type, fn)      fires once per false→true transition of `ready`
 *   - onDisconnect(type, fn) fires once per true→false transition of `ready`
 *                            (covers explicit setReady(false), SHUTDOWN/REBOOT
 *                             via SlaveServer, and USB unmount via setConnected)
 *
 * The callbacks let consumers wire "load config + push it down on attach"
 * without polling — main loop no longer needs per-board reconcile() helpers.
 *
 * Usage:
 *   #include <usb/usb_registry.h>
 *   UsbRegistry& reg = UsbRegistry::instance();
 *   reg.registerSlave(SlaveType::GunFX, &gunfxClient, 0);
 *   reg.onReady(SlaveType::LightFX, [](SlaveType, BusClient* c) { ... });
 *   reg.setConnected(SlaveType::GunFX, true);
 *   reg.setReady(SlaveType::GunFX, true);   // fires onReady callback
 *   BusClient* client = reg.getClient(SlaveType::GunFX);
 */

#ifndef SFX_USB_REGISTRY_H
#define SFX_USB_REGISTRY_H

#include <Arduino.h>
#include <stdint.h>
#include <functional>

#include <platform/diag_log.h>
#include <serial/client/bus_client.h>  // BusClient::serverName() for auto-log

// ============================================================================
// Slave Type Enumeration
// ============================================================================

/**
 * @brief Type of slave controller connected via USB
 *
 * Each ScaleFX controller type has a unique identifier used by the
 * registry to track and differentiate connected devices.
 */
enum class SlaveType : uint8_t {
    Unknown      = 0,
    GunFX        = 1,
    LightFX      = 2,
    GearControl  = 3,
    COUNT        = 4
};

/**
 * @brief Get human-readable name for a SlaveType
 */
inline const char* slaveTypeName(SlaveType type) {
    switch (type) {
        case SlaveType::GunFX:       return "GunFX";
        case SlaveType::LightFX:     return "LightFX";
        case SlaveType::GearControl: return "GearControl";
        default:                     return "Unknown";
    }
}

// ============================================================================
// Slave Entry — Registry entry for a connected slave
// ============================================================================

/**
 * @brief State and metadata for a single connected slave controller
 */
struct SlaveEntry {
    SlaveType  type       = SlaveType::Unknown;
    int        usbIndex   = -1;       ///< USB CDC device index in UsbHost
    bool       connected  = false;    ///< USB physically connected
    bool       ready      = false;    ///< INIT handshake completed
    BusClient* client     = nullptr;  ///< Protocol client (GunFxClient, etc.)
};

// ============================================================================
// USB Device Registry — Manages connected slave controllers
// ============================================================================

/**
 * @brief Singleton registry tracking connected USB slave controllers
 *
 * Provides registration, lookup, and state management for slave devices.
 * Used by HubFX to track which controllers are online and route commands.
 *
 * @tparam MaxSlaves  Maximum number of slave devices to track (default: 4)
 *
 * Thread safety: Currently single-core access only. If cross-core access
 * is needed, protect with SfxMutex.
 */
template <size_t MaxSlaves = 4>
class UsbRegistryT {
public:
    static constexpr size_t MAX_SLAVES = MaxSlaves;

    static UsbRegistryT& instance() {
        static UsbRegistryT inst;
        return inst;
    }

    // Delete copy/move
    UsbRegistryT(const UsbRegistryT&) = delete;
    UsbRegistryT& operator=(const UsbRegistryT&) = delete;
    UsbRegistryT(UsbRegistryT&&) = delete;
    UsbRegistryT& operator=(UsbRegistryT&&) = delete;

    // ========================================================================
    // Registration
    // ========================================================================

    /**
     * @brief Register a slave type with its client instance and USB index
     * @param type      Slave controller type
     * @param client    BusClient subclass for this slave (not owned)
     * @param usbIndex  USB CDC device index in UsbHost
     * @return true if registered (or updated existing entry)
     */
    bool registerSlave(SlaveType type, BusClient* client, int usbIndex) {
        // Update existing entry if same type already registered
        for (uint8_t i = 0; i < _count; i++) {
            if (_slaves[i].type == type) {
                _slaves[i].client = client;
                _slaves[i].usbIndex = usbIndex;
                return true;
            }
        }
        if (_count >= MaxSlaves) return false;
        _slaves[_count].type = type;
        _slaves[_count].client = client;
        _slaves[_count].usbIndex = usbIndex;
        _count++;
        return true;
    }

    // ========================================================================
    // State Management
    // ========================================================================

    /**
     * @brief Mark a slave as connected or disconnected (USB CDC state)
     *
     * When disconnected, ready is also cleared (must re-handshake) and the
     * onDisconnect callback for this type fires if the slave was previously
     * ready.
     */
    void setConnected(SlaveType type, bool connected) {
        SlaveEntry* e = find(type);
        if (!e) return;
        e->connected = connected;
        if (!connected && e->ready) {
            e->ready = false;
            fireDisconnect(type);
        }
    }

    /**
     * @brief Mark a slave as ready (INIT handshake completed)
     *
     * Fires the per-type onReady callback on false→true transitions, and the
     * per-type onDisconnect callback on true→false transitions. No-op when
     * the value already matches — callbacks are edge-triggered, not level.
     */
    void setReady(SlaveType type, bool ready) {
        SlaveEntry* e = find(type);
        if (!e) return;
        if (e->ready == ready) return;
        e->ready = ready;
        if (ready) fireReady(type, e->client);
        else       fireDisconnect(type);
    }

    // ========================================================================
    // Lifecycle Callbacks (per slave type)
    // ========================================================================
    //
    // One callback slot per SlaveType. Re-registering overwrites the previous
    // callback for that type. Callbacks fire from setReady() / setConnected()
    // on the same core that mutates the registry — the consumer does not need
    // its own synchronization for the transition itself.

    using ReadyCallback      = std::function<void(SlaveType, BusClient*)>;
    using DisconnectCallback = std::function<void(SlaveType)>;

    /** @brief Register an on-ready callback for a specific slave type. */
    void onReady(SlaveType type, ReadyCallback cb) {
        uint8_t i = (uint8_t)type;
        if (i < (uint8_t)SlaveType::COUNT) _readyCb[i] = std::move(cb);
    }

    /** @brief Register an on-disconnect callback for a specific slave type. */
    void onDisconnect(SlaveType type, DisconnectCallback cb) {
        uint8_t i = (uint8_t)type;
        if (i < (uint8_t)SlaveType::COUNT) _disconnectCb[i] = std::move(cb);
    }

    // ========================================================================
    // Lookup
    // ========================================================================

    /**
     * @brief Get the BusClient for a given slave type
     * @return nullptr if not found or not ready
     */
    BusClient* getClient(SlaveType type) const {
        const SlaveEntry* e = find(type);
        return (e && e->ready) ? e->client : nullptr;
    }

    /**
     * @brief Find a slave entry by type (mutable)
     * @return nullptr if not found
     */
    SlaveEntry* find(SlaveType type) {
        for (uint8_t i = 0; i < _count; i++) {
            if (_slaves[i].type == type) return &_slaves[i];
        }
        return nullptr;
    }

    /**
     * @brief Find a slave entry by type (const)
     * @return nullptr if not found
     */
    const SlaveEntry* find(SlaveType type) const {
        for (uint8_t i = 0; i < _count; i++) {
            if (_slaves[i].type == type) return &_slaves[i];
        }
        return nullptr;
    }

    /**
     * @brief Find a slave entry by USB device index
     * @return nullptr if not found
     */
    SlaveEntry* findByUsbIndex(int usbIndex) {
        for (uint8_t i = 0; i < _count; i++) {
            if (_slaves[i].usbIndex == usbIndex) return &_slaves[i];
        }
        return nullptr;
    }

    // ========================================================================
    // Iteration
    // ========================================================================

    uint8_t count() const { return _count; }
    const SlaveEntry& operator[](uint8_t index) const { return _slaves[index]; }
    SlaveEntry& operator[](uint8_t index) { return _slaves[index]; }

    /**
     * @brief Reset all entries to disconnected/not ready state
     *
     * Does not remove registrations — just clears connection state.
     * Useful on USB host reset or shutdown.
     */
    void resetAll() {
        for (uint8_t i = 0; i < _count; i++) {
            _slaves[i].connected = false;
            _slaves[i].ready = false;
        }
    }

private:
    UsbRegistryT() = default;

    void fireReady(SlaveType type, BusClient* client) {
        // Auto-log every transition so per-board callbacks don't have to
        // duplicate the line for the simple "log + nothing else" case.
        SFX_LOG_INFO("[%s] slave ready: %s",
                     slaveTypeName(type),
                     (client && client->serverName()) ? client->serverName() : "?");
        uint8_t i = (uint8_t)type;
        if (i < (uint8_t)SlaveType::COUNT && _readyCb[i]) _readyCb[i](type, client);
    }

    void fireDisconnect(SlaveType type) {
        SFX_LOG_INFO("[%s] slave disconnected", slaveTypeName(type));
        uint8_t i = (uint8_t)type;
        if (i < (uint8_t)SlaveType::COUNT && _disconnectCb[i]) _disconnectCb[i](type);
    }

    SlaveEntry _slaves[MaxSlaves];
    uint8_t _count = 0;

    ReadyCallback      _readyCb     [(size_t)SlaveType::COUNT];
    DisconnectCallback _disconnectCb[(size_t)SlaveType::COUNT];
};

/// Default registry with 4 slave slots (matches ScaleFX topology)
using UsbRegistry = UsbRegistryT<4>;

#endif // SFX_USB_REGISTRY_H
