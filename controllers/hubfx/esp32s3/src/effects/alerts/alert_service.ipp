/*
 * alert_service.ipp — AlertService template-method bodies.
 */

#ifndef HUBFX_ALERT_SERVICE_IPP
#define HUBFX_ALERT_SERVICE_IPP

#if defined(SFX_HAS_AUDIO)
#include <audio/audio_mixer.h>
#endif

namespace hubfx::effects::alerts {

// ─── Lifecycle ──────────────────────────────────────────────────────

template <MixerLike TMixer>
bool AlertServicePolicyT<TMixer>::begin(sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    SFX_LOG_INFO("[alert] %s on channel %u",
                 _cfg.enabled ? "ENABLED" : "disabled",
                 (unsigned)_cfg.channel);
    return _ctx != nullptr;
}

// ─── Wire dispatch ──────────────────────────────────────────────────

template <MixerLike TMixer>
CommandHandleResult AlertServicePolicyT<TMixer>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case AlertPacket::ALERT_BEEP:       handleBeep(payload, len);    return CommandHandleResult::Handled;
        case AlertPacket::ALERT_STOP:       handleStop();                return CommandHandleResult::Handled;
        case AlertPacket::ALERT_STATUS_REQ: handleStatusReq();           return CommandHandleResult::Handled;
        default:                            return CommandHandleResult::NotMyCommand;
    }
}

template <MixerLike TMixer>
void AlertServicePolicyT<TMixer>::handleBeep(const uint8_t* p, size_t len) {
    if (!_cfg.enabled) { _ctx->sendNack(AlertError::ALERT_DISABLED); return; }
    if (len < 1)       { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t severity = p[0];
    const uint8_t mask     = (len >= 2) ? p[1] : 0;
    if (!slotFor(severity)) {
        _ctx->sendNack(AlertError::UNKNOWN_SEVERITY);
        return;
    }
    if (!beep(severity, mask)) {
        _ctx->sendNack(AlertError::SOUND_NOT_CONFIGURED);
        return;
    }
    _ctx->sendAck();
}

template <MixerLike TMixer>
void AlertServicePolicyT<TMixer>::handleStop() {
    stop();
    _ctx->sendAck();
}

template <MixerLike TMixer>
void AlertServicePolicyT<TMixer>::handleStatusReq() {
    const uint8_t buf[2] = {
        static_cast<uint8_t>(isPlaying() ? 1 : 0),
        _lastSeverity,
    };
    _ctx->sendRawPacket(AlertPacket::ALERT_STATUS_RESP,
                        _ctx->currentTag(), buf, sizeof(buf));
}

// ─── C++ API ────────────────────────────────────────────────────────

template <MixerLike TMixer>
bool AlertServicePolicyT<TMixer>::beep(uint8_t severity, uint8_t outputMask) {
    if (!_cfg.enabled) return false;
    const AlertSeverityCfg* slot = slotFor(severity);
    if (!slot) {
        SFX_LOG_DEBUG("[alert] severity %s: no slot",
                      AlertSeverity::getName(severity));
        return false;
    }
    // Resolve the path: enum lookup first (preferred — set via YAML's
    // `sound: <name>` field), explicit override second.
    const char* path = (slot->sound != AlertSound::None)
                       ? alertSoundPath(slot->sound)
                       : slot->path;
    if (!path || !path[0]) {
        SFX_LOG_DEBUG("[alert] severity %s: no sound configured",
                      AlertSeverity::getName(severity));
        return false;
    }
    const uint8_t mask = outputMask ? outputMask : slot->outputMask;
    if (!playCustom(path, mask, slot->volume)) return false;
    _lastSeverity = severity;
    SFX_LOG_INFO("[alert] %s → %s", AlertSeverity::getName(severity), path);
    return true;
}

template <MixerLike TMixer>
bool AlertServicePolicyT<TMixer>::playCustom(const char* path,
                                              uint8_t outputMask,
                                              uint8_t volume) {
    if (!_cfg.enabled || !path || !path[0]) return false;
#if defined(SFX_HAS_AUDIO)
    AudioPlaybackOptions opts;
    const uint8_t mask = outputMask ? outputMask : 0x03;
    const uint8_t vol  = volume     ? volume     : 100;
    opts.volume         = static_cast<float>(vol) / 100.0f;
    opts.outputChannels = mask;
    opts.startOffsetMs  = 0;
    return TMixer::instance().playAsync(_cfg.channel, path, opts);
#else
    (void)path; (void)outputMask; (void)volume;
    return false;
#endif
}

template <MixerLike TMixer>
void AlertServicePolicyT<TMixer>::stop() {
    if (!_cfg.enabled) return;
#if defined(SFX_HAS_AUDIO)
    TMixer::instance().stopAsync(_cfg.channel, AudioStopMode::Immediate);
#endif
}

template <MixerLike TMixer>
bool AlertServicePolicyT<TMixer>::isPlaying() const {
    if (!_cfg.enabled) return false;
#if defined(SFX_HAS_AUDIO)
    return TMixer::instance().isPlaying(_cfg.channel);
#else
    return false;
#endif
}

// ─── Helpers ────────────────────────────────────────────────────────

template <MixerLike TMixer>
const AlertSeverityCfg* AlertServicePolicyT<TMixer>::slotFor(uint8_t severity) const {
    switch (severity) {
        case AlertSeverity::Info:     return &_cfg.info;
        case AlertSeverity::Warning:  return &_cfg.warning;
        case AlertSeverity::Error:    return &_cfg.error;
        case AlertSeverity::Critical: return &_cfg.critical;
        default:                      return nullptr;
    }
}

}  // namespace hubfx::effects::alerts

#endif  // HUBFX_ALERT_SERVICE_IPP
