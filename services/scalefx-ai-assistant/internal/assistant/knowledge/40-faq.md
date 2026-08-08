# FAQ — common questions

**Q: How do I start? It looks intimidating.**
Open the **Setup Wizard** (the wand button, top-left). It walks you through
features → radio input → channel mapping → each effect, with sensible defaults,
and applies everything at the end.

**Q: My change didn't take effect.**
Changes are held as a draft until you **Apply**. Press **Apply** (top-right) or
wait for **Auto-apply**'s countdown. If Apply is greyed out, you have a
validation error (shown in red) — fix it first.

**Q: Nothing happens when I move the stick.**
Check three things: (1) the toolbar's RC routing toggle is on "RC routing" (not
manual); (2) the channel is mapped to the right function on the Input & Ports
tab; (3) the effect's trigger threshold matches your switch. The live channel
bars on the Input & Ports tab show what the model is receiving.

**Q: Which port do I use for X?**
Match the port type to the part: servo ports for doors/turret/retracts, LED ports
for LEDs (and the smoke heater/fan), motor ports for gear legs. The wizard only
offers compatible free ports and sets them up for you.

**Q: My gear deploys the wrong way / stops short.**
Flip the deploy *direction* in the gear leg settings, and make sure the servo and
travel are calibrated. If the travel *timeout* is too short, the leg stops before
it reaches the end.

**Q: Can I use off-the-shelf / integrated retracts instead of the ScaleFX motor driver?**
Yes. Set **Strut drive** on the gear tab: `Servo per strut` gives every strut
its own servo (PWM) channel to its built-in controller, and `Servo shared`
drives the whole undercarriage from ONE channel. Those controllers give no
feedback, so you set a **Travel time** per strut (or one shared) — the door
sequence waits that long before closing. Time a full stroke and add a safety
margin. Doors stay on regular servo channels in every mode.

**Q: My gear motor is rated 6 V but the model runs a bigger pack — will it burn out?**
No — the gear drive is voltage-first: each strut declares its **Motor V**
(drive voltage, set in the **Calibrate motor…** window) and the firmware
delivers exactly that average at the motor on ANY pack, using the live
per-motor voltage reading — it even compensates as the battery sags during
flight. Set it to the voltage the mechanism was actually tuned for (e.g.
nominal-6 V retracts are often happily driven at 9 V). Default 6 V, minimum
1 V; there are no raw duty numbers to tune any more.

**Q: Can I open/close the gear doors or move the strut by hand during setup?**
Yes. Each strut card has a **Manual / maintenance** section with Open/Close doors
and Strut down/up buttons (plus fleet "all" buttons), so you can move the doors
and the strut independently of the full deploy/retract sequence. For safety the
firmware blocks the unsafe combinations: you can't close the doors while the strut
is out, and you can't move the strut unless the doors are open — the matching
button greys out and tells you why.

**Q: How do I calibrate a servo?**
In the effect's panel, use **Calibrate Servo…**. Set the end-points, centre, and
direction; re-calibrating takes effect on the next move.

**Q: My expander board's ports aren't showing.**
Make sure it's plugged into the controller and powered. A configured-but-
unplugged board shows dimmed; its setup is kept until it returns.

**Q: How do I change the battery cutoff?**
On a battery-capable board, the battery card shows pack voltage; set the cutoff
voltage and chemistry there. Alerts fire below the cutoff.

**Q: Can the assistant change my settings for me?**
No — by design it advises and points you to the wizard or the right tab. You
apply changes yourself so nothing reconfigures your hardware unattended.

**Q: Can I type commands?**
Yes — the **Console** (right edge) has a command line. `system-info` and
`hub:topo-ports` are the handiest for seeing what your model has and how it's
wired. See the Console section.

**Q: Do I need internet / an API key for the assistant?**
You don't manage any API key — the assistant talks to a ScaleFX assistant service
that holds the provider keys for you. Studio just needs to reach that service.
The FAQ tab works without making an AI request; chat asks a model through the
service and is lightly rate-limited.

**Q: Will the assistant answer general questions?**
No — it only helps with ScaleFX configuration and setup. Ask it about your
effects, channels, ports, the wizard, or the Console.

**Q: How loud is my system / how many watts is the audio using?**
The **Firmware tab** has an **Audio Power** card (firmware 2.42.0+): it shows the
amplifier's supply voltage (measured by the chip itself), the auto-picked analog
gain, the live output level, and an estimated wattage with a 4 Ω / 8 Ω speaker
toggle (default 4 Ω). The wattage is an estimate from signal level and gain, not
a current measurement, and the speaker impedance can't be autodetected — pick
the one matching your speakers.

**Q: Do I need to set the amplifier / codec supply voltage?**
Not anymore. Since firmware 2.42.0 the codec measures its own supply rail at
every boot and picks the analog gain to match — a bigger battery automatically
gives you more clean output. The old `codec_supply` config key is ignored.
