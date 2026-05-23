/*
 * element_scaling.h — voltage scaling for sub-rail elements on a PWM port.
 *
 * A board's PWM rail (`PwmBinding.voltageMv`, set in Phase 0) often
 * sits above the rated voltage of the element wired to it — a 5 V
 * smoke heater on the 8 V rail, a 3 V LED ring on the 5 V rail, a
 * 6 V brushed fan on the 12 V battery rail.  Driving such an element
 * at 100 % duty would burn it out.
 *
 * `scaleDutyPct(...)` converts a 0..100 % "intent" (what the effect
 * wants the element to do) into a port-native duty (0..maxDuty) that
 * delivers the element's RATED voltage on average.
 *
 * Where this lives in the stack:
 *
 *   - PORT (Phase 0)    — declares its rail voltage at board time.
 *                         `binding.voltageMv` is the rail.
 *   - ROLE (this file)  — knows the element's rated voltage + scaling
 *                         mode; converts intent → duty internally.
 *                         Generic, used by HeaterRole + DcMotorRole.
 *   - EFFECT (GunFx, etc.) — just commands the role at the intent
 *                         level (target temp, target speed, "puff
 *                         for 200 ms").  Never sees voltages.
 *
 * This split is consistent with where `ServoMotionProfile` lives
 * (sfx_board/motion/) — generic per-element / per-actuator behaviour
 * belongs on the role layer, not duplicated inside each effect.
 *
 * Phase 2 of the GunFX rollout (instructions/22) promotes the
 * element-voltage concern out of `SmokeConfig` into the role layer.
 *
 * Scaling modes:
 *   - LINEAR     `duty = elementMv / portMv * requestedPct` — correct
 *                for resistive elements (heaters, incandescent bulbs)
 *                whose dissipated power is V²/R and where the average
 *                voltage is what matters.
 *   - QUADRATIC  `duty = (elementMv / portMv)² * requestedPct` —
 *                power-mode scaling; useful when you want to think in
 *                power terms (e.g. cap a heater at 50 % power, not
 *                50 % voltage).
 *   - PASSTHROUGH `duty = requestedPct` — operator-driven, ignore
 *                  element/port voltages.  Used when the element is
 *                  intentionally on a matched rail or when the
 *                  operator wants raw control.
 *
 * `elementMv == 0` OR `portMv == 0` also falls back to PASSTHROUGH —
 * "unknown" voltages can't be safely scaled.
 */

#ifndef SFX_ELEMENT_SCALING_H
#define SFX_ELEMENT_SCALING_H

#include <cstdint>

namespace sfx_core {

enum class ElementScalingMode : uint8_t {
    Passthrough = 0,   ///< duty = requestedPct (no scaling)
    Linear      = 1,   ///< duty = (Ve/Vp) * requestedPct      — resistive
    Quadratic   = 2,   ///< duty = (Ve/Vp)² * requestedPct     — power-mode
};

struct ElementConfig {
    uint16_t           elementMv = 0;                          ///< element's rated voltage (0 = unknown → passthrough)
    ElementScalingMode mode      = ElementScalingMode::Linear;
};

/// Convert an intent percentage (0..100) into a port-native duty
/// (0..portMaxDuty), honouring the element's rated voltage relative
/// to the port rail and the chosen scaling mode.
///
/// Returns 0 if `requestedPct` is 0 (off is always off).  Caps at
/// `portMaxDuty` even when the requested pct exceeds 100 (defensive).
inline uint16_t scaleDuty(uint8_t            requestedPct,
                          uint16_t           portMaxDuty,
                          uint16_t           portMv,
                          const ElementConfig& elem) {
    if (requestedPct == 0 || portMaxDuty == 0) return 0;

    // Clamp intent to [0, 100] — defensive against config errors.
    if (requestedPct > 100) requestedPct = 100;

    // Compute the duty *ratio* (0..1).
    float ratio = float(requestedPct) * 0.01f;

    if (elem.mode != ElementScalingMode::Passthrough
        && elem.elementMv > 0 && portMv > 0 && elem.elementMv < portMv) {
        const float vRatio = float(elem.elementMv) / float(portMv);
        switch (elem.mode) {
            case ElementScalingMode::Linear:    ratio *= vRatio;          break;
            case ElementScalingMode::Quadratic: ratio *= vRatio * vRatio; break;
            default: break;
        }
    }
    // elementMv >= portMv → element rated at OR above the rail; passthrough
    // is the safe choice (can't deliver more than rail anyway).

    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return static_cast<uint16_t>(float(portMaxDuty) * ratio + 0.5f);
}

}  // namespace sfx_core

#endif  // SFX_ELEMENT_SCALING_H
