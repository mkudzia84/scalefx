import { describe, it, expect } from 'vitest'
import { commitDuties } from './motor_calibration'

// "Save to strut" must keep the operator's Forward/Reverse direction (the sign of
// the strut's existing deployDuty) while taking the MAGNITUDE from the calibrated
// seek duty.  Regression: it used to hard-code +abs/-abs, resetting a Reverse
// strut to Forward on every save.
describe('commitDuties (gear motor direction preserved on Save to strut)', () => {
    it('keeps a FORWARD strut forward', () => {
        expect(commitDuties(20000, 18000)).toEqual({ deployDuty: 18000, retractDuty: -18000 })
    })
    it('keeps a REVERSE strut reversed', () => {
        expect(commitDuties(-20000, 18000)).toEqual({ deployDuty: -18000, retractDuty: 18000 })
    })
    it('uses the calibrated magnitude regardless of the seed magnitude', () => {
        expect(commitDuties(-12345, 30000)).toEqual({ deployDuty: -30000, retractDuty: 30000 })
    })
    it('treats a 0/unset seed as forward', () => {
        expect(commitDuties(0, 20000)).toEqual({ deployDuty: 20000, retractDuty: -20000 })
    })
    it('normalises a negative calibrated duty to a magnitude', () => {
        expect(commitDuties(20000, -15000)).toEqual({ deployDuty: 15000, retractDuty: -15000 })
    })
})
