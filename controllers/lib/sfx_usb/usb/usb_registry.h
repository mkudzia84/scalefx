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
 * Usage:
 *   #include <usb/usb_registry.h>
 *   UsbRegistry& reg = UsbRegistry::instance();
 *   reg.registerSlave(SlaveType::GunFX, &gunfxClient, 0);
 *   reg.setConnected(SlaveType::GunFX, true);
 *   reg.setReady(SlaveType::GunFX, true);
 *   BusClient* client = reg.getClient(SlaveType::GunFX);
 */

#ifndef SFX_USB_REGISTRY_H
#define SFX_USB_REGISTRY_H

#include <Arduino.h>
#include <stdint.h>

// Forward declaration — avoid pulling in full bus_client.h
class BusClient;

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
     * When disconnected, ready is also cleared (must re-handshake).
     */
    void setConnected(SlaveType type, bool connected) {
        SlaveEntry* e = find(type);
        if (e) {
            e->connected = connected;
            if (!connected) e->ready = false;
        }
    }

    /**
     * @brief Mark a slave as ready (INIT handshake completed)
     */
    void setReady(SlaveType type, bool ready) {
        SlaveEntry* e = find(type);
        if (e) e->ready = ready;
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

    SlaveEntry _slaves[MaxSlaves];
    uint8_t _count = 0;
};

/// Default registry with 4 slave slots (matches ScaleFX topology)
using UsbRegistry = UsbRegistryT<4>;

#endif // SFX_USB_REGISTRY_H
