import { describe, it, expect } from 'vitest'
import { render } from '@testing-library/svelte'
import ServoIoWidget from './ServoIoWidget.svelte'

// Component (render) test — exercises the REAL Svelte template + reactivity,
// the layer Vitest pure-logic tests can't reach. This is the "test the GUI"
// gap that a Playwright browser run would cover, but runs headless in the gate.

describe('ServoIoWidget', () => {
    it('renders NO SIGNAL when the input is present but invalid', () => {
        const { getByText } = render(ServoIoWidget, { hasInput: true, inputValid: false })
        expect(getByText('NO SIGNAL')).toBeTruthy()
    })

    it('shows the live input µs when the signal is valid', () => {
        const { getByText } = render(ServoIoWidget, {
            hasInput: true, inputValid: true, inputUs: 1500,
        })
        expect(getByText('1500 µs')).toBeTruthy()
    })

    it('renders the live servo position + the OPEN-position marker', () => {
        const { container } = render(ServoIoWidget, {
            hasServo: true, minUs: 1100, maxUs: 1900, openUs: 1850,
            servo: { posUs: 1600, targetUs: 1600, velUsPerS: 0 },
        })
        expect(container.textContent).toContain('1600 µs')
        expect(container.querySelector('.servo-track-open')).toBeTruthy()
    })

    it('reactively updates when props change (the reactivity-trap class)', async () => {
        const { container, component } = render(ServoIoWidget, {
            hasServo: true, servo: { posUs: 1200, targetUs: 1200, velUsPerS: 0 },
        })
        expect(container.textContent).toContain('1200 µs')
        await component.$set({ servo: { posUs: 1800, targetUs: 1800, velUsPerS: 0 } })
        expect(container.textContent).toContain('1800 µs')
        expect(container.textContent).not.toContain('1200 µs')
    })
})
