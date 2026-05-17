/*
 * PwmDutyAdapter<TPwms> — adapts a PwmCollection as a PwmOutput backend.
 *
 * `LedRuntime<N, TPwm>` templates on the `PwmOutput` concept (defined in
 * `pwm/pwm_output.h`).  Two backends satisfy that concept directly —
 * `NativeGpio` (MCU pins via analogWrite) and `PCA9685` (I²C PWM chip).
 *
 * For PWM-borrowed LED channels — PwmCollection channels temporarily in
 * `ComponentKind::PwmLed` mode — we need a third backend that writes
 * 0..255 brightness through the PwmCollection's `writeDuty(idx, 0..1000)`
 * interface.  This adapter does that mapping in one place so the LED
 * runtime can be the same template instantiated against the adapter
 * type, no special-case code.
 *
 * Usage on an expander board:
 *
 *   PwmCollection<6, MySense>                        pwms;
 *   PwmDutyAdapter<decltype(pwms)>                   pwmDuty(pwms);
 *   LedRuntime<6, decltype(pwmDuty)>                 borrowedLeds;
 *   uint8_t borrowedPins[6] = { 0, 1, 2, 3, 4, 5 };  // PWM channel indices
 *   borrowedLeds.begin(&pwmDuty, borrowedPins);
 *
 *   // CoreServer holds both `borrowedLeds` and `dedicatedLeds` and
 *   // routes LED commands by address-byte bit 7.  When a PWM channel
 *   // leaves PwmLed mode, CoreServer calls
 *   //   borrowedLeds.stopQueue(idx); borrowedLeds.clearQueue(idx);
 *   // via the PwmCollection's mode-change event callback.
 *
 * Trust boundary:
 *   The adapter writes unconditionally — it does NOT gate on the
 *   channel's current ComponentKind.  Gating happens above, in the
 *   wire dispatcher and in the mode-change cleanup hook.  After the
 *   cleanup hook runs (stop + clear), the LedRuntime's update() won't
 *   emit anything because LedEventSeq isn't playing — so no stale
 *   writes can reach the channel.
 */

#ifndef SFX_PWM_DUTY_ADAPTER_H
#define SFX_PWM_DUTY_ADAPTER_H

#include <cstdint>

namespace sfx_peripherals {

template <typename TPwms>
class PwmDutyAdapter {
public:
    /// Channel count surface — matches the underlying collection.
    static constexpr uint8_t NUM_PINS  = (uint8_t)TPwms::COUNT;
    static constexpr bool    HAS_HW_PWM = true;

    explicit PwmDutyAdapter(TPwms& pwms) : _pwms(pwms) {}

    // ── PwmOutput concept surface ─────────────────────────────────

    bool isAvailable() const { return true; }

    /// PWM channels are output-only — no-op, mirrors PCA9685's behaviour.
    bool setPinDirection(uint8_t /*idx*/, bool /*isInput*/) { return true; }

    /// Digital write — for LED use this becomes "full on" / "off".
    bool writePin(uint8_t idx, bool high) {
        return setLedBrightness(idx, high ? 255 : 0);
    }

    /// 0..255 brightness → 0..1000 PWM duty thousandths (matches the
    /// scale that PwmCollection::writeDuty expects).  Rounded.
    bool setLedBrightness(uint8_t idx, uint8_t brightness) {
        if (idx >= TPwms::COUNT) return false;
        const uint16_t duty = (uint16_t)(((uint32_t)brightness * 1000u + 127u) / 255u);
        _pwms.writeDuty(idx, duty);
        return true;
    }

private:
    TPwms& _pwms;
};

}  // namespace sfx_peripherals

#endif  // SFX_PWM_DUTY_ADAPTER_H
