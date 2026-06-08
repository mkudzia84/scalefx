import { describe, it, expect } from 'vitest'
import { get } from 'svelte/store'
import { deviceModel, studioTabs } from './devicemodel'
import { connectionInfo } from './stores'

// Tab gating by connected board (the "only Firmware tab for non-HubFX boards"
// rule). studioTabs is a derived store over [deviceModel, connectionInfo].
function setConn(connected: boolean, controllerType: string) {
    connectionInfo.update(c => ({ ...c, connected, controllerType }))
}
function setDomains(ids: string[]) {
    deviceModel.update(m => ({ ...m, domains: ids.map(id => ({ id, label: id } as any)) }))
}

describe('studioTabs gating by connected board', () => {
    it('a connected gearcontrol expander shows ONLY its Diagnostics + Firmware tabs', () => {
        setDomains(['gearcontrol']) // even with capability domains present…
        setConn(true, 'gearcontrol')
        const keys = get(studioTabs).map(t => t.key)
        expect(keys).toEqual(['gear-diag', 'firmware'])
        expect(keys).not.toContain('io') // Input & Ports is HubFX-only
    })

    it('a connected non-gearcontrol expander shows ONLY the Firmware tab', () => {
        setConn(true, 'lightfx')
        const keys = get(studioTabs).map(t => t.key)
        expect(keys).toEqual(['firmware'])
    })

    it('a connected HubFX shows the full capability-gated set (incl. IO + Firmware)', () => {
        setDomains(['engine', 'gun', 'lighting'])
        setConn(true, 'hubfx')
        const keys = get(studioTabs).map(t => t.key)
        expect(keys).toContain('io')
        expect(keys).toContain('firmware')
        expect(keys).toContain('engine')
        expect(keys).toContain('gun')
        expect(keys.length).toBeGreaterThan(1)
    })

    it('pre-connect (controllerType unset) keeps the full set — not restricted', () => {
        setDomains([])
        setConn(false, '')
        const keys = get(studioTabs).map(t => t.key)
        expect(keys).toContain('io')
        expect(keys).toContain('firmware')
    })

    it('connected but unidentified (empty controllerType) is NOT restricted', () => {
        setConn(true, '')
        const keys = get(studioTabs).map(t => t.key)
        expect(keys).toContain('io') // unknown board → don't hide tabs on uncertainty
    })
})
