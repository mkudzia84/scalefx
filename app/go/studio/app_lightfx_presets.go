package main

// LightFx preset library — Studio-side catalog of program presets.
//
// Programs were previously author-time YAML files living on the device's
// /lightfx/programs/ directory.  Operators had to upload + manage them
// manually.  This file owns the new model (Rule 46 + 2026-05-24 refactor):
//
//   - The library is a Studio-side catalog of named programs (the
//     "preset" concept the operator picks from).
//
//   - The catalog is the OVERLAY of two sources:
//       * FACTORY  — embedded into the .exe at build time from
//                    app/go/studio/assets/presets/lightfx/programs/
//                    (mirror of /media/presets/lightfx/programs/).
//                    Read-only — the operator can't overwrite a
//                    factory preset; Save-As clones to USER instead.
//       * USER     — written to %APPDATA%/ScaleFX/lightfx-presets/ on
//                    Windows (os.UserConfigDir() / ScaleFX / …),
//                    survives Studio updates.
//
//   - Library entries are pure data.  Editing a program in Studio
//     modifies the ACTIVE LIST entry (held in the TS draft); only
//     SavePresetAs persists edits to the library (always as a USER
//     preset, even when the seed was FACTORY).
//
//   - Apply (`SyncLightFxToDevice`) is the only operation that touches
//     the device — uploads each active program YAML to
//     /lightfx/programs/<name>.yaml AND deletes any on-device program
//     not in the active list.

import (
	"bytes"
	"embed"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"scalefx/client"

	"gopkg.in/yaml.v3"
)

// Factory preset library — bundled into the .exe.  Mirror of
// /media/presets/lightfx/programs/; keep in sync via
// `app/go/studio/sync-presets.ps1`.  Embedded as `programs/<name>.yaml`
// — the leading `assets/presets/lightfx/` is stripped by the embed
// directive's `all:` prefix when fs.Sub'ing below.
//
//go:embed assets/presets/lightfx/programs/*.yaml
var factoryPresetsFS embed.FS

const factoryPresetsRoot = "assets/presets/lightfx/programs"

// presetNamePattern restricts USER preset names to ASCII filesystem-
// safe characters so writes to the user dir never escape it via a
// crafted name.  Length cap matches LittleFS's per-path limit comfortably.
var presetNamePattern = regexp.MustCompile(`^[A-Za-z0-9_][A-Za-z0-9_\-]{0,63}$`)

// PresetSource — provenance flag returned with each library entry so
// the UI can render factory presets with a 🔒 badge + disable rename /
// in-place save (Save-As only — see CLAUDE rule, 2026-05-24 refactor).
type PresetSource string

const (
	PresetSourceFactory PresetSource = "factory"
	PresetSourceUser    PresetSource = "user"
)

// PresetLibraryEntry — one program in the catalog returned by
// ListPresetLibrary.  `Name` is the basename without `.yaml`; that's
// also the wire-format program reference (matches what the selector
// ranges use and what becomes `/lightfx/programs/<Name>.yaml` on the
// device at sync time).
type PresetLibraryEntry struct {
	Name     string       `json:"name"`
	Source   PresetSource `json:"source"`
	Category string       `json:"category"` // derived from the name prefix (aircraft, drone, car, …)
	Note     string       `json:"note"`     // first comment line of the YAML, surfaced as a tooltip
	Program  ProgramDTO   `json:"program"`
}

// ─── User-data directory ─────────────────────────────────────────────

// userPresetsDir returns %APPDATA%/ScaleFX/lightfx-presets/ on Windows
// (and the platform equivalent elsewhere via os.UserConfigDir).  Created
// on first call so write operations don't fail with ENOENT — empty dir
// is fine.
func userPresetsDir() (string, error) {
	cfgDir, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("user config dir: %w", err)
	}
	dir := filepath.Join(cfgDir, "ScaleFX", "lightfx-presets")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", fmt.Errorf("create %s: %w", dir, err)
	}
	return dir, nil
}

// ─── Category inference ──────────────────────────────────────────────
//
// We don't store a category field in the YAML to keep the schema simple.
// The library UI groups presets by name prefix (`helicopter_*`,
// `aircraft_*`, `drone_*`, `car_*`, `ship_*`, `effect_*`, `all_*`).
// Unknown prefix falls into "custom".

func categoryForName(name string) string {
	switch {
	case strings.HasPrefix(name, "helicopter_"):
		return "Helicopter"
	case strings.HasPrefix(name, "aircraft_"):
		return "Aircraft"
	case strings.HasPrefix(name, "drone_"):
		return "Drone"
	case strings.HasPrefix(name, "car_"):
		return "Vehicle"
	case strings.HasPrefix(name, "ship_"), strings.HasPrefix(name, "boat_"):
		return "Naval"
	case strings.HasPrefix(name, "effect_"), strings.HasPrefix(name, "fx_"):
		return "Effects"
	case strings.HasPrefix(name, "all_"):
		return "Generic"
	default:
		return "Custom"
	}
}

// extractNote returns the first contiguous comment block at the top of
// the YAML (the "name — description" line + any continuation).  Empty
// string if the file doesn't start with comments.  Surfaced as the
// tooltip on the preset picker.
func extractNote(yamlBytes []byte) string {
	out := []string{}
	for _, line := range bytes.Split(yamlBytes, []byte("\n")) {
		trim := strings.TrimSpace(string(line))
		if strings.HasPrefix(trim, "#") {
			out = append(out, strings.TrimSpace(strings.TrimPrefix(trim, "#")))
			if len(out) >= 4 {
				break
			}
			continue
		}
		if trim == "" && len(out) == 0 {
			continue // skip leading blank lines
		}
		break
	}
	return strings.Join(out, " ")
}

// ─── Per-source iteration ────────────────────────────────────────────

// loadFactoryPresets walks the embedded FS once, parses each YAML, and
// returns the catalog entries.  Errors on individual files are logged
// and skipped — a corrupt factory preset shouldn't make the whole
// library unloadable.
func (a *App) loadFactoryPresets() []PresetLibraryEntry {
	sub, err := fs.Sub(factoryPresetsFS, factoryPresetsRoot)
	if err != nil {
		a.diag.Info("LIGHTFX", "factory preset fs.Sub failed: %v", err)
		return nil
	}
	var out []PresetLibraryEntry
	_ = fs.WalkDir(sub, ".", func(path string, d fs.DirEntry, walkErr error) error {
		if walkErr != nil || d.IsDir() || !strings.HasSuffix(path, ".yaml") {
			return nil
		}
		data, err := fs.ReadFile(sub, path)
		if err != nil {
			a.diag.Info("LIGHTFX", "factory preset read %s: %v", path, err)
			return nil
		}
		name := strings.TrimSuffix(filepath.Base(path), ".yaml")
		prog := ProgramDTO{}
		if err := yaml.Unmarshal(data, &prog); err != nil {
			a.diag.Info("LIGHTFX", "factory preset parse %s: %v", path, err)
			return nil
		}
		// Migrate v1 channels[] → v2 tracks[] on load (idempotent for
		// already-v2 presets).  Studio's editor only sees the v2 shape.
		prog = normalizeProgram(prog)
		out = append(out, PresetLibraryEntry{
			Name:     name,
			Source:   PresetSourceFactory,
			Category: categoryForName(name),
			Note:     extractNote(data),
			Program:  prog,
		})
		return nil
	})
	return out
}

// loadUserPresets walks the user data dir.  Missing dir → empty result
// (not an error — fresh install).
func (a *App) loadUserPresets() []PresetLibraryEntry {
	dir, err := userPresetsDir()
	if err != nil {
		a.diag.Info("LIGHTFX", "user presets dir: %v", err)
		return nil
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []PresetLibraryEntry
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".yaml") {
			continue
		}
		full := filepath.Join(dir, e.Name())
		data, err := os.ReadFile(full)
		if err != nil {
			a.diag.Info("LIGHTFX", "user preset read %s: %v", full, err)
			continue
		}
		name := strings.TrimSuffix(e.Name(), ".yaml")
		prog := ProgramDTO{}
		if err := yaml.Unmarshal(data, &prog); err != nil {
			a.diag.Info("LIGHTFX", "user preset parse %s: %v", full, err)
			continue
		}
		// Same v1 → v2 migration as factory presets.  A user preset
		// saved by older Studio (with v1 channels[]) loads cleanly.
		prog = normalizeProgram(prog)
		out = append(out, PresetLibraryEntry{
			Name:     name,
			Source:   PresetSourceUser,
			Category: categoryForName(name),
			Note:     extractNote(data),
			Program:  prog,
		})
	}
	return out
}

// ─── Wails API ───────────────────────────────────────────────────────

// ListPresetLibrary returns the merged catalog (factory + user).  A
// USER preset with the same name as a FACTORY one shadows the factory
// entry — this is the override mechanism for operators who want to
// edit a factory preset's defaults without losing them on Studio
// update (Studio rebuilds bump the embedded factory copies; the user
// override stays put in %APPDATA%).
//
// Result is sorted by category, then by name.
func (a *App) ListPresetLibrary() ([]PresetLibraryEntry, error) {
	defer a.diag.Around("ListPresetLibrary", nil)()

	all := append([]PresetLibraryEntry{}, a.loadFactoryPresets()...)
	user := a.loadUserPresets()

	// Apply user-overrides-factory shadowing.
	byName := make(map[string]int, len(all))
	for i, e := range all {
		byName[e.Name] = i
	}
	for _, ue := range user {
		if i, ok := byName[ue.Name]; ok {
			all[i] = ue
		} else {
			all = append(all, ue)
			byName[ue.Name] = len(all) - 1
		}
	}

	sort.SliceStable(all, func(i, j int) bool {
		if all[i].Category != all[j].Category {
			return all[i].Category < all[j].Category
		}
		return all[i].Name < all[j].Name
	})

	a.diag.Info("LIGHTFX", "library: %d preset(s) (factory + user merged)", len(all))
	return all, nil
}

// SavePresetAs writes a program to the USER templates directory under
// `<name>.yaml`.  `name` MUST match presetNamePattern — Studio's
// Save-As dialog enforces this, but we re-validate server-side as
// defense against a hand-crafted Wails call.
//
// REJECTS collisions with FACTORY template names (Rule: no shadowing
// — every template name is globally unique across factory + user).
// User-vs-user collisions are allowed; Studio's dialog confirms the
// overwrite before calling.
func (a *App) SavePresetAs(name string, prog ProgramDTO) (PresetLibraryEntry, error) {
	defer a.diag.Around("SavePresetAs", map[string]any{"name": name})()

	if !presetNamePattern.MatchString(name) {
		return PresetLibraryEntry{}, fmt.Errorf("invalid template name %q (allowed: letters, digits, _ and -, 1-64 chars, must start with letter/digit/_)", name)
	}

	// Factory-collision check — refuses overshadowing.  Operator must
	// pick a different name (e.g. `helicopter_flight_v2`).
	factoryPath := factoryPresetsRoot + "/" + name + ".yaml"
	if _, err := factoryPresetsFS.ReadFile(factoryPath); err == nil {
		return PresetLibraryEntry{}, fmt.Errorf("name %q is already used by a built-in template — pick a different name (e.g. %q_v2)", name, name)
	}

	dir, err := userPresetsDir()
	if err != nil {
		return PresetLibraryEntry{}, err
	}

	// Canonical v2 emit — clears any legacy Channels[] if the program
	// came from a v1 load that wasn't normalized at read time.
	prog = normalizeProgram(prog)
	data, err := yaml.Marshal(&prog)
	if err != nil {
		return PresetLibraryEntry{}, fmt.Errorf("serialise: %w", err)
	}

	target := filepath.Join(dir, name+".yaml")
	if err := os.WriteFile(target, data, 0o644); err != nil {
		return PresetLibraryEntry{}, fmt.Errorf("write %s: %w", target, err)
	}

	a.diag.Info("LIGHTFX", "saved user template %s (%d tracks)", target, len(prog.Tracks))
	return PresetLibraryEntry{
		Name:     name,
		Source:   PresetSourceUser,
		Category: categoryForName(name),
		Note:     extractNote(data),
		Program:  prog,
	}, nil
}

// ListLightFxOrphans returns the basenames (no path, no .yaml) of
// programs currently on the device's /lightfx/programs/ that are NOT
// in `activeNames`.  Studio calls this BEFORE SyncLightFxToDevice to
// surface a "will delete N files" confirmation if non-empty — destruction
// shouldn't be silent.
//
// Returns empty slice on first-sync (no /lightfx/programs/ dir yet),
// disconnected, or any list error — i.e. always safe to ignore the
// result on the "nothing to warn about" side.
func (a *App) ListLightFxOrphans(activeNames []string) ([]string, error) {
	defer a.diag.Around("ListLightFxOrphans", map[string]any{"active": len(activeNames)})()
	c := a.snapshotClient()
	if c == nil {
		return []string{}, nil
	}
	blob, err := c.Storage.FileList("/lightfx/programs", client.TargetFlash)
	if err != nil {
		return []string{}, nil // no dir yet → nothing to delete
	}
	keep := map[string]struct{}{}
	for _, n := range activeNames {
		keep[n] = struct{}{}
	}
	out := []string{}
	for _, line := range strings.Split(blob, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasSuffix(line, "/") {
			continue
		}
		if i := strings.IndexAny(line, " \t"); i >= 0 {
			line = line[:i]
		}
		if !strings.HasSuffix(line, ".yaml") {
			continue
		}
		name := strings.TrimSuffix(line, ".yaml")
		if _, ok := keep[name]; !ok {
			out = append(out, name)
		}
	}
	sort.Strings(out)
	return out, nil
}

// DeletePreset removes a USER preset.  Factory presets are immutable —
// attempting to delete one returns an error explaining why.  If the
// name shadowed a factory preset, the factory version becomes
// visible again on the next ListPresetLibrary.
func (a *App) DeletePreset(name string) error {
	defer a.diag.Around("DeletePreset", map[string]any{"name": name})()

	if !presetNamePattern.MatchString(name) {
		return fmt.Errorf("invalid preset name %q", name)
	}
	dir, err := userPresetsDir()
	if err != nil {
		return err
	}
	target := filepath.Join(dir, name+".yaml")
	if err := os.Remove(target); err != nil {
		if os.IsNotExist(err) {
			// User dir doesn't have it — check the factory FS.
			factoryPath := factoryPresetsRoot + "/" + name + ".yaml"
			if _, err := factoryPresetsFS.ReadFile(factoryPath); err == nil {
				return fmt.Errorf("preset %q is a factory preset and cannot be deleted (Save-As to a new name to override it instead)", name)
			}
			return fmt.Errorf("preset %q not found in user library", name)
		}
		return fmt.Errorf("delete %s: %w", target, err)
	}
	a.diag.Info("LIGHTFX", "deleted user preset %s", target)
	return nil
}

// ─── Device sync (Apply) ─────────────────────────────────────────────

// ActiveProgramDTO is one entry in the operator's active list — the
// Studio TS layer assembles it from a library entry's current draft
// state (after inline edits).  `Name` becomes
// /lightfx/programs/<Name>.yaml on the device.
type ActiveProgramDTO struct {
	Name    string     `json:"name"`
	Program ProgramDTO `json:"program"`
}

// SyncLightFxToDevice is the new Apply path for LightFx.
//
// Steps (in order):
//   1. List the existing programs on the device (/lightfx/programs/*.yaml).
//   2. Upload each active program's YAML to /lightfx/programs/<name>.yaml,
//      overwriting whatever's there.
//   3. Delete any on-device program file whose basename isn't in the
//      active list — keeps the device clean of stale programs.
//   4. Build the LightFxConfigDTO with `Programs: ["/lightfx/programs/<n>.yaml", …]`
//      and call SetLightFxConfig (which uploads /lightfx.yaml + reloads).
//
// The orphan-delete step is what the operator means by "Apply does a
// sync — unused programs are removed".  Operators no longer manage
// /lightfx/programs/ by hand; Studio owns the directory contents on
// every Apply.
func (a *App) SyncLightFxToDevice(cfg LightFxConfigDTO, active []ActiveProgramDTO) error {
	defer a.diag.Around("SyncLightFxToDevice",
		map[string]any{"programs": len(active), "selector": cfg.ProgramSelector.Enabled})()

	c := a.snapshotClient()
	if c == nil {
		return fmt.Errorf("not connected")
	}

	// 1. List the on-device /lightfx/programs/ directory so step 3 knows
	//    what to delete.  Missing dir is fine — first-time sync.
	existing := map[string]struct{}{}
	if blob, err := c.Storage.FileList("/lightfx/programs", client.TargetFlash); err == nil {
		for _, line := range strings.Split(blob, "\n") {
			line = strings.TrimSpace(line)
			if line == "" || strings.HasSuffix(line, "/") {
				continue
			}
			// "name.yaml   size" or just "name.yaml" — strip everything
			// after the first run of whitespace.
			if i := strings.IndexAny(line, " \t"); i >= 0 {
				line = line[:i]
			}
			if strings.HasSuffix(line, ".yaml") {
				existing[strings.TrimSuffix(line, ".yaml")] = struct{}{}
			}
		}
	}

	// 2. Upload each active program.  Build the path list as we go so
	//    /lightfx.yaml's `programs:` block stays in sync with what we
	//    actually pushed.
	activeNames := map[string]struct{}{}
	progPaths := make([]string, 0, len(active))
	for _, p := range active {
		if !presetNamePattern.MatchString(p.Name) {
			return fmt.Errorf("invalid active program name %q", p.Name)
		}
		activeNames[p.Name] = struct{}{}

		// Force canonical v2 emit per program.  Sync NEVER writes
		// legacy channels[] to the device — every uploaded YAML is
		// schema_version: 2 with tracks[].  The firmware accepts both
		// shapes on load (legacy on-device YAMLs that predate this
		// refactor still parse) — Studio just stops PRODUCING v1.
		p.Program = normalizeProgram(p.Program)
		data, err := yaml.Marshal(&p.Program)
		if err != nil {
			return fmt.Errorf("serialise %s: %w", p.Name, err)
		}
		tmp, err := os.CreateTemp("", "lightfx-prog-*.yaml")
		if err != nil {
			return err
		}
		tmpPath := tmp.Name()
		if _, err := tmp.Write(data); err != nil {
			tmp.Close()
			os.Remove(tmpPath)
			return err
		}
		tmp.Close()

		remote := "/lightfx/programs/" + p.Name + ".yaml"
		if _, err := c.Storage.FileUpload(tmpPath, client.UploadOptions{
			Path: remote, Target: client.TargetFlash, Mode: client.UploadSync,
		}); err != nil {
			os.Remove(tmpPath)
			return fmt.Errorf("upload %s: %w", remote, err)
		}
		os.Remove(tmpPath)
		progPaths = append(progPaths, remote)
	}

	// 3. Orphan delete — on-device files not in activeNames.
	for name := range existing {
		if _, keep := activeNames[name]; keep {
			continue
		}
		remote := "/lightfx/programs/" + name + ".yaml"
		// Best-effort — a delete failure on one orphan shouldn't abort
		// the whole sync (the operator can re-Apply or clean up via
		// the file manager).
		if err := c.Storage.FileDelete(remote, client.TargetFlash, 0); err != nil {
			a.diag.Info("LIGHTFX", "orphan delete %s failed: %v", remote, err)
		} else {
			a.diag.Info("LIGHTFX", "deleted orphan program %s", remote)
		}
	}

	// 4. Push /lightfx.yaml with the canonical programs[] path list.
	cfg.Programs = progPaths
	if err := a.SetLightFxConfig(cfg); err != nil {
		return fmt.Errorf("save lightfx.yaml: %w", err)
	}

	a.diag.Info("LIGHTFX", "sync ok — %d active program(s), %d orphan(s) cleaned",
		len(active), len(existing)-len(activeNames))
	return nil
}

// ─── Unused import guard ─────────────────────────────────────────────
// `time` is referenced via SetLightFxConfig's downstream FileUpload
// path; keep an explicit reference here so a future refactor that
// drops the call doesn't leave the import as dead.
var _ = time.Second
