/*
 * Slave Registry — HubFX-specific wrapper around shared UsbRegistry
 *
 * Provides SlaveRegistry as an alias for UsbRegistry and adds
 * HubFX-specific routing (slaveTypeForRoutePacket) that depends on
 * HubFxPacket constants.
 */

#ifndef SLAVE_REGISTRY_H
#define SLAVE_REGISTRY_H

#include <usb/usb_registry.h>
#include <serial/hubfx/hubfx.h>

// SlaveRegistry is an alias for the shared UsbRegistry singleton.
// All types (SlaveType, SlaveEntry, slaveTypeName) come from usb/usb_registry.h.
using SlaveRegistry = UsbRegistry;

// ============================================================================
// HubFX-specific: Map SLAVE_ROUTE_* packet types to SlaveType
// ============================================================================

/**
 * @brief Map a SLAVE_ROUTE_* packet type to a SlaveType
 * @return SlaveType::Unknown if not a routing packet
 *
 * This function depends on HubFxPacket constants and therefore cannot
 * live in the shared library.
 */
inline SlaveType slaveTypeForRoutePacket(uint8_t hubPacketType) {
    switch (hubPacketType) {
        case HubFxPacket::SLAVE_ROUTE_GUNFX:       return SlaveType::GunFX;
        case HubFxPacket::SLAVE_ROUTE_LIGHTFX:     return SlaveType::LightFX;
        case HubFxPacket::SLAVE_ROUTE_GEARCONTROL: return SlaveType::GearControl;
        default:                                    return SlaveType::Unknown;
    }
}

#endif // SLAVE_REGISTRY_H
