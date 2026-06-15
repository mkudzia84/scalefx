<!-- LLM/FAQ grounding for the ScaleFX Studio assistant.  USER/SETUP perspective
     only — no firmware internals, protocol details, or low-level names.  Refer
     to ports and effects by their human-readable Studio labels. -->

# ScaleFX — what it is

ScaleFX adds realistic effects to RC scale models (warbirds, jets, helis, ships,
vehicles): **engine sound, gunfire, retractable landing gear, navigation and
landing lights, and alerts** — all driven from your RC radio. You set everything
up in the **ScaleFX Studio** desktop app, which talks to the model's main
controller over USB.

## The pieces (in plain terms)

- **The main controller** runs all the effects, reads your RC receiver, and
  plays sound through its speaker output. Studio connects to this.
- **Optional expander boards** simply add more outputs (extra servo and motor
  connections, more LED channels). Once plugged in, their outputs appear in
  Studio alongside the main controller's — you use them the same way.

You don't need to think about which board a connection is on; Studio shows them
all together, each with a friendly name.

## Outputs, inputs, and "what's plugged into what"

Every physical connection is a **port** with a type that decides what you can
plug into it:

- **Servo ports** — drive hobby servos (gear doors, gun turret, retracts).
- **LED / PWM ports** — drive LEDs, a smoke heater, or a small fan/motor.
- **Motor ports** — drive a bidirectional DC motor (e.g. a landing-gear leg).
- **Input ports** — connect to your RC receiver.

When you set up an effect, you pick which port each part uses; Studio only offers
ports of the right type that are still free, and gives each port a readable name
(an operator label you set, or its on-board silkscreen label like "SRV1").

## Radio channels by name

You map each RC channel to a **named function** — e.g. channel 5 → *landing
gear*, channel 6 → *gun fire*, a switch → *engine on/off*. Effects then refer to
the **name**, not the channel number, so if you move a function to a different
channel later, the effects follow automatically.

## How configuration works

You edit settings in Studio (they're held as a draft), then press **Apply** to
save them to the model and have them take effect — no power cycle needed. The
easiest way to go from nothing to a working setup is the **Setup Wizard**.
