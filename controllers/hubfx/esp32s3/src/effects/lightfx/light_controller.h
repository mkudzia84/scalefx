/*
 * light_controller.h — `LightControllerT<TTopology, TLandingService>`.
 *
 *   Master-side engine for the LightFX effect family.  Owns:
 *     - a small fixed list of `Program`s parsed from config (or
 *       hard-coded at compile time for now),
 *     - a master brightness scalar (0..100) that scales every claimed
 *       LightFx channel,
 *     - the mutual-exclusion / diff algorithm that drives
 *       LED_QUEUE_LOAD / LED_START / LED_STOP commands through the
 *       topology service when a new program is selected.
 *
 *   For each LED channel in the active program, the effective
 *   per-channel scale shipped via LED_SET_BRIGHTNESS is:
 *
 *       effective = (perChannelBrightnessPct * masterBrightnessPct) / 100
 *
 *   Re-broadcast on master-brightness change.
 *
 *   Landing-light state changes route through the supplied
 *   `LandingLightServicePolicy` reference — the LightController never
 *   touches landing-light ports directly.
 *
 *   Templated on the topology + landing-light service types to keep
 *   dispatch fully static, per Rule 33.
 */

#ifndef HUBFX_LIGHT_CONTROLLER_H
#define HUBFX_LIGHT_CONTROLLER_H

#include <cstdint>
#include <cstring>

#include <serial/roles.h>
#include <serial/diag_log.h>

#include "../effect_id.h"
#include "../landing_lights/landing_light_protocol.h"
#include "led_program.h"
#include "light_event.h"
#include "lightfx_protocol.h"

namespace hubfx::effects::lightfx {

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
class LightControllerT {
public:
    LightControllerT() = default;

    void bind(TTopology* topo, TLandingService* ll) {
        _topo = topo;
        _ll   = ll;
    }

    /// Install a static program list.  Copied — caller's array may go
    /// out of scope.  Programs are matched by `idx` (their position in
    /// the array) on `LIGHTFX_PROGRAM_SELECT`.
    void configure(const Program* programs, uint8_t count) {
        _numPrograms = 0;
        for (uint8_t i = 0; i < count && i < kMaxPrograms; ++i) {
            _programs[_numPrograms++] = programs[i];
        }
        // Claim every referenced port NOW that the program list is
        // populated.  `begin()` runs during board.begin() — BEFORE the
        // config chain loads programs — so claiming there would walk an
        // empty list and own nothing, and `selectProgram` would then skip
        // every channel as "not owned".  `claim()` is an ownership-
        // registry op independent of role attachment (applyPortRoles may
        // run later) and idempotent, so re-running it on every reload is
        // safe.
        claimPorts();
    }

    /// Walk every LED channel referenced in any program and claim each
    /// port for `EffectId::LightFx`.  Called from `configure()` once the
    /// program list is populated (NOT begin(), which runs before programs
    /// load).  Ports already held by another effect (e.g. LandingLight)
    /// log a warning and are skipped — programs referencing those ports
    /// will silently no-op.
    void claimPorts() {
        if (!_topo) return;
        for (uint8_t pi = 0; pi < _numPrograms; ++pi) {
            const Program& p = _programs[pi];
            for (uint8_t ci = 0; ci < p.numChannels; ++ci) {
                const PortRef& addr = p.channels[ci].addr;
                if (!_topo->claim(addr, EffectId::LightFx,
                                  RoleKind::LedAnimator)) {
                    // Claim is logged inside topology — nothing more to do.
                }
            }
        }
    }

    uint8_t numPrograms()   const { return _numPrograms; }
    int8_t  activeIndex()   const { return _activeIdx; }
    uint8_t masterBrightnessPct() const { return _masterBrightnessPct; }

    const Program* programAt(uint8_t idx) const {
        return (idx < _numPrograms) ? &_programs[idx] : nullptr;
    }

    /// Atomic program switch.  Walks the diff between the outgoing
    /// program and the requested one, stopping channels that drop out
    /// and (re)loading channels that come in.  Landing-light states
    /// are forwarded to the LandingLightService.  Returns false on
    /// invalid index.
    bool selectProgram(uint8_t idx);

    /// Drop the active program (everything off, landing-lights
    /// retracted).  Equivalent to selecting "no program".
    void reset();

    /// Live master brightness setter (0..100).  Re-broadcasts
    /// `LED_SET_BRIGHTNESS` for every channel in the active program
    /// so the change is visible immediately.
    void setMasterBrightness(uint8_t pct);

private:
    // Apply a single channel: send LED_QUEUE_LOAD + scaled
    // LED_SET_BRIGHTNESS + LED_START.
    void applyChannel(const LedChannelSpec& ch);

    // Stop a single channel: LED_STOP + LED_SET_BRIGHTNESS=0.
    void stopChannel(const PortRef& addr);

    // Apply / retract landing lights in the given binding list.
    void applyLandings(const LandingLightBinding* defs, uint8_t n);

    // Compute scaled brightness for a channel: (chan * master) / 100.
    uint8_t scaledBrightness(uint8_t channelPct) const;

    // True iff `addr` appears in `prog`'s channel list.
    static bool programHasChannel(const Program& prog, const PortRef& addr);

    // True iff `id` appears in `prog`'s landings list.
    static bool programHasLanding(const Program& prog, uint8_t id);

    TTopology*       _topo  = nullptr;
    TLandingService* _ll    = nullptr;

    Program _programs[kMaxPrograms] = {};
    uint8_t _numPrograms            = 0;

    int8_t  _activeIdx            = -1;
    uint8_t _masterBrightnessPct  = 100;
};

}  // namespace hubfx::effects::lightfx

#include "light_controller.ipp"

#endif  // HUBFX_LIGHT_CONTROLLER_H
