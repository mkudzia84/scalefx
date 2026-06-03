/*
 * lightfx_program_selector.h — range-driven RC program selector.
 *
 *   When `/lightfx.yaml`'s `program_selector:` block is populated, the
 *   sketch wires an instance of this driver during boot.  It owns one
 *   `TriggerInput` (Raw type — passes µs straight through), subscribes
 *   it to the named RC channel, and on every change finds the matching
 *   `(from_us, to_us)` range from the YAML and calls
 *   `controller.selectProgram(idx)` for the program whose name matches
 *   the range entry.
 *
 *   Hysteresis: `hysteresis_us` keeps the selector pinned to the
 *   current range until the input moves at least that far past the
 *   nearest boundary — prevents stick-jitter flipping the active
 *   program at the edge of a range.
 *
 *   Decoupled from `LightFxEffectServicePolicyT` so the service stays
 *   free of an InputDispatcher template parameter; the sketch wires
 *   the dispatcher + the lightfx controller into this driver at apply
 *   time.
 */

#ifndef HUBFX_LIGHTFX_PROGRAM_SELECTOR_H
#define HUBFX_LIGHTFX_PROGRAM_SELECTOR_H

#include <cstdint>
#include <cstring>

#include <serial/diag_log.h>

#include "../effects/effect_id.h"
#include "../effects/input/trigger_input.h"
#include "../effects/lightfx/led_program.h"
#include "lightfx_config.h"

namespace hubfx::config {

// A selector range's program name (kSelectorProgramMax) must match a
// loaded program's name (kProgramNameMax) in FULL — keep the two buffers
// the same width so neither side silently truncates.
static_assert(kSelectorProgramMax == hubfx::effects::lightfx::kProgramNameMax,
              "kSelectorProgramMax must equal kProgramNameMax");

class LightFxProgramSelector {
public:
    LightFxProgramSelector() = default;

    /// Wire the selector to a freshly-loaded config + lightfx
    /// controller.  Called by the sketch right after applyLightFxConfig.
    /// Idempotent: a second call re-installs ranges + re-subscribes.
    /// Pass `dispatcher == nullptr` or `cfg.programSelector.enabled == false`
    /// to leave the selector dormant.
    template <typename TInputDispatcher, typename TController>
    void install(TInputDispatcher* dispatcher,
                 TController* controller,
                 const LightFxYamlConfig& cfg,
                 const hubfx::effects::PortRef& source,
                 uint8_t channel) {
        // Drop any prior subscription UP-FRONT so a re-install (CONFIG_RELOAD
        // from Studio) or a disable actually detaches the old trigger — the
        // typed `dispatcher` is available here, so we can unsubscribe cleanly.
        if (dispatcher) dispatcher->unsubscribe(&_trigger);
        _subscribed  = false;
        _dispatcher  = nullptr;
        _controllerPtr = nullptr;
        _selectFn    = nullptr;
        _numRanges   = 0;
        _lastRangeIx = -1;

        if (!dispatcher || !controller || !cfg.programSelector.enabled) {
            return; // dormant
        }

        // Index each range's program against the loaded catalog so the
        // tick callback can selectProgram() by INDEX (cheap) rather
        // than rerunning a name lookup every event.
        const uint8_t numPrograms = controller->numPrograms();
        for (uint8_t i = 0; i < cfg.programSelector.numRanges
                            && _numRanges < kMaxSelectorRanges; ++i) {
            const auto& r = cfg.programSelector.ranges[i];
            int8_t idx = -1;
            for (uint8_t pi = 0; pi < numPrograms; ++pi) {
                const auto* p = controller->programAt(pi);
                if (p && std::strcmp(p->name, r.program) == 0) {
                    idx = (int8_t)pi;
                    break;
                }
            }
            if (idx < 0) {
                SFX_LOG_WARN("[lightfx-selector] range[%u]: program '%s' not loaded — skipping",
                             (unsigned)i, r.program);
                continue;
            }
            _ranges[_numRanges].fromUs  = r.fromUs;
            _ranges[_numRanges].toUs    = r.toUs;
            _ranges[_numRanges].progIdx = idx;
            _numRanges++;
        }
        if (_numRanges == 0) {
            SFX_LOG_WARN("[lightfx-selector] no usable ranges — selector dormant");
            return;
        }

        // Type-erase the controller pointer + selectProgram entry so
        // the template parameter doesn't leak into private members.
        _dispatcher    = dispatcher;
        _controllerPtr = controller;
        _selectFn      = [](void* p, uint8_t idx) {
            return static_cast<TController*>(p)->selectProgram(idx);
        };
        _hysteresisUs  = cfg.programSelector.hysteresisUs;

        using namespace hubfx::effects::input;
        TriggerMapping m;
        m.kind     = TriggerKind::Raw;
        m.failsafe = FailsafeBehaviour::Hold;
        _trigger.configure(m, &LightFxProgramSelector::onChange, this);

        // Subscribe (any prior binding was already dropped at the top).
        if (dispatcher->subscribe(&_trigger, source, channel)) {
            _subscribed = true;
            SFX_LOG_INFO("[lightfx-selector] bound to {%s, %u} ch=%u  %u range(s), hyst=%uus",
                         PortKind::getName(source.portKind),
                         (unsigned)source.portIdx,
                         (unsigned)channel,
                         (unsigned)_numRanges,
                         (unsigned)_hysteresisUs);
        } else {
            _subscribed = false;
            SFX_LOG_WARN("[lightfx-selector] dispatcher subscribe failed");
        }
    }

private:
    struct ResolvedRange {
        uint16_t fromUs  = 0;
        uint16_t toUs    = 0;
        int8_t   progIdx = -1;
    };

    using SelectFn = bool (*)(void* controllerPtr, uint8_t idx);

    static void onChange(void* ctx,
                         const hubfx::effects::input::TriggerValue& v) {
        if (!ctx || !v.valid) return;
        auto* self = static_cast<LightFxProgramSelector*>(ctx);
        self->process(v.us);
    }

    void process(uint16_t us) {
        // Find the range containing `us`, with hysteresis around the
        // currently-active range's boundaries.
        int8_t hit = -1;
        for (uint8_t i = 0; i < _numRanges; ++i) {
            const auto& r = _ranges[i];
            // Active range gets a hysteresis band — exit only after the
            // input moves `hysteresisUs` outside its boundaries.
            const uint16_t lo = (i == _lastRangeIx && r.fromUs > _hysteresisUs)
                                ? (uint16_t)(r.fromUs - _hysteresisUs) : r.fromUs;
            const uint16_t hi = (i == _lastRangeIx)
                                ? (uint16_t)(r.toUs + _hysteresisUs)   : r.toUs;
            if (us >= lo && us < hi) { hit = (int8_t)i; break; }
        }
        if (hit < 0 || hit == _lastRangeIx) return;
        _lastRangeIx = hit;
        if (_selectFn && _controllerPtr) {
            _selectFn(_controllerPtr, (uint8_t)_ranges[hit].progIdx);
            SFX_LOG_INFO("[lightfx-selector] us=%u → program idx %u",
                         (unsigned)us, (unsigned)_ranges[hit].progIdx);
        }
    }

    hubfx::effects::input::TriggerInput _trigger{};
    ResolvedRange _ranges[kMaxSelectorRanges]{};
    uint8_t       _numRanges    = 0;
    int8_t        _lastRangeIx  = -1;
    uint16_t      _hysteresisUs = 50;
    bool          _subscribed   = false;
    void*         _dispatcher   = nullptr;
    void*         _controllerPtr = nullptr;
    SelectFn      _selectFn     = nullptr;
};

}  // namespace hubfx::config

#endif  // HUBFX_LIGHTFX_PROGRAM_SELECTOR_H
