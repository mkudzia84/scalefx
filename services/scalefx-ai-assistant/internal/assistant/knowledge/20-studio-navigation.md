# ScaleFX Studio — getting around

Layout, top to bottom:

1. **Config bar (top)** — always visible. Holds the **Setup Wizard** button (the
   wand), a board **Diagram**, **Refresh**, the **RC routing** toggle (whether
   the radio drives effects or you're in manual control), the **Auto-apply**
   toggle, a **status** indicator (in sync / unapplied changes / errors), and the
   **Apply** button. Apply saves all your changes to the model and makes them
   take effect.
2. **Tabs** — one per area: **Input & Ports**, **Engine**, **Gun**, **Gear**,
   **Lighting**, **Firmware**, and so on. Some tabs have **sub-tabs** (e.g.
   Lighting → Programs | Retractable).
3. **Right-edge docks** — slide-out **Console** (type commands / watch live
   messages) and **Assistant** (this chat). Open them from the tickers on the
   right edge. A **status bar** sits at the bottom.

## Applying your changes

- Edits are held as a **draft** — nothing reaches the model until you **Apply**.
- **Auto-apply** (a toolbar toggle, on by default): a few seconds after you stop
  editing, a countdown runs and then Applies for you. Press **Hold** to keep
  editing, or **Apply** to commit now.
- Apply is **blocked while there are validation errors** — fix anything flagged
  in red first.

## The Setup Wizard (start here)

The **Setup Wizard** (wand button, top-left) is the guided path: pick features →
set up the radio input → map channels to functions → configure each effect (with
ports assigned for you) → review → Apply. Use it for first-time setup or whenever
the full tabs feel like too much.

## Per-effect tabs

Each effect has its own tab for fine-tuning after the wizard. Output ports are
chosen from the free, compatible ports; Studio attaches the right behaviour
automatically when you pick one.

## Calibrating servos

Set a servo's travel (its end-points, centre, and direction) using the
**Calibrate Servo…** button inside the effect's panel — not on the Input & Ports
tab. After calibrating, the effect drives the servo within those limits, and
re-calibrating takes effect on the next move.

## Connecting

Studio connects to the model's main controller over USB (use **File → Connect**
or the Connect button). If an expander board is configured but unplugged, its
ports still show (dimmed) so your setup is kept until it's reconnected. Connect
the controller so the assistant can see your current setup and give specific
advice.

## Settings

**View → Settings…** holds display options (theme, font size). The AI assistant
needs no configuration here — it talks to a ScaleFX assistant service that owns
the provider keys. The assistant's model is picked in the Assistant dock itself
(a dropdown when several are offered, otherwise just the one in use).
