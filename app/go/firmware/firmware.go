package firmware

// ScaleFX Firmware Package
// Reusable library for building, flashing, and verifying firmware.
// Used by the CLI (scalefx-cli) and the GUI (ScaleFX Studio).

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// ─── Controller Definitions ───

// Platform identifies the MCU platform.
type Platform int

const (
	PlatformPico Platform = iota // RP2040/RP2350, UF2 flash
	PlatformESP32                // ESP32-S3, esptool flash
)

// Controller holds the build/flash configuration for one controller type.
type Controller struct {
	Name    string   // e.g. "gunfx"
	SubDir  string   // relative to controllers/, e.g. "gunfx/pico"
	PIOEnv  string   // PlatformIO environment name
	FwExt   string   // firmware extension: "uf2" or "bin"
	Platform Platform
}

// Controllers is the registry of known controller targets.
var Controllers = map[string]Controller{
	// gunfx standalone Pico controller was removed (2026-06-06 cleanup) — the
	// GunFx effects now live only on the HubFX master
	// (controllers/hubfx/esp32s3/src/effects/).  lightfx + gearcontrol are
	// thin generic-expander boards (ports + roles; the hub drives them).
	"lightfx":     {Name: "lightfx", SubDir: "lightfx/pico", PIOEnv: "pico", FwExt: "uf2", Platform: PlatformPico},
	"gearcontrol": {Name: "gearcontrol", SubDir: "gearcontrol/pico", PIOEnv: "pico", FwExt: "uf2", Platform: PlatformPico},
	"hubfx":       {Name: "hubfx", SubDir: "hubfx/esp32s3", PIOEnv: "esp32s3", FwExt: "bin", Platform: PlatformESP32},
}

// ControllerNames returns sorted controller names.
func ControllerNames() []string {
	return []string{"lightfx", "gearcontrol", "hubfx"}
}

// IsESP32 returns true if the controller uses esptool flashing.
func (c Controller) IsESP32() bool {
	return c.Platform == PlatformESP32
}

// ─── Event/Progress Callback ───

// EventType classifies progress events.
type EventType int

const (
	EventInfo    EventType = iota
	EventOK
	EventWarning
	EventError
	EventStep           // major step transition
	EventProgress       // progress update (0-100)
)

// Event is emitted during build/flash/verify operations.
type Event struct {
	Type     EventType
	Message  string
	Step     int // current step (1-based)
	Total    int // total steps
	Progress int // 0-100 for EventProgress
}

// EventCallback receives build/flash progress events.
type EventCallback func(Event)

// ─── Build Info ───

// BuildInfo holds the result of a successful firmware build.
type BuildInfo struct {
	FirmwarePath string
	FirmwareSize int64
	FirmwareMD5  string
	Version      string
	BuildNumber  int
	FlashUsed    string
	RAMUsed      string
}

// ─── Options ───

// Options controls the build/flash pipeline.
type Options struct {
	Controller string // controller name (required)
	Port       string // serial port (empty = auto-detect)
	NoBuild    bool   // skip build step
	NoClean    bool   // skip clean (incremental build)
	SkipVerify bool   // skip post-flash verification
	Timeout    int    // BOOTSEL wait timeout in seconds (default 15)
	OnEvent    EventCallback

	// WorkspaceRoot is the project root (containing "controllers/" directory).
	// If empty, auto-detected from CWD or executable location.
	WorkspaceRoot string
}

func (o *Options) emit(evt Event) {
	if o.OnEvent != nil {
		o.OnEvent(evt)
	}
}

func (o *Options) info(msg string, args ...any) {
	o.emit(Event{Type: EventInfo, Message: fmt.Sprintf(msg, args...)})
}

func (o *Options) ok(msg string, args ...any) {
	o.emit(Event{Type: EventOK, Message: fmt.Sprintf(msg, args...)})
}

func (o *Options) warn(msg string, args ...any) {
	o.emit(Event{Type: EventWarning, Message: fmt.Sprintf(msg, args...)})
}

func (o *Options) errMsg(msg string, args ...any) {
	o.emit(Event{Type: EventError, Message: fmt.Sprintf(msg, args...)})
}

func (o *Options) step(step, total int, msg string, args ...any) {
	o.emit(Event{Type: EventStep, Step: step, Total: total, Message: fmt.Sprintf(msg, args...)})
}

// ─── Workspace Detection ───

// FindWorkspaceRoot locates the ScaleFX workspace root by walking up from
// the given starting directory until a "controllers" directory is found.
func FindWorkspaceRoot(startDir string) (string, error) {
	dir, err := filepath.Abs(startDir)
	if err != nil {
		return "", err
	}
	for {
		if isWorkspaceRoot(dir) {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return "", fmt.Errorf("workspace root not found (no 'controllers' directory above %s)", startDir)
}

func isWorkspaceRoot(dir string) bool {
	info, err := os.Stat(filepath.Join(dir, "controllers"))
	return err == nil && info.IsDir()
}

// resolveWorkspace returns the workspace root, auto-detecting if not set.
func (o *Options) resolveWorkspace() (string, error) {
	if o.WorkspaceRoot != "" {
		return o.WorkspaceRoot, nil
	}
	cwd, err := os.Getwd()
	if err != nil {
		return "", fmt.Errorf("cannot get working directory: %w", err)
	}
	root, err := FindWorkspaceRoot(cwd)
	if err != nil {
		// Try from executable location
		exe, exeErr := os.Executable()
		if exeErr == nil {
			root2, err2 := FindWorkspaceRoot(filepath.Dir(exe))
			if err2 == nil {
				return root2, nil
			}
		}
		return "", err
	}
	return root, nil
}

// ControllerPath returns the full path to the controller directory.
func (o *Options) ControllerPath(ctrl Controller) (string, error) {
	root, err := o.resolveWorkspace()
	if err != nil {
		return "", err
	}
	return filepath.Join(root, "controllers", ctrl.SubDir), nil
}

// FirmwarePath returns the full path to the built firmware file.
func (o *Options) FirmwarePath(ctrl Controller) (string, error) {
	ctrlPath, err := o.ControllerPath(ctrl)
	if err != nil {
		return "", err
	}
	return filepath.Join(ctrlPath, ".pio", "build", ctrl.PIOEnv, "firmware."+ctrl.FwExt), nil
}

// ─── Top-Level Pipeline ───

// Run executes the full build → flash → verify pipeline.
func Run(opts *Options) error {
	ctrl, ok := Controllers[opts.Controller]
	if !ok {
		return fmt.Errorf("unknown controller: %s (valid: %s)",
			opts.Controller, strings.Join(ControllerNames(), ", "))
	}

	if opts.Timeout == 0 {
		opts.Timeout = 15
	}

	// Plan steps
	steps := planSteps(opts, ctrl)
	total := len(steps)

	var buildInfo *BuildInfo
	stepNum := 0

	for _, s := range steps {
		stepNum++
		switch s {
		case "build":
			opts.step(stepNum, total, "Building firmware...")
			bi, err := Build(opts, ctrl)
			if err != nil {
				return fmt.Errorf("build failed: %w", err)
			}
			buildInfo = bi

		case "verify_fw":
			opts.step(stepNum, total, "Verifying firmware file...")
			bi, err := VerifyFirmwareFile(opts, ctrl)
			if err != nil {
				return fmt.Errorf("firmware verification failed: %w", err)
			}
			if buildInfo == nil {
				buildInfo = bi
			}
			opts.ok("Firmware verified")
			opts.info("Size: %d bytes", bi.FirmwareSize)
			opts.info("MD5:  %s", bi.FirmwareMD5)
			opts.info("Version: %s (Build %d)", bi.Version, bi.BuildNumber)

		case "flash_pico":
			opts.step(stepNum, total, "Flashing via UF2...")
			if err := FlashPico(opts, ctrl); err != nil {
				return fmt.Errorf("flash failed: %w", err)
			}

		case "flash_esp32":
			opts.step(stepNum, total, "Uploading via esptool...")
			if err := FlashESP32(opts, ctrl); err != nil {
				return fmt.Errorf("upload failed: %w", err)
			}

		case "verify_device":
			opts.step(stepNum, total, "Verifying device...")
			if err := VerifyDevice(opts, ctrl); err != nil {
				opts.warn("Verification: %s", err)
			} else {
				opts.ok("Device verified")
			}
		}
	}

	opts.ok("Flash complete!")
	return nil
}

func planSteps(opts *Options, ctrl Controller) []string {
	var steps []string
	if !opts.NoBuild {
		steps = append(steps, "build")
	}
	steps = append(steps, "verify_fw")
	if ctrl.IsESP32() {
		steps = append(steps, "flash_esp32")
	} else {
		steps = append(steps, "flash_pico")
	}
	if !opts.SkipVerify {
		steps = append(steps, "verify_device")
	}
	return steps
}
