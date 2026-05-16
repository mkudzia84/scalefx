/*
 * StallCalibrator (placeholder) — promotion deferred.
 *
 * The production calibrator lives at
 *   controllers/gearcontrol/pico/src/stall_calibrator.{h,cpp}
 *
 * It's a 250-line state machine (CLEAR_RUN → DEPLOY_RUN → MID_SETTLE
 * → RETRACT_RUN → COMPLETE) that drives a motor via injected
 * callbacks, measures peak currents in both directions, computes a
 * baseline-aware stall threshold with an 80 % safety margin, and
 * emits progress updates.  All the substantive behaviour is
 * already there.
 *
 * Promotion to this directory is **deferred** because two of its
 * dependencies are still gear-specific:
 *
 *   1. `<serial/gearcontrol/gearcontrol.h>` — provides `CalibPhase`
 *      and `GearControlCalibStatus` (the wire-format struct emitted
 *      via `onProgress`).  Generalising means defining a new
 *      board-agnostic `CalibStatus` POD here, plus a new generic
 *      phase enum.
 *
 *   2. `DoorSequencer*` — used to coordinate door open/close around
 *      the calibration motor runs.  Generalising means replacing
 *      the pointer with a `std::function<bool(Phase)>` callback the
 *      caller provides — non-gear boards pass an empty function.
 *
 * Both generalisations are mechanical but touch the wire format the
 * GearControl protocol exposes.  They land in **Step 2** of the
 * slave migration (the GearControl PR), where the gearcontrol
 * protocol header is being deleted anyway and the calibration
 * progress emission becomes a master-side concern.
 *
 * Until then: master-side calibration uses the lower-level
 * `PWM_SET_STALL_GUARD` / `PWM_CLEAR_STALL` / `PWM_STALL` packets
 * defined in slave.h (see refactor plan §"Endpoint calibration via
 * stall — master-side state machine") and runs the calibration
 * sequence directly without needing this class.  `StallCalibrator`
 * becomes useful again as a hub-side helper once it's generalised.
 *
 * See instructions/15-GENERIC-SLAVE-REFACTOR.md §"Motor primitives
 * — promote from gearcontrol, don't reinvent".
 */

#ifndef SFX_STALL_CALIBRATOR_H
#define SFX_STALL_CALIBRATOR_H

// Intentionally empty header — promoted version lands in the GearControl
// migration PR.  Including this file is fine; it just doesn't expose
// anything yet.

#endif  // SFX_STALL_CALIBRATOR_H
