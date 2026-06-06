import { describe, it, expect } from 'vitest'
import { colourize, tagColour, phaseColour } from './console_colour'

describe('tagColour', () => {
    it('maps severity + module tags to palette classes', () => {
        expect(tagColour('[LOG E]')).toBe('c-red')
        expect(tagColour('[LOG W]')).toBe('c-yellow')
        expect(tagColour('[LOG I]')).toBe('c-cyan')
        expect(tagColour('[GUNFX]')).toBe('c-red')
        expect(tagColour('[LL]')).toBe('c-blue')
        expect(tagColour('[ENG]')).toBe('c-green')
        expect(tagColour('[gap]')).toBe('c-yellow')
    })
})

describe('phaseColour (mirrors CLI term.go Phase)', () => {
    it('green for live, yellow for transitions, red for faults, dim for idle', () => {
        expect(phaseColour('deployed')).toBe('c-green')
        expect(phaseColour('firing')).toBe('c-green')
        expect(phaseColour('deploying')).toBe('c-yellow')
        expect(phaseColour('error')).toBe('c-red')
        expect(phaseColour('retracted')).toBe('c-dim')
        expect(phaseColour('servo')).toBe('') // not a state word
    })
})

describe('colourize', () => {
    it('wraps the leading tag, dims the timestamp, cyans the values', () => {
        const out = colourize('[LOG I] @78605ms [servo] setprofile idx=2 min=1373 max=1587')
        expect(out).toContain('<span class="c-cyan">[LOG I]</span>')
        expect(out).toContain('<span class="c-dim">@78605ms</span>')
        expect(out).toContain('idx=<span class="c-cyan">2</span>')
        expect(out).toContain('min=<span class="c-cyan">1373</span>')
        expect(out).toContain('max=<span class="c-cyan">1587</span>')
    })

    it('colours a GUID value magenta + the [gap] tag yellow', () => {
        const out = colourize('[gap] light[0] servo[0] ref guid=6D60')
        expect(out).toContain('<span class="c-yellow">[gap]</span>')
        expect(out).toContain('guid=<span class="c-cyan">6D60</span>')
    })

    it('colours a phase word', () => {
        const out = colourize('landing[0] is deployed')
        expect(out).toContain('<span class="c-green">deployed</span>')
    })

    it('does not double-wrap (single pass) — class names contain no re-matchable tokens', () => {
        const out = colourize('idx=2')
        // exactly one wrap of the value, not nested
        expect(out).toBe('idx=<span class="c-cyan">2</span>')
        expect(out.match(/<span/g)?.length).toBe(1)
    })

    it('colours a standalone hex magenta, but a hex VALUE (after key=) cyan', () => {
        expect(colourize('die 0x5449')).toContain('<span class="c-magenta">0x5449</span>')
        expect(colourize('mfg=0x5449')).toContain('mfg=<span class="c-cyan">0x5449</span>')
    })
})
