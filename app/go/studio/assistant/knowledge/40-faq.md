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
The AI assistant needs an API key (Gemini or Groq), set in **View → Settings…**.
It's stored locally on your machine.

**Q: Will the assistant answer general questions?**
No — it only helps with ScaleFX configuration and setup. Ask it about your
effects, channels, ports, the wizard, or the Console.
