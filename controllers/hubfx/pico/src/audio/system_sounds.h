/**
 * System Sounds Configuration
 * 
 * Maps system sounds under /sounds/sys/ to appropriate components.
 * These are the actual sound files present on the SD card.
 * 
 * Naming convention: <component>_<event>.wav
 *   - hubfx_initialized    : Main hub initialized
 *   - gunfx_detected       : GunFX controller detected on bus
 *   - gunfx_initialized    : GunFX controller initialized
 *   - gunfx_fw_error       : GunFX firmware error
 *   - lightfx_detected     : LightFX controller detected
 *   - lightfx_initialized  : LightFX controller initialized
 *   - lightfx_fw_error     : LightFX firmware error
 *   - gearctrl_detected    : GearCtrl controller detected
 *   - gearcontrol_initialized : GearCtrl controller initialized
 *   - gearctrl_fw_error    : GearCtrl firmware error
 */

#ifndef SYSTEM_SOUNDS_H
#define SYSTEM_SOUNDS_H

#include <cstring>
#include "audio_channels.h"

// ============================================================================
//  SOUND PATHS - Actual files in /sounds/sys/
// ============================================================================

namespace SystemSounds {
    // Base path for system sounds
    constexpr const char* BASE_PATH = "/sounds/sys/";
    
    // ---- HubFX (Main Controller) ----
    constexpr const char* HUBFX_INITIALIZED     = "/sounds/sys/hubfx_initialized.wav";
    
    // ---- GunFX Controller ----
    constexpr const char* GUNFX_DETECTED        = "/sounds/sys/gunfx_detected.wav";
    constexpr const char* GUNFX_INITIALIZED     = "/sounds/sys/gunfx_initialized.wav";
    constexpr const char* GUNFX_FW_ERROR        = "/sounds/sys/gunfx_fw_error.wav";
    
    // ---- LightFX Controller ----
    constexpr const char* LIGHTFX_DETECTED      = "/sounds/sys/lightfx_detected.wav";
    constexpr const char* LIGHTFX_INITIALIZED   = "/sounds/sys/lightfx_initialized.wav";
    constexpr const char* LIGHTFX_FW_ERROR      = "/sounds/sys/lightfx_fw_error.wav";
    
    // ---- GearControl Controller ----
    constexpr const char* GEARCTRL_DETECTED     = "/sounds/sys/gearctrl_detected.wav";
    constexpr const char* GEARCTRL_INITIALIZED  = "/sounds/sys/gearcontrol_initialized.wav";
    constexpr const char* GEARCTRL_FW_ERROR     = "/sounds/sys/gearctrl_fw_error.wav";

    // ---- Channel for system sounds (from audio_channels.h) ----
    constexpr int CHANNEL = AudioChannels::SYSTEM;
}

// ============================================================================
//  SOUND TYPES BY COMPONENT
// ============================================================================

enum class SystemSoundComponent : uint8_t {
    HubFX = 0,
    GunFX,
    LightFX,
    GearCtrl,
    Unknown
};

enum class SystemSoundEvent : uint8_t {
    Detected = 0,
    Initialized,
    FirmwareError,
    Unknown
};

/**
 * Get the component from a filename
 */
inline SystemSoundComponent getComponentFromFilename(const char* filename) {
    if (!filename) return SystemSoundComponent::Unknown;
    
    // Skip path to get to filename
    const char* name = strrchr(filename, '/');
    name = name ? name + 1 : filename;
    
    if (strncmp(name, "hubfx_", 6) == 0)      return SystemSoundComponent::HubFX;
    if (strncmp(name, "gunfx_", 6) == 0)      return SystemSoundComponent::GunFX;
    if (strncmp(name, "lightfx_", 8) == 0)    return SystemSoundComponent::LightFX;
    if (strncmp(name, "gearctrl_", 9) == 0)   return SystemSoundComponent::GearCtrl;
    if (strncmp(name, "gearcontrol_", 12) == 0) return SystemSoundComponent::GearCtrl;
    
    return SystemSoundComponent::Unknown;
}

/**
 * Get the event type from a filename
 */
inline SystemSoundEvent getEventFromFilename(const char* filename) {
    if (!filename) return SystemSoundEvent::Unknown;
    
    if (strstr(filename, "_detected"))    return SystemSoundEvent::Detected;
    if (strstr(filename, "_initialized")) return SystemSoundEvent::Initialized;
    if (strstr(filename, "_fw_error"))    return SystemSoundEvent::FirmwareError;
    
    return SystemSoundEvent::Unknown;
}

/**
 * Get sound filename for a component and event
 * Returns nullptr if no sound is mapped
 */
inline const char* getSystemSound(SystemSoundComponent component, SystemSoundEvent event) {
    switch (component) {
        case SystemSoundComponent::HubFX:
            switch (event) {
                case SystemSoundEvent::Initialized: return SystemSounds::HUBFX_INITIALIZED;
                default: return nullptr;
            }
        case SystemSoundComponent::GunFX:
            switch (event) {
                case SystemSoundEvent::Detected:      return SystemSounds::GUNFX_DETECTED;
                case SystemSoundEvent::Initialized:   return SystemSounds::GUNFX_INITIALIZED;
                case SystemSoundEvent::FirmwareError: return SystemSounds::GUNFX_FW_ERROR;
                default: return nullptr;
            }
        case SystemSoundComponent::LightFX:
            switch (event) {
                case SystemSoundEvent::Detected:      return SystemSounds::LIGHTFX_DETECTED;
                case SystemSoundEvent::Initialized:   return SystemSounds::LIGHTFX_INITIALIZED;
                case SystemSoundEvent::FirmwareError: return SystemSounds::LIGHTFX_FW_ERROR;
                default: return nullptr;
            }
        case SystemSoundComponent::GearCtrl:
            switch (event) {
                case SystemSoundEvent::Detected:      return SystemSounds::GEARCTRL_DETECTED;
                case SystemSoundEvent::Initialized:   return SystemSounds::GEARCTRL_INITIALIZED;
                case SystemSoundEvent::FirmwareError: return SystemSounds::GEARCTRL_FW_ERROR;
                default: return nullptr;
            }
        default:
            return nullptr;
    }
}

#endif // SYSTEM_SOUNDS_H
