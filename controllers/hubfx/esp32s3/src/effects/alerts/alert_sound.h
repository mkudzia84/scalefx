/*
 * alert_sound.h — `AlertSound` enum + filesystem-name helpers.
 *
 * Single source of truth for the named system-alert sounds.  Each
 * enum value names a file under `/sounds/sys/<snake_name>.mp3` on SD
 * (post 2026-05-28 — the deployed sound library is MP3-encoded;
 * `Mp3PsramSource` loads it into PSRAM at first play and decodes via
 * libhelix).  Append-only (Rule 11) — never renumber.
 *
 * Master-side code references sounds by enum
 * (`alerts.play(AlertSound::Init)`), the YAML side references them by
 * snake-case name (`sound: init` in `/alerts.yaml`), and the alert
 * service resolves the path on the fly with `alertSoundPath()`.
 *
 * Missing files (enum value with no MP3 on SD) log a boot WARN; the
 * service's `play()` returns false rather than crashing.
 */

#ifndef HUBFX_ALERT_SOUND_H
#define HUBFX_ALERT_SOUND_H

#include <cstdint>
#include <cstring>

namespace hubfx::effects::alerts {

/// Catalog of well-known system-alert sounds.  File on SD is
/// `/sounds/sys/<snake_name>.mp3`.  Add new entries at the bottom —
/// **never renumber**.
enum class AlertSound : uint8_t {
    None              = 0,     ///< sentinel: "no sound for this severity"
    Init              = 1,     ///< /sounds/sys/init.mp3 — system started
    Warning           = 2,     ///< /sounds/sys/warning.mp3 — generic warning
    Error             = 3,     ///< /sounds/sys/error.mp3 — generic error
    Critical          = 4,     ///< /sounds/sys/critical.mp3 — fatal-ish
    LightFxDetected   = 5,     ///< /sounds/sys/lightfx_detected.mp3
    LightFxFwError    = 6,     ///< /sounds/sys/lightfx_fw_error.mp3
    GunFxFwError      = 7,     ///< /sounds/sys/gunfx_fw_error.mp3
    GearMoving        = 8,     ///< /sounds/sys/gear_moving.mp3
    BatteryLow        = 9,     ///< /sounds/sys/battery_low.mp3
    HubFxInitialized  = 10,    ///< /sounds/sys/hubfx_initialized.mp3 — spoken boot announcement
    // ─── Append below this line ─────────────────────────────────────
};

/// Snake-case name used in YAML + filenames.  Empty string for `None`.
inline const char* alertSoundName(AlertSound s) {
    switch (s) {
        case AlertSound::None:            return "";
        case AlertSound::Init:            return "init";
        case AlertSound::Warning:         return "warning";
        case AlertSound::Error:           return "error";
        case AlertSound::Critical:        return "critical";
        case AlertSound::LightFxDetected: return "lightfx_detected";
        case AlertSound::LightFxFwError:  return "lightfx_fw_error";
        case AlertSound::GunFxFwError:    return "gunfx_fw_error";
        case AlertSound::GearMoving:      return "gear_moving";
        case AlertSound::BatteryLow:      return "battery_low";
        case AlertSound::HubFxInitialized: return "hubfx_initialized";
    }
    return "";
}

/// Reverse lookup — used by the YAML schema's string→enum mapper.
/// Returns `AlertSound::None` on unknown names so a typo in YAML is a
/// no-op rather than a parse failure.
inline AlertSound alertSoundFromName(const char* name) {
    if (!name || !name[0]) return AlertSound::None;
    if (std::strcmp(name, "init")             == 0) return AlertSound::Init;
    if (std::strcmp(name, "warning")          == 0) return AlertSound::Warning;
    if (std::strcmp(name, "error")            == 0) return AlertSound::Error;
    if (std::strcmp(name, "critical")         == 0) return AlertSound::Critical;
    if (std::strcmp(name, "lightfx_detected") == 0) return AlertSound::LightFxDetected;
    if (std::strcmp(name, "lightfx_fw_error") == 0) return AlertSound::LightFxFwError;
    if (std::strcmp(name, "gunfx_fw_error")   == 0) return AlertSound::GunFxFwError;
    if (std::strcmp(name, "gear_moving")      == 0) return AlertSound::GearMoving;
    if (std::strcmp(name, "battery_low")      == 0) return AlertSound::BatteryLow;
    if (std::strcmp(name, "hubfx_initialized") == 0) return AlertSound::HubFxInitialized;
    return AlertSound::None;
}

/// Full SD path: `/sounds/sys/<name>.mp3`.  Filled into a per-enum
/// static buffer so the returned `const char*` is stable for the
/// caller's lifetime.  Empty path (`""`) for `AlertSound::None`.
inline const char* alertSoundPath(AlertSound s) {
    // Per-enum static buffer; assembled once on first access.  Storing
    // them as static locals keeps the per-board firmware image small:
    // only sounds the code actually references get string-pooled.
    switch (s) {
        case AlertSound::None: return "";
        default: break;
    }
    constexpr size_t kBufLen = 48;
    static char buf[16][kBufLen];   // up to 16 enum values
    const uint8_t  idx = static_cast<uint8_t>(s);
    if (idx == 0 || idx >= 16) return "";
    if (buf[idx][0] == '\0') {
        std::snprintf(buf[idx], kBufLen, "/sounds/sys/%s.mp3", alertSoundName(s));
    }
    return buf[idx];
}

}  // namespace hubfx::effects::alerts

#endif  // HUBFX_ALERT_SOUND_H
