package firmware

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"strings"
)

// ─── ESP32 Flash Pipeline ───
// Prefers standalone esptool binary (Python-free).
// Falls back to python -m esptool / python -m platformio as needed.

// FlashESP32 uploads firmware to an ESP32-S3 via esptool or PlatformIO.
// When standalone esptool is available, uses it directly (no Python needed).
// Falls back to PlatformIO upload target if esptool binary is not found.
func FlashESP32(opts *Options, ctrl Controller) error {
	ctrlPath, err := opts.ControllerPath(ctrl)
	if err != nil {
		return err
	}

	port := opts.Port
	if port == "" {
		port, err = DetectESP32Port()
		if err != nil {
			opts.warn("No ESP32 port detected, will attempt auto-detect")
		} else {
			opts.info("Detected ESP32 port: %s", port)
		}
	}

	// Find the built firmware binary
	fwPath, err := opts.FirmwarePath(ctrl)
	if err != nil {
		return fmt.Errorf("cannot locate firmware binary: %w", err)
	}

	// Try standalone esptool first (Python-free)
	if info := ResolveEsptool(opts); info != nil {
		opts.info("Using standalone esptool (%s)", info.Source)
		return flashWithEsptool(opts, info, port, fwPath)
	}

	// Fall back to PlatformIO upload (requires Python)
	opts.info("Standalone esptool not found, falling back to PlatformIO upload...")
	return flashWithPlatformIO(opts, ctrl, ctrlPath, port)
}

// FlashESP32FromBinary flashes a standalone .bin file using esptool.
// This is the Python-free path used by release-flash and GUI.
func FlashESP32FromBinary(opts *Options, port, fwPath string) error {
	info := ResolveEsptoolOrPython(opts)
	if info == nil {
		return fmt.Errorf("esptool not found — run 'scalefx-flash tools download' or install esptool")
	}

	if port == "" {
		var err error
		port, err = DetectESP32Port()
		if err != nil {
			opts.warn("No ESP32 port detected, esptool will attempt auto-detect")
		} else {
			opts.info("Detected ESP32 port: %s", port)
		}
	}

	if info.Source == "python" {
		return flashWithPythonEsptool(opts, port, fwPath)
	}

	return flashWithEsptool(opts, info, port, fwPath)
}

// ─── Standalone esptool (no Python) ───

// flashImage picks WHAT to write: the combined factory image at 0x0 when the
// build produced one (bootloader + partition table + otadata + app), else the
// bare app at 0x10000.  An app-only write leaves whatever partition table a
// PREVIOUS project flashed — bit hard on rev B bring-up (2026-07-02): the
// i2c-probe test firmware's default table had no `littlefs` partition, so the
// real firmware's config storage came up NOT_INITIALIZED on every boot.  The
// factory image spans only bootloader..app; the LittleFS data region is never
// written, so existing on-device configs survive a reflash unchanged.
func flashImage(opts *Options, fwPath string) (addr, img string) {
	fac := strings.TrimSuffix(fwPath, ".bin") + ".factory.bin"
	if _, err := os.Stat(fac); err == nil {
		opts.info("Flashing factory image (bootloader + partition table + app)")
		return "0x0", fac
	}
	return "0x10000", fwPath
}

func flashWithEsptool(opts *Options, info *EsptoolInfo, port, fwPath string) error {
	args := []string{
		"--chip", "esp32s3",
	}
	if port != "" {
		args = append(args, "--port", port)
	}
	addr, img := flashImage(opts, fwPath)
	args = append(args, "write_flash", addr, img)

	opts.info("Uploading via standalone esptool to ESP32...")
	return runTool(opts, info.Path, args)
}

// ─── Python esptool fallback ───

func flashWithPythonEsptool(opts *Options, port, fwPath string) error {
	args := []string{
		"-m", "esptool",
		"--chip", "esp32s3",
	}
	if port != "" {
		args = append(args, "--port", port)
	}
	addr, img := flashImage(opts, fwPath)
	args = append(args, "write_flash", addr, img)

	opts.info("Uploading via python -m esptool (legacy fallback)...")
	return runPythonCmd(opts, args, "")
}

// ─── PlatformIO fallback (build+flash only) ───

func flashWithPlatformIO(opts *Options, ctrl Controller, ctrlPath, port string) error {
	pioBin := resolvePIO()
	args := pioArgs(pioBin, "run", "-e", ctrl.PIOEnv, "-t", "upload", "-d", ctrlPath)
	if port != "" {
		args = append(args, "--upload-port", port)
	}

	opts.info("Uploading via PlatformIO (%s)...", ctrl.PIOEnv)
	return runCmd(opts, args, ctrlPath)
}

// ─── PlatformIO / Python resolution ───

// resolvePIO returns "pio" if the PlatformIO CLI is on PATH,
// otherwise falls back to "python" (for python -m platformio).
func resolvePIO() string {
	if _, err := exec.LookPath("pio"); err == nil {
		return "pio"
	}
	return "python"
}

// pioArgs builds the argument slice for a PlatformIO command.
// If bin is "pio", args are passed directly; if "python", prepends -m platformio.
func pioArgs(bin string, args ...string) []string {
	if bin == "pio" {
		return append([]string{bin}, args...)
	}
	return append([]string{bin, "-m", "platformio"}, args...)
}

// ─── Command runners ───

// runTool runs a standalone executable and streams output.
func runTool(opts *Options, binary string, args []string) error {
	cmd := exec.Command(binary, args...)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("cannot pipe stdout: %w", err)
	}
	cmd.Stderr = cmd.Stdout

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("cannot start %s: %w", binary, err)
	}

	scanner := bufio.NewScanner(stdout)
	for scanner.Scan() {
		opts.info("%s", scanner.Text())
	}

	if err := cmd.Wait(); err != nil {
		return fmt.Errorf("%s failed: %w", binary, err)
	}

	opts.ok("ESP32 upload complete")
	return nil
}

// runCmd runs a command (given as full args slice) and streams output to opts.
func runCmd(opts *Options, args []string, dir string) error {
	cmd := exec.Command(args[0], args[1:]...)
	cmd.Env = append(os.Environ(), "PYTHONIOENCODING=utf-8")
	if dir != "" {
		cmd.Dir = dir
	}

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("cannot pipe stdout: %w", err)
	}
	cmd.Stderr = cmd.Stdout

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("cannot start command: %w", err)
	}

	scanner := bufio.NewScanner(stdout)
	for scanner.Scan() {
		line := scanner.Text()
		opts.info("%s", line)
	}

	if err := cmd.Wait(); err != nil {
		return fmt.Errorf("command failed: %w", err)
	}

	opts.ok("ESP32 upload complete")
	return nil
}

// runPythonCmd runs a python command and streams output to opts.
func runPythonCmd(opts *Options, args []string, dir string) error {
	return runCmd(opts, append([]string{"python"}, args...), dir)
}
