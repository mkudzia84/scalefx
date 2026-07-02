# pcb-nextver — hardware exports

Drop EasyEDA exports of the new PCB revision here for analysis:

- `*.net` / `*.txt`  — **netlist** (Export → Netlist) — exact connectivity, preferred
- `*.pdf`            — **schematic PDF** (Export → PDF, all sheets, vector)
- `*.csv`            — **BOM** (optional)
- `old/…`            — the previous revision's netlist/PDF for a diff (optional)

Name them by board + rev, e.g. `hubfx_revC.net`, `hubfx_revC_sch.pdf`.
Then tell Claude the path and it will extract the pin/peripheral map and diff it against the current firmware config.
