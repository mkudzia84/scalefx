#include "hub_lightfx_apply.h"

#include <platform/diag_log.h>
#include <serial/lightfx/lightfx.h>

namespace {

inline uint8_t code(const CommandResult& r) {
    return r.success ? LightFxError::OK : r.errorCode;
}

inline bool report(const char* op, uint8_t slot, uint8_t err) {
    if (err == LightFxError::OK) return true;
    if (slot == 0) SFX_LOG_WARN("[LightFx] push %s failed: 0x%02X",            op, err);
    else           SFX_LOG_WARN("[LightFx] push %s(slot=%u) failed: 0x%02X",   op, slot, err);
    return false;
}

} // namespace

bool pushLightFxConfigToSlave(const LightProgramConfig& cfg, LightFxClient& client) {

    if (!report("setMasterBrightness", 0,
                code(client.ledMasterBrightness(cfg.masterBrightness)))) return false;

    // LightFX (slave) only owns servos declared with servo_board=lightfx in
    // a landing group; hub-owned servos live on hub-side hardware. We still
    // push every entry from cfg.servos[] to the slave since the slave's
    // servo IDs (1-3) are board-local — the per-group binding below is what
    // tells the slave which servo to use.
    for (uint8_t i = 0; i < cfg.servoCount; ++i) {
        const auto& s = cfg.servos[i];
        if (s.id == 0) continue;
        if (!report("servoSettings", s.id,
                    code(client.servoSettings(s.id,
                                              s.min_us, s.max_us,
                                              s.speed,  s.accel, s.decel,
                                              s.reversed)))) return false;
        // Drive to YAML-declared center so a freshly-online slave matches
        // the standalone applyLightProgramConfigLocal() semantics.
        if (!report("servoSet", s.id,
                    code(client.servoSet(s.id, static_cast<int16_t>(s.default_us))))) return false;
    }

    // Reset every slot first so a YAML with fewer groups than the hardware
    // supports clears stale bindings from a previous config.
    if (!report("landingLightUnbind(all)", 0,
                code(client.landingLightUnbind(0)))) return false;

    uint8_t pushed = 0;
    for (uint8_t i = 0; i < cfg.landingGroupCount; ++i) {
        const auto& g = cfg.landingGroups[i];
        // Slave only sees the lightfx-side channels. A group that names
        // only hubfx channels (and a hubfx servo) is invisible to the slave.
        if (g.lightfxChannelMask == 0) continue;
        const uint8_t slot = static_cast<uint8_t>(i + 1);
        // Servo only travels with the bind if the slave actually owns it.
        const uint8_t servoId = LightProgramBoard::isLightFx(g.servoBoard) ? g.servoId : 0;
        // Wire arg order: (slot, servoId, channelMask, brightness).
        if (!report("landingLightBind", slot,
                    code(client.landingLightBind(slot, servoId, g.lightfxChannelMask, g.brightness)))) {
            return false;
        }
        ++pushed;
    }

    SFX_LOG_INFO("[LightFx] config pushed to slave (%u servo(s), %u/%u landing group(s) with lightfx channels)",
                 (unsigned)cfg.servoCount, (unsigned)pushed, (unsigned)cfg.landingGroupCount);
    return true;
}
