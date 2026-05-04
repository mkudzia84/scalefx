// LightFX virtual board — default demo program.
//
// Real firmware loads /lightfx.yaml from flash on boot. The virtual
// board has no flash, so without a default it would boot with every
// channel at 0 % and no sequences playing — the GUI would correctly
// report "nothing happening" but it'd be misleading to anyone trying
// to verify Studio's LED panel works.
//
// DefaultDemoConfig returns a single-program config that exercises the
// six animated event types across the eight channels:
//
//   ch1 — beacon (cycle 1500 ms, 15 % flash window)        anti-collision
//   ch2 — flash (250 ms, 50 % duty)                        strobe
//   ch3 — fading sinusoid (cycle 2 s)                      breathing
//   ch4 — fade-in to steady on (1 s ramp, then hold)       runway / position
//   ch5 — beacon mask via landing group 1 (off by policy)
//   ch6 — beacon mask via landing group 1 (off by policy)
//   ch7 — steady on @ 60 %                                 nav
//   ch8 — slow flash (1 s, 50 % duty)                      tail strobe
//
// Slot 1 is bound to channels 5 + 6 with no servo, so a Studio user
// can hit "Deploy" / "Retract" on the landing-light card and watch
// those channels jump to 100 % / 0 % live.

package lightfx

import "scalefx/tests/virtual_board/shared"

// DefaultDemoConfig returns the demo LightFX program loaded at startup
// when -no-demo isn't passed.
func DefaultDemoConfig() LightProgramConfig {
	return LightProgramConfig{
		MasterBrightness: 100,
		LandingGroups: []LandingGroupDef{
			{
				Name:        "Demo Gear",
				ServoID:     0,    // LED-only group (no servo)
				ChannelMask: 0x30, // bits 4 + 5 → channels 5 + 6
				Brightness:  100,
			},
		},
		Programs: []LightProgram{
			{
				Name:          "DEMO",
				GroupPolicies: []uint8{GroupPolicyOff}, // gear retracted by default
				Channels: []ChannelDef{
					{
						Channel:    1,
						GroupIndex: 0xFF,
						Events: []shared.SeqEvent{
							{Type: shared.EvtBeacon, P1: 1500, P2: 0,
								P3: 15, P4: 100, P5: 0},
						},
					},
					{
						Channel:    2,
						GroupIndex: 0xFF,
						Events: []shared.SeqEvent{
							{Type: shared.EvtFlash, P1: 250, P2: 0,
								P3: 100, P4: 50},
						},
					},
					{
						Channel:    3,
						GroupIndex: 0xFF,
						Events: []shared.SeqEvent{
							{Type: shared.EvtFading, P1: 2000, P2: 0,
								P3: 0, P4: 100},
						},
					},
					{
						Channel:    4,
						GroupIndex: 0xFF,
						Events: []shared.SeqEvent{
							{Type: shared.EvtFadeIn, P1: 1000, P3: 100},
							{Type: shared.EvtOn, P1: 0, P3: 100}, // terminal hold
						},
					},
					// Channels 5 + 6 are landing-group controlled — no events here.
					{Channel: 5, GroupIndex: 0},
					{Channel: 6, GroupIndex: 0},
					{
						Channel:    7,
						GroupIndex: 0xFF,
						Events: []shared.SeqEvent{
							{Type: shared.EvtOn, P1: 0, P3: 60},
						},
					},
					{
						Channel:    8,
						GroupIndex: 0xFF,
						Events: []shared.SeqEvent{
							{Type: shared.EvtFlash, P1: 1000, P2: 0,
								P3: 100, P4: 50},
						},
					},
				},
			},
		},
	}
}

// LoadDemo loads the default demo config, selects program 0, and
// seeds `/lightfx.yaml` on the faux flash so Studio's File Manager
// and config loader see realistic content. Used by the virtual_board
// CLI when -no-demo is not set.
func (b *Board) LoadDemo() {
	b.LoadProgramConfig(DefaultDemoConfig())
	b.SelectProgram(0)
	if b.fs != nil {
		_ = b.fs.Seed(1 /*flash*/, "/lightfx.yaml", []byte(demoYaml))
		_ = b.fs.Seed(1 /*flash*/, "/system/version.txt",
			[]byte("LightFX-Virtual\n0.99.0-virt\n"))
	}
}

// demoYaml is a roughly-canonical /lightfx.yaml that mirrors what the
// demo program loads in code. Seeded into the faux filesystem so the
// Studio config-loader has a real document to round-trip on.
const demoYaml = `master_brightness: 100

landing_groups:
  - name: "Demo Gear"
    servo_board: lightfx
    servo_id: 0
    lightfx_channels: 0x30   # bits 4 + 5 → channels 5 + 6
    hubfx_channels:   0x00
    brightness: 100

programs:
  - name: "DEMO"
    group_policies: [off]
    channels:
      - board: lightfx
        channel: 1
        events:
          - type: beacon
            cycle_ms: 1500
            flash_pct: 15
            max_brightness: 100
            min_brightness: 0
      - board: lightfx
        channel: 2
        events:
          - type: flash
            cycle_ms: 250
            brightness: 100
            duty: 50
      - board: lightfx
        channel: 3
        events:
          - type: fading
            cycle_ms: 2000
            min_brightness: 0
            max_brightness: 100
      - board: lightfx
        channel: 4
        events:
          - type: fade_in
            duration_ms: 1000
            brightness: 100
          - type: on
            brightness: 100
      - board: lightfx
        channel: 5
        group: 0
      - board: lightfx
        channel: 6
        group: 0
      - board: lightfx
        channel: 7
        events:
          - type: on
            brightness: 60
      - board: lightfx
        channel: 8
        events:
          - type: flash
            cycle_ms: 1000
            brightness: 100
            duty: 50
`

