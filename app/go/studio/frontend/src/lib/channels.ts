// channels.ts — the ONE collector for named-RC-channel picker options.
//
// Every effect that binds an RC channel by NAME (Rule 43) — Engine throttle,
// Gun trigger/ROF, Landing activation, LightFx selector, Gear up/down — built
// this same list inline (collectChannels / collectChannelOpts), five drifting
// copies of identical logic.  Hoisted 2026-06-13.
//
// One option per ASSIGNED channel across every input port: `function` names
// the role (engine_toggle, gun_trigger, gear_updown, …) the operator typed on
// the Inputs tab; `unassigned` channels are skipped (no name to bind to).
// The returned shape is a SUPERSET — panels that only need {fnId,label} (Gun,
// LightFx) ignore the port fields; panels that resolve the live µs bar
// (Engine, Gear, Landing) use portGuid/portIdx/channel.

import type { DeviceModelSnapshotT } from './devicemodel'

export interface ChannelOption {
    fnId:     string   // the channel's `function` (role id / operator name)
    label:    string   // "CH3 · Throttle"
    portGuid: string   // input port GUID (for the live-channel key)
    portIdx:  number
    channel:  number   // 0-based channel index within the port
}

/** Build the picker options from the device model.  Pass `$deviceModel` so a
 *  caller's `$:` rebuilds when channel names / assignments change. */
export function collectChannelOptions(dm: DeviceModelSnapshotT): ChannelOption[] {
    const fns = new Map(dm.channelFunctions.map(f => [f.id, f.label] as const))
    const out: ChannelOption[] = []
    for (const inp of dm.inputs) {
        for (const c of inp.channels) {
            if (c.function === 'unassigned') continue
            out.push({
                fnId:     c.function,
                label:    `CH${c.channel + 1} · ${fns.get(c.function) ?? c.function}`,
                portGuid: inp.port.guid,
                portIdx:  inp.port.index,
                channel:  c.channel,
            })
        }
    }
    return out
}
