/*
 * telemetry_emitter.h — periodic aggregated live-state snapshots.
 *
 *   When `/hubfx.yaml`'s `telemetry.inputs:` / `telemetry.outputs:`
 *   flags are on, the sketch's loop tick calls `tickTelemetry()` at
 *   `interval_ms` cadence.  Each tick emits one or two one-line INFO
 *   log entries via the DiagLog wire stream:
 *
 *     [T-in]  throttle=75% engine_toggle=on gear_switch=pos1 lights=off ...
 *     [T-out] pwm=[100,0,100,100,100,100,0,0]% (max=4095)
 *
 *   The `[T-in]` / `[T-out]` prefix is a parser anchor — CLI subscribers
 *   render them inline; Studio matches on the prefix to update a live
 *   dashboard.  Wire format: regular `LOG_MESSAGE` packets (TAG_ASYNC),
 *   so no new wire surface is needed.
 *
 *   The input snapshot iterates `cfg.inputs[]` (the named-channel
 *   declarations) and looks each up in the InputDispatcher's binding
 *   table.  Inputs declared in YAML but not subscribed by any effect
 *   show as `(unbound)`.  Inputs subscribed but with stale data show
 *   `(no data)`.
 *
 *   The output snapshot walks the board's hub-local PWM ports directly
 *   — the helper is templated on the board so it can reach
 *   `board.pwm[i].duty()`.  Servo / hbridge are out of scope for v1.
 */

#ifndef HUBFX_TELEMETRY_EMITTER_H
#define HUBFX_TELEMETRY_EMITTER_H

#include <cstdint>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <cstdio>
#include <cstring>

#include <serial/diag_log.h>

#include "hubfx_config.h"
#include "../effects/input/trigger_input.h"

namespace hubfx::config {

namespace detail {

/// Format one TriggerValue into `buf` (truncated to `cap-1`).
inline int formatTriggerValue(char* buf, size_t cap,
                              const hubfx::effects::input::TriggerValue& v) {
    using namespace hubfx::effects::input;
    if (!v.valid) {
        return std::snprintf(buf, cap, "(fs)");   // failsafe / no data yet
    }
    switch (v.kind) {
        case TriggerKind::Boolean:        return std::snprintf(buf, cap, "%s", v.b ? "on" : "off");
        case TriggerKind::EnumN:          return std::snprintf(buf, cap, "pos%u",   (unsigned)v.e);
        case TriggerKind::ProportionalU8: return std::snprintf(buf, cap, "%u%%",    (unsigned)v.u);
        case TriggerKind::ProportionalS8: return std::snprintf(buf, cap, "%+d%%",   (int)v.s);
        case TriggerKind::Raw:            return std::snprintf(buf, cap, "%uus",    (unsigned)v.us);
    }
    return std::snprintf(buf, cap, "?");
}

}  // namespace detail

/// Format + emit one input snapshot line.  Walks the named-input
/// declarations in `cfg.inputs[]`, looks each up in `dispatcher`, and
/// renders the current TriggerValue using the value's TriggerKind.
template <typename TInputDispatcher>
inline void emitInputSnapshot(const TInputDispatcher& dispatcher,
                              const HubFxConfig& cfg) {
    using hubfx::effects::PortRef;
    using hubfx::effects::input::TriggerInput;
    using hubfx::effects::input::TriggerValue;

    if (cfg.numInputs == 0) return;

    char line[256];
    int  off = std::snprintf(line, sizeof(line), "[T-in]");

    for (uint8_t i = 0; i < cfg.numInputs && off < (int)sizeof(line) - 16; ++i) {
        const InputBinding& def = cfg.inputs[i];
        const uint8_t wantCh = (def.channelId > 0) ? (uint8_t)(def.channelId - 1) : 0;

        // Find the matching binding in the dispatcher.
        const TriggerInput* found = nullptr;
        dispatcher.forEachBinding(
            [&](const TriggerInput* ti, const PortRef& src, uint8_t ch) {
                if (found || !ti) return;
                if (ch != wantCh) return;
                if (!src.equals(def.port)) return;
                found = ti;
            });

        off += std::snprintf(line + off, sizeof(line) - off, " %s=", def.name);
        if (!found) {
            off += std::snprintf(line + off, sizeof(line) - off, "(unbound)");
        } else {
            const TriggerValue v = found->last();
            off += detail::formatTriggerValue(line + off, sizeof(line) - off, v);
        }
    }
    SFX_LOG_INFO("%s", line);
}

/// Format + emit one PWM-output snapshot line.  Templated on the board
/// type so it can reach the concrete `pwm[]` port array directly —
/// HubFX has 8 PCA9685 channels; future boards override the index
/// range via `kNumPwm` if declared.
template <typename TBoard>
inline void emitOutputSnapshot(TBoard& board) {
    char line[160];
    int  off = std::snprintf(line, sizeof(line), "[T-out] pwm=[");

    constexpr uint8_t kNum = sizeof(board.pwm) / sizeof(board.pwm[0]);
    for (uint8_t i = 0; i < kNum && off < (int)sizeof(line) - 8; ++i) {
        const uint16_t d   = board.pwm[i].duty();
        const uint16_t max = board.pwm[i].maxDuty();
        const unsigned pct = (max == 0) ? 0u : (unsigned)((uint32_t)d * 100u / max);
        off += std::snprintf(line + off, sizeof(line) - off, "%s%u",
                             i == 0 ? "" : ",", pct);
    }
    off += std::snprintf(line + off, sizeof(line) - off, "]%%");
    SFX_LOG_INFO("%s", line);
}

/// Tick — call from the sketch's `loop()`.  Emits the configured
/// snapshots when at least `interval_ms` have elapsed since the last
/// tick.  Stateless apart from the `lastEmitMs` caller stores; the
/// caller is responsible for owning that variable (one per board).
template <typename TBoard, typename TInputDispatcher>
inline void tickTelemetry(TBoard& board,
                          const TInputDispatcher& dispatcher,
                          const HubFxConfig& cfg,
                          uint32_t& lastEmitMs) {
    const auto& t = cfg.telemetry;
    if (!t.inputs && !t.outputs) return;
    const uint32_t now = SFX_MILLIS();
    if (now - lastEmitMs < t.intervalMs) return;
    lastEmitMs = now;
    if (t.inputs)  emitInputSnapshot(dispatcher, cfg);
    if (t.outputs) emitOutputSnapshot(board);
}

}  // namespace hubfx::config

#endif  // HUBFX_TELEMETRY_EMITTER_H
