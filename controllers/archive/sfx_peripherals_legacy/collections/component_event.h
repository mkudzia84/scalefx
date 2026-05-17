/*
 * ComponentEvent — uniform state-change notification for collections.
 *
 * Lets board firmware hook component state to **board-local
 * concerns** that aren't part of the slave protocol — typically
 * indicator LEDs (the board's status / diagnostic LEDs in
 * `sfx_peripherals/indicators/indicator_leds.h`), buzzers, or
 * diag-log output.  These are NOT the master-controlled application
 * LEDs in the slave's `LedCollection`; the two are separate concerns
 * and live on different wires.
 *
 * Each collection exposes a single `setEventCallback(fn)` that fires
 * on every meaningful state transition with the channel index and a
 * `ComponentEvent` tag.  Board firmware writes one tiny dispatcher
 * mapping (collection, idx, event) → board indicator LED action:
 *
 *   pwms.setEventCallback([&](uint8_t idx, ComponentEvent ev, uint16_t data) {
 *       if (idx == MOTOR_RETRACT) {
 *           switch (ev) {
 *               case ComponentEvent::MotionStarted:  motorIndicator.flash(GREEN); break;
 *               case ComponentEvent::MotionEnded:    motorIndicator.solid(GREEN); break;
 *               case ComponentEvent::StallDetected:  motorIndicator.solid(RED);   break;
 *               case ComponentEvent::StallCleared:   motorIndicator.solid(GREEN); break;
 *               default: break;
 *           }
 *       }
 *   });
 *
 * Events are deliberately broad — same enum across servo / PWM / LED
 * — so the board's hookup code is uniform.  Some events are no-ops
 * for some component kinds (e.g. ProgramStarted never fires on a
 * servo).
 */

#ifndef SFX_COMPONENT_EVENT_H
#define SFX_COMPONENT_EVENT_H

#include <cstdint>
#include <functional>

namespace sfx_peripherals {

enum class ComponentEvent : uint8_t {
    Activated       = 0,    ///< collection attach()'d the channel
    Deactivated     = 1,    ///< collection detach()'d the channel
    MotionStarted   = 2,    ///< servo target armed; motor speed transitioned 0 → ≠0
    MotionEnded     = 3,    ///< servo at target; motor speed transitioned ≠0 → 0
    StallDetected   = 4,    ///< PWM motor stall guard tripped (fires alongside StallCb)
    StallCleared    = 5,    ///< PWM_CLEAR_STALL acknowledged; channel re-armed
    QueueStarted    = 6,    ///< LED event queue begun running
    QueueEnded      = 7,    ///< LED event queue finished naturally (REPEAT clear)
    QueueStopped    = 8,    ///< LED event queue stopped by master command
    ModeChanged     = 9,    ///< PWM channel mode swapped (PwmGeneric ↔ PwmMotor ↔ PwmLed ↔ PwmHeater)
    SafeStateEntered= 10,   ///< collection commanded into safe state (keepalive/SHUTDOWN)
    Error           = 11,   ///< component-specific error (timeout, motor not detected, etc.)
};

/// Standardised callback signature.  Channel `idx` is 0-based,
/// matching the wire-format addressing.  `data` carries optional
/// event-specific payload (e.g. motor speed at MotionStarted, peak
/// current at StallDetected) — packed into a single uint16_t to keep
/// the signature small.  Events that don't need a payload pass 0.
using ComponentEventCb = std::function<void(uint8_t idx, ComponentEvent ev, uint16_t data)>;

}  // namespace sfx_peripherals

#endif  // SFX_COMPONENT_EVENT_H
