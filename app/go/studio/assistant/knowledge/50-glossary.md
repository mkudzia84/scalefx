# Glossary

- **Main controller** — the board that runs the effects, reads the radio, and
  plays sound. Studio connects to it.
- **Expander board** — an add-on board that gives you more output connections
  (servo, motor, LED). Its ports appear in Studio next to the main controller's.
- **Port** — a physical connection on a board. Its **type** decides what plugs
  in: **servo**, **LED/PWM**, **motor**, or **input** (receiver).
- **Port label** — a port's human-readable name: the label you give it, or its
  on-board silkscreen marking (like "SRV1"). Use these, not internal ids.
- **Channel function** — a name you assign to an RC channel (*engine on/off*,
  *landing gear*, *landing lights*, *gun fire*, *gun fire mode*, *gun smoke*,
  *gun yaw*, *gun pitch*, *light program*, *light brightness*, *master volume*).
  Effects refer to the name, not the channel number.
- **Draft** — your in-progress edits in Studio. They become live only when you
  **Apply**.
- **Apply** — save all your changes to the model and make them take effect (no
  power cycle).
- **Unapplied / dirty** — you have edits not yet applied (shown in the top bar).
  **In sync** = nothing to apply.
- **Auto-apply** — an optional toolbar mode that applies a few seconds after you
  stop editing, with a visible countdown and a Hold button.
- **Setup Wizard** — the guided, step-by-step setup (the wand button).
- **Failsafe** — what an effect does if the radio signal is lost (e.g. gear
  deploys, engine stops).
- **Stall guard** — gear-motor protection that senses when a leg reaches its end
  and stops the motor.
- **Coordination (gear)** — whether the gear legs move independently or all in
  sync.
- **Calibrate (servo)** — setting a servo's end-points, centre, and direction so
  effects drive it within safe limits.
- **RC routing** — toolbar toggle: whether the radio drives the effects, or
  you've taken manual control from Studio.
- **Console** — the right-edge panel for typing commands and watching live
  messages.
- **SBUS / Jeti / PPM** — common RC receiver link types ScaleFX can read; Jeti
  also sends telemetry back to your transmitter.
