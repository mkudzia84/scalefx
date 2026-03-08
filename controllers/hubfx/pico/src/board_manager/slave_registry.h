/*
 * Slave Registry — Manages connected slave controllers for HubFX
 *
 * Tracks which slave controllers (GunFX, LightFX, GearControl) are connected
 * via USB Host CDC and provides access to their BusClient instances.
 *
 * Slave routing uses the subcmd pattern (SLAVE_ROUTE_* packet types) so
 * the registry no longer maps packet type ranges to slaves.
 */

#ifndef SLAVE_REGISTRY_H
#define SLAVE_REGISTRY_H

#include <Arduino.h>
#include <serial_bus_client.h>

#include "hubfx_protocol.h"

// ============================================================================
// Slave Type Enumeration
// ============================================================================

enum class SlaveType : uint8_t {
    Unknown      = 0,
    GunFX        = 1,
    LightFX      = 2,
    GearControl  = 3,
    COUNT        = 4
};

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

struct SlaveEntry {
    SlaveType type       = SlaveType::Unknown;
    int       usbIndex   = -1;       // USB CDC device index in UsbHost
    bool      connected  = false;    // USB physically connected
    bool      ready      = false;    // INIT handshake completed
    BusClient* client    = nullptr;  // Protocol client (GunFxClient, etc.)
};

// ============================================================================
// Slave Registry — Manages connected slave controllers
// ============================================================================

class SlaveRegistry {
public:
    static constexpr uint8_t MAX_SLAVES = 4;

    static SlaveRegistry& instance() {
        static SlaveRegistry inst;
        return inst;
    }

    // Delete copy/move
    SlaveRegistry(const SlaveRegistry&) = delete;
    SlaveRegistry& operator=(const SlaveRegistry&) = delete;
    SlaveRegistry(SlaveRegistry&&) = delete;
    SlaveRegistry& operator=(SlaveRegistry&&) = delete;

    /**
     * @brief Register a slave type with its client instance and USB index
     * @param type   Slave controller type
     * @param client BusClient subclass for this slave
     * @param usbIndex USB CDC device index in UsbHost
     * @return true if registered (or updated existing entry)
     */
    bool registerSlave(SlaveType type, BusClient* client, int usbIndex) {
        // Update existing entry if same type
        for (uint8_t i = 0; i < _count; i++) {
            if (_slaves[i].type == type) {
                _slaves[i].client = client;
                _slaves[i].usbIndex = usbIndex;
                return true;
            }
        }
        if (_count >= MAX_SLAVES) return false;
        _slaves[_count].type = type;
        _slaves[_count].client = client;
        _slaves[_count].usbIndex = usbIndex;
        _count++;
        return true;
    }

    /**
     * @brief Mark a slave as connected/disconnected (USB CDC state)
     */
    void setConnected(SlaveType type, bool connected) {
        SlaveEntry* e = find(type);
        if (e) {
            e->connected = connected;
            if (!connected) e->ready = false;
        }
    }

    /**
     * @brief Mark a slave as ready (INIT handshake complete)
     */
    void setReady(SlaveType type, bool ready) {
        SlaveEntry* e = find(type);
        if (e) e->ready = ready;
    }

    /**
     * @brief Find the BusClient for a given slave type
     * @return nullptr if not found or not ready
     */
    BusClient* getClient(SlaveType type) const {
        const SlaveEntry* e = findConst(type);
        return (e && e->ready) ? e->client : nullptr;
    }

    /**
     * @brief Map a SLAVE_ROUTE_* packet type to a SlaveType
     * @return SlaveType::Unknown if not a routing packet
     */
    static SlaveType slaveTypeForRoutePacket(uint8_t hubPacketType) {
        switch (hubPacketType) {
            case HubFxPacket::SLAVE_ROUTE_GUNFX:       return SlaveType::GunFX;
            case HubFxPacket::SLAVE_ROUTE_LIGHTFX:     return SlaveType::LightFX;
            case HubFxPacket::SLAVE_ROUTE_GEARCONTROL: return SlaveType::GearControl;
            default:                                    return SlaveType::Unknown;
        }
    }

    // Iteration
    uint8_t count() const { return _count; }
    const SlaveEntry& operator[](uint8_t index) const { return _slaves[index]; }
    SlaveEntry& operator[](uint8_t index) { return _slaves[index]; }

    SlaveEntry* find(SlaveType type) {
        for (uint8_t i = 0; i < _count; i++) {
            if (_slaves[i].type == type) return &_slaves[i];
        }
        return nullptr;
    }

private:
    SlaveRegistry() = default;

    const SlaveEntry* findConst(SlaveType type) const {
        for (uint8_t i = 0; i < _count; i++) {
            if (_slaves[i].type == type) return &_slaves[i];
        }
        return nullptr;
    }

    SlaveEntry _slaves[MAX_SLAVES];
    uint8_t _count = 0;
};

#endif // SLAVE_REGISTRY_H
