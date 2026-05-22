/*
 * alert_service.h — `AlertServicePolicyT<TMixer>`.
 *
 *   System-alert audio layer on top of the local mixer.  Owns
 *   channel `HubFxLayout::Alert` (== 0) and plays a short preset
 *   sound per severity level.
 *
 *   The service is the canonical place every other effect / service
 *   reaches for "make a sound at the user":
 *
 *     auto* alerts = ctx->findPolicy<AlertService>();
 *     if (alerts) alerts->beep(AlertSeverity::Warning);
 *
 *   Sound files live on the hub's storage (typically /alerts/*.wav)
 *   and are configured via `AlertServiceConfig::*Path` strings at
 *   `configure()` time.  Missing files are no-ops with a diag warn.
 *
 *   Future extensibility — the design accommodates:
 *     - voice messages: `playCustom("/voice/battery_low.wav", OUTPUT_ALL)`
 *     - mission audio: `playCustom("/missions/target_destroyed.wav", ...)`
 *     - dynamic message queues: layered on top of `playCustom`
 *
 *   Wire surface 0xD3..0xD6 — see `alert_protocol.h`.  Master-side
 *   C++ API is the primary user; the wire surface is for Studio /
 *   CLI to fire test beeps.
 */

#ifndef HUBFX_ALERT_SERVICE_H
#define HUBFX_ALERT_SERVICE_H

#include <cstdint>
#include <cstring>

#include <audio/audio_mixer.h>                 // AudioMixer concept
#include <serial/core/core.h>
#include <serial/diag_log.h>
#include <serial/wire.h>
#include <server/board_server.h>

#include "../audio_layout.h"
#include "alert_protocol.h"
#include "alert_sound.h"

namespace hubfx::effects::alerts {

/// Path-and-routing per severity.  Two ways to address a sound:
///   - `sound != AlertSound::None`  → resolved at play() time via
///     `alertSoundPath()` (preferred — `/alerts.yaml` says `sound: init`).
///   - `sound == AlertSound::None`  → fall back to the explicit `path`
///     string (only place ad-hoc paths still work — e.g. for `playCustom`).
struct AlertSeverityCfg {
    AlertSound sound      = AlertSound::None;
    char       path[64]   = {};
    uint8_t    outputMask = 0x03;    ///< AudioWire::OUTPUT_ALL
    uint8_t    volume     = 100;     ///< 0..100
};

struct AlertServiceConfig {
    bool             enabled    = false;
    uint8_t          channel    = audio::HubFxLayout::Alert;
    AlertSeverityCfg info;
    AlertSeverityCfg warning;
    AlertSeverityCfg error;
    AlertSeverityCfg critical;
};

template <MixerLike TMixer>
class AlertServicePolicyT {
public:
    /// Advertises `CoreCapability::ALERTS`.  Lets the host filter the
    /// `alert*` verbs in `help` when this policy isn't loaded — alerts
    /// also implicitly require `AUDIO`, but the dedicated bit keeps the
    /// host from showing the alert verbs on an audio-only firmware
    /// (e.g., a bare-mixer test build).
    static constexpr uint32_t kCapabilityBits = CoreCapability::ALERTS;

    AlertServicePolicyT() = default;

    void configure(const AlertServiceConfig& cfg) { _cfg = cfg; }

    /// Runtime kill-switch — `/hubfx.yaml`'s `alerts.enabled:` flips
    /// this independently of the full `configure()` so the master
    /// enable matrix in /hubfx.yaml has the final word over the local
    /// `enabled:` in /alerts.yaml.  Symmetric with the other services.
    void setEnabled(bool v) { _cfg.enabled = v; }

    /// Runtime-enable accessor — picked up by
    /// `BoardServer::computeEnabledCapabilities()` so the host can
    /// distinguish "alert service compiled in" from "alert service
    /// active right now".  Driven by `_cfg.enabled`.
    bool enabled() const { return _cfg.enabled; }

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == AlertPacket::ALERT_BEEP
            || type == AlertPacket::ALERT_STOP
            || type == AlertPacket::ALERT_STATUS_REQ;
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    void update() {}

    const char* getErrorMessage(uint8_t code) const {
        return AlertError::getMessage(code);
    }

    // ── Master-internal C++ API ──────────────────────────────────────

    /// Play the preset sound for `severity`.  Returns true if a
    /// playback was queued, false on disabled / unknown severity /
    /// missing sound path.  `outputMask == 0` means "use the severity
    /// preset's configured output mask".
    bool beep(uint8_t severity, uint8_t outputMask = 0);

    /// Generic play: any WAV path on the alert channel.  Use for
    /// voice messages / mission audio.  Same return semantics.
    bool playCustom(const char* path, uint8_t outputMask = 0,
                    uint8_t volume = 0);

    /// Direct enum-based playback — resolves `s` via
    /// `alertSoundPath()` and plays that WAV.  Used by the sketch /
    /// other services for explicit named cues (`alerts.playSound(
    /// AlertSound::Init)` at boot, etc.) that aren't tied to a
    /// configured severity slot.  Volume defaults to 100%.
    bool playSound(AlertSound s, uint8_t outputMask = 0, uint8_t volume = 0) {
        if (s == AlertSound::None) return false;
        const char* path = alertSoundPath(s);
        if (!path || !path[0]) return false;
        return playCustom(path, outputMask, volume);
    }

    /// Stop whatever's playing on the alert channel.
    void stop();

    /// True if the alert channel is currently active.
    bool isPlaying() const;

    uint8_t lastSeverity() const { return _lastSeverity; }

private:
    void handleBeep      (const uint8_t* p, size_t len);
    void handleStop      ();
    void handleStatusReq ();

    /// Look up the per-severity config slot.
    const AlertSeverityCfg* slotFor(uint8_t severity) const;

    sfx_core::BoardServerBase* _ctx          = nullptr;
    AlertServiceConfig         _cfg{};
    uint8_t                    _lastSeverity = 0xFF;
};

}  // namespace hubfx::effects::alerts

#include "alert_service.ipp"

#endif  // HUBFX_ALERT_SERVICE_H
