# The Console

The **Console** is a slide-out panel on the right edge of Studio (open it from
the **Console** ticker, or **View → Console**). It does two things:

- **Watch live messages** from the model — status updates and diagnostics scroll
  by as you use the model or the radio.
- **Type commands** — there's an input line at the bottom; press **Enter** to
  run a command. Use the **Up / Down arrows** to recall previous commands.

Most setup is done through the tabs and the Setup Wizard — the Console is an
optional power tool for **inspecting** the model and **troubleshooting**.

## Useful commands

These work for everyone (no prefix):

- **`connect`** — connect to the model over USB.
- **`init`** — initialise the model and report its firmware version + build.
- **`status`** — show the model's current run status.
- **`system-info`** — controller details (firmware, free memory, capabilities,
  attached expander boards + their battery readings).
- **`config.status`** — show the saved configuration state.
- **`file.list`** — list files on the model's memory card (e.g. sound files).
- **`sd.status`** — memory-card status (present, size, free space).
- **`subscribe`** — stream live events + diagnostic messages until you
  disconnect (handy for watching what the radio/effects are doing).

## Commands for a specific effect

Effect commands are grouped by a prefix so they're unambiguous:

- **`hub:`** — the main controller (e.g. `hub:topo-ports` lists every port and
  the role attached to it; `hub:slaves` lists connected expander boards).
- **`gear:`** — landing-gear commands (e.g. `gear:reset`).
- **`gun:`** — gun commands (e.g. `gun:trigger`).
- **`light:`** — lighting commands.

Tip: if you type a bare effect command without its prefix, the Console suggests
the right one. When in doubt, `system-info` and `hub:topo-ports` are the safest
way to see exactly what the model has and how it's wired.

> Safety: Console commands can drive real hardware (servos, motors, gear). Only
> trigger movement with the model supported and supervised on the bench.
