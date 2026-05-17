/*
 * expander_protocol.h — HubFX expander-management wire protocol + types.
 *
 * "Expander" = a downstream board attached to HubFX over USB CDC
 * (GunFX, LightFX, GearControl).  `ExpanderServicePolicyT<>` watches
 * the USB host stack, identifies devices by VID/PID, runs an `IDENTIFY`
 * handshake over CDC to harvest the full board spec (GUID + firmware
 * version + capabilities), and surfaces those transitions to:
 *
 *   - master-side consumers via `onConnect()` / `onIdentified()` /
 *     `onDisconnect()` lambdas;
 *   - the upstream host (CLI / Studio) via async wire packets
 *     `EXPANDER_CONNECTED` / `EXPANDER_IDENTIFIED` / `EXPANDER_DISCONNECTED`,
 *     plus the synchronous `EXPANDER_LIST_REQ` → `EXPANDER_LIST_RESP`
 *     enumeration.
 *
 * Wire-format Rule 11 applies — every payload appends optional fields
 * at the end so older masters still parse.
 *
 * Packet slice: 0x80..0x84 of the HubFX 0x80..0xAF range (was occupied
 * by the legacy SLAVE_LIST / SLAVE_INIT / SLAVE_STATUS packets that the
 * generic-expander refactor retired — see
 * `instructions/15-GENERIC-EXPANDER-REFACTOR.md`).
 *
 * GUID convention — see CLAUDE.md "Board GUID":  every board's
 * `deviceName` is `<Prefix>-<4-hex-chars>` where the suffix is the
 * last 4 chars of `sfxGetBoardId()`.  Master extracts the suffix from
 * the `IDENTIFY` response's deviceName and uses it as the persistence
 * key for that board's cached spec / config.
 */

#ifndef HUBFX_EXPANDER_PROTOCOL_H
#define HUBFX_EXPANDER_PROTOCOL_H

#include <Arduino.h>
#include <cstdint>

namespace hubfx::expanders {

// ============================================================================
// ExpanderKind — single-byte enum, append-only (Rule 11)
// ============================================================================

namespace ExpanderKind {
    constexpr uint8_t Unknown      = 0x00;  ///< Device enumerated but VID/PID not in the known table
    constexpr uint8_t GunFX        = 0x01;
    constexpr uint8_t LightFX      = 0x02;
    constexpr uint8_t GearControl  = 0x03;
    constexpr uint8_t PicoDefault  = 0x04;  ///< Arduino-Pico default PID 0x000A — can't disambiguate by PID alone

    inline const char* getName(uint8_t kind) {
        switch (kind) {
            case GunFX:        return "GunFX";
            case LightFX:      return "LightFX";
            case GearControl:  return "GearControl";
            case PicoDefault:  return "Pico(default-PID)";
            default:           return "Unknown";
        }
    }
}

// ============================================================================
// ExpanderSpec — board-reported identity + capability snapshot
// ============================================================================

/// Decoded `IDENTIFY` response.  This is the canonical record we
/// persist per-board; the master keys it by `guid` so a board that
/// reconnects on a different USB port still finds its prior spec.
///
/// Lifetime:
///   - On USB mount: spec is zero-initialised; `valid == false`.
///   - On IDENTIFY response: every field below is filled, `valid` set true.
///   - On USB unmount: the live slot's spec is snapshotted into the
///     GUID-keyed history (kept for the rest of the session) before
///     the slot is cleared.
struct ExpanderSpec {
    bool      valid                  = false;   ///< true once IDENTIFY decoded
    char      guid[5]                = {0};     ///< 4 hex chars + NUL (suffix of deviceName)
    char      deviceName[32]         = {0};     ///< full `<Prefix>-<guid>`
    char      firmwareVersion[16]    = {0};
    char      platform[24]           = {0};
    uint32_t  cpuFrequencyMHz        = 0;
    uint32_t  freeRamBytes           = 0;
    uint32_t  buildNumber            = 0;
    uint32_t  capabilities           = 0;       ///< CoreCapability::* bitmask
};

// ============================================================================
// ExpanderEntry — one tracked downstream board (live slot)
// ============================================================================

/// State for a single attached expander.  USB info + classification by
/// VID/PID is filled at USB mount; `spec` is populated when the
/// `IDENTIFY` response arrives over CDC.
struct ExpanderEntry {
    uint8_t   kind        = ExpanderKind::Unknown;  ///< from VID/PID
    bool      connected   = false;     ///< USB physically connected
    bool      identifying = false;     ///< IDENTIFY in flight (no response yet)
    uint8_t   usbAddr     = 0;         ///< USB device address (Host-assigned)
    uint8_t   usbIndex    = 0xFF;      ///< Index into UsbHost::getCdcDevice(i)
    uint16_t  vid         = 0;
    uint16_t  pid         = 0;
    ExpanderSpec spec;                 ///< filled once IDENTIFY response decoded
};

// ============================================================================
// Wire packet types
// ============================================================================

namespace ExpanderPacket {
    // Synchronous enumeration ----------------------------------------------
    constexpr uint8_t EXPANDER_LIST_REQ      = 0x80;
        ///< [] → EXPANDER_LIST_RESP
    constexpr uint8_t EXPANDER_LIST_RESP     = 0x81;
        ///< [count:u8] per-entry:
        ///<   [kind:u8][usbAddr:u8][vid:u16LE][pid:u16LE]
        ///<   [identified:u8]
        ///<   if identified == 1: [guidLen:u8][guid:str][nameLen:u8][name:str]
        ///<                       [verLen:u8][ver:str][capabilities:u32LE]
        ///<                       [buildNum:u32LE]

    // Async lifecycle (TAG_ASYNC) -----------------------------------------
    constexpr uint8_t EXPANDER_CONNECTED     = 0x82;
        ///< async: [kind:u8][usbAddr:u8][vid:u16LE][pid:u16LE]
        ///< Fires immediately on USB mount, BEFORE the IDENTIFY round-trip.
    constexpr uint8_t EXPANDER_DISCONNECTED  = 0x83;
        ///< async: [kind:u8][usbAddr:u8][guidLen:u8][guid:str]
        ///< (guid is empty when the device disconnected before IDENTIFY landed.)
    constexpr uint8_t EXPANDER_IDENTIFIED    = 0x84;
        ///< async: [kind:u8][usbAddr:u8][guidLen:u8][guid:str]
        ///<        [nameLen:u8][name:str][verLen:u8][ver:str]
        ///<        [capabilities:u32LE][buildNum:u32LE]
        ///< Fires after `IDENTIFY` response is decoded — carries the
        ///< full spec including the GUID used as persistence key.
}

// ============================================================================
// Expander-layer error codes (within the HubFX 0x80..0x8F slot)
// ============================================================================

namespace ExpanderError {
    constexpr uint8_t USB_HOST_DOWN         = 0x80;  ///< USB host stack not initialized
    constexpr uint8_t EXPANDER_TABLE_FULL   = 0x81;  ///< Too many attached devices for the registry
    constexpr uint8_t IDENTIFY_FAILED       = 0x82;  ///< IDENTIFY round-trip timed out / NACKed

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case USB_HOST_DOWN:        return "USB host not initialized";
            case EXPANDER_TABLE_FULL:  return "Expander tracking table full";
            case IDENTIFY_FAILED:      return "Expander IDENTIFY failed";
            default:                   return nullptr;
        }
    }
}

}  // namespace hubfx::expanders

#endif  // HUBFX_EXPANDER_PROTOCOL_H
