# Studio bundled preset library

Mirror of [`/media/presets/`](../../../../media/presets/). The canonical
content lives in `/media/`; these files are embedded into the Studio
binary via `//go:embed` so the .exe ships with the factory preset
catalog independent of any repo checkout on the user's machine.

**DO NOT edit files here directly** — edit them in `/media/presets/`
and re-run the sync:

```powershell
# from repo root
./app/go/studio/sync-presets.ps1
```

The script is a verbatim file copy (`media/presets/* → assets/presets/*`).
It's safe to re-run; no diff = no-op.

Go's `//go:embed` directive cannot escape its package directory, so we
can't reference `../../../media/presets/` directly — the mirror is the
mechanism that bridges that.
