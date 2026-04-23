/*
 * Slave Registry — HubFX-specific wrapper around shared UsbRegistry
 *
 * Provides SlaveRegistry as an alias for UsbRegistry and adds HubFX-specific
 * routing helpers (slaveTypeForPacketType) that depend on the per-board
 * packet ranges defined in serial/<board>/<board>.h.
 */

#ifndef SLAVE_REGISTRY_H
#define SLAVE_REGISTRY_H

#include <usb/usb_registry.h>
#include <serial/hubfx/hubfx.h>

// SlaveRegistry is an alias for the shared UsbRegistry singleton.
// All types (SlaveType, SlaveEntry, slaveTypeName) come from usb/usb_registry.h.
using SlaveRegistry = UsbRegistry;

// ============================================================================
// HubFX-specific: Map a slave-range packet type to its target SlaveType
// ============================================================================

/**
 * @brief Resolve a packet type byte to the SlaveType that owns that range.
 *
 * Type ranges (from .github/copilot-instructions.md):
 *   GunFX        0x01-0x2F
 *   LightFX      0x40-0x5F
 *   GearControl  0x60-0x7F
 *
 * Returns SlaveType::Unknown for any packet outside these ranges (HubFX,
 * Stream, Core, or unallocated). The hub uses this to auto-route inbound
 * slave-range packets without any envelope wrapping.
 */
inline SlaveType slaveTypeForPacketType(uint8_t type) {
    if (type >= 0x01 && type <= 0x2F) return SlaveType::GunFX;
    if (type >= 0x40 && type <= 0x5F) return SlaveType::LightFX;
    if (type >= 0x60 && type <= 0x7F) return SlaveType::GearControl;
    return SlaveType::Unknown;
}

#endif // SLAVE_REGISTRY_H
