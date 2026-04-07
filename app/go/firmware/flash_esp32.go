package firmware

import (
	"bufio"
	"fmt"
	"os/exec"
	"strings"
)

// ─── ESP32 Flash Pipeline ───
// Uses PlatformIO's built-in esptool integration for uploading.

// FlashESP32 uploads firmware to an ESP32-S3 via PlatformIO upload target.
func FlashESP32(opts *Options, ctrl Controller) error {
	ctrlPath, err := opts.ControllerPath(ctrl)
	if err != nil {
		return err
	}

	port := opts.Port
	if port == "" {
		port, err = DetectESP32Port()
		if err != nil {
			// No auto-detected port — PlatformIO will try to find one itself
			opts.warn("No ESP32 port detected, PlatformIO will attempt auto-detect")
		} else {
			opts.info("Detected ESP32 port: %s", port)
		}
	}

	// Build PlatformIO upload command
	args := []string{"-m", "platformio", "run", "-e", ctrl.PIOEnv, "-t", "upload", "-d", ctrlPath}
	if port != "" {
		args = append(args, "--upload-port", port)
	}

	opts.info("Uploading to ESP32 (%s)...", ctrl.PIOEnv)

	return runPythonCmd(opts, args, ctrlPath)
}

// runEsptool runs esptool.py via python -m esptool with the given arguments.
func runEsptool(opts *Options, args []string) error {
	return runPythonCmd(opts, args, "")
}

// runPythonCmd runs a python command and streams output to opts.
func runPythonCmd(opts *Options, args []string, dir string) error {
	cmd := exec.Command("python", args...)
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
		if strings.Contains(line, "Writing at") && strings.Contains(line, "%") {
			opts.info("%s", line)
		} else if strings.Contains(line, "Leaving...") || strings.Contains(line, "Hard resetting") {
			opts.info("%s", line)
		} else {
			opts.info("%s", line)
		}
	}

	if err := cmd.Wait(); err != nil {
		return fmt.Errorf("command failed: %w", err)
	}

	opts.ok("ESP32 upload complete")
	return nil
}
