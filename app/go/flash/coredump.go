package main

// `scalefx-flash coredump <controller>` — pull the ESP32 coredump from flash
// and decode it to a backtrace.  When the firmware panics, ESP-IDF saves an
// ELF coredump to a dedicated flash partition (CONFIG_ESP_COREDUMP_ENABLE_TO_
// FLASH).  This reads that partition with esptool and decodes it against the
// built firmware ELF with espcoredump + xtensa gdb, printing the crash
// backtrace + register dump — the fastest way to locate a firmware crash.
//
// Requires PlatformIO's toolchain (python + espcoredump + xtensa gdb), which
// is present after any firmware build.  If the decode tools aren't found the
// raw coredump is saved and the manual decode command is printed.

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"

	fw "scalefx/firmware"
)

// HubFX ESP32-S3 coredump partition (controllers/hubfx/esp32s3/partitions.csv:
// `coredump, data, coredump, 0x7F0000, 0x10000`).  64 KB.
const (
	coredumpOffset = "0x7F0000"
	coredumpSize   = "0x10000"
)

func cmdCoredump(controller, port, saveTo string, rawOnly bool) {
	ctrl, ok := resolveCtrl(controller)
	if !ok {
		return
	}
	if ctrl.Name != "hubfx" {
		printError("coredump is HubFX-only (ESP32 coredump partition); got %q", ctrl.Name)
		return
	}
	opts := makeOpts(ctrl.Name)
	opts.Port = port

	if port == "" {
		p, err := fw.DetectESP32Port()
		if err != nil || p == "" {
			printError("no ESP32 port detected (pass --port PORT): %v", err)
			return
		}
		port = p
	}

	// Read the coredump partition.
	binPath := saveTo
	if binPath == "" {
		f, err := os.CreateTemp("", "scalefx-coredump-*.bin")
		if err != nil {
			printError("temp file: %v", err)
			return
		}
		binPath = f.Name()
		f.Close()
		defer os.Remove(binPath)
	}
	printInfo("Reading coredump partition (%s, %s) from %s ...", coredumpOffset, coredumpSize, port)
	if err := esptoolReadFlash(opts, port, coredumpOffset, coredumpSize, binPath); err != nil {
		printError("read coredump: %v", err)
		return
	}

	if looksErased(binPath) {
		printOK("No coredump stored — the board has not panicked since the partition was last erased.")
		return
	}
	if saveTo != "" {
		printOK("Coredump saved to %s", saveTo)
	}
	if rawOnly {
		printInfo("Raw coredump at %s — decode with:", binPath)
		printDecodeHint(opts, ctrl, binPath)
		return
	}

	// Decode against the firmware ELF.
	if !decodeCoredump(opts, ctrl, binPath) {
		// decodeCoredump prints why; leave the raw file for manual decode.
		if saveTo == "" {
			kept := "scalefx-coredump.bin"
			if data, err := os.ReadFile(binPath); err == nil {
				_ = os.WriteFile(kept, data, 0o644)
				printInfo("Raw coredump kept at %s", kept)
			}
		}
		printDecodeHint(opts, ctrl, binPath)
	}
}

// esptoolReadFlash reads `size` bytes at `off` into `out`.  Prefers the
// standalone esptool binary; falls back to PlatformIO's python + esptool.py
// (always present after a build, and the same toolchain espcoredump needs).
func esptoolReadFlash(opts *fw.Options, port, off, size, out string) error {
	args := []string{
		"--chip", "esp32s3", "--port", port, "--baud", "921600",
		"--before", "default_reset", "--after", "hard_reset",
		"read_flash", off, size, out,
	}
	env := append(os.Environ(), "PYTHONIOENCODING=utf-8", "PYTHONUTF8=1")

	var cmd *exec.Cmd
	if info := fw.ResolveEsptool(opts); info != nil { // standalone binary
		cmd = exec.Command(info.Path, args...)
	} else if py := platformioPython(); py != "" {
		if esp := findPlatformioFile("packages/tool-esptoolpy*/esptool.py"); esp != "" {
			cmd = exec.Command(py, append([]string{esp}, args...)...)
		} else {
			cmd = exec.Command(py, append([]string{"-m", "esptool"}, args...)...)
		}
	} else {
		return fmt.Errorf("no esptool found (standalone, or PlatformIO python+esptool) — run 'scalefx-flash tools download'")
	}
	cmd.Env = env
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

// looksErased returns true if the coredump partition is all-0xFF (never written)
// or all-zero — i.e. there's no stored coredump.
func looksErased(path string) bool {
	data, err := os.ReadFile(path)
	if err != nil || len(data) == 0 {
		return true
	}
	// ESP-IDF writes a small header first; a stored coredump has a valid magic
	// in the first words.  An erased partition is 0xFF; sample the head.
	head := data
	if len(head) > 64 {
		head = head[:64]
	}
	allFF := bytes.IndexFunc(head, func(r rune) bool { return r != 0xFF }) < 0
	allZero := bytes.IndexFunc(head, func(r rune) bool { return r != 0x00 }) < 0
	return allFF || allZero
}

// ─── Decode ───

func decodeCoredump(opts *fw.Options, ctrl fw.Controller, binPath string) bool {
	py := platformioPython()
	ecd := findPlatformioFile("packages/framework-espidf*/components/espcoredump/espcoredump.py")
	gdb := findPlatformioFile("packages/tool-xtensa-esp-elf-gdb*/bin/xtensa-esp32s3-elf-gdb" + exeSuffix())
	elf := firmwareElf(opts, ctrl)

	if py == "" || ecd == "" || gdb == "" || elf == "" {
		printWarning("decode tools not all found (python=%v espcoredump=%v gdb=%v elf=%v)",
			py != "", ecd != "", gdb != "", elf != "")
		return false
	}

	printInfo("Decoding against %s ...", filepath.Base(elf))
	printWarning("(the ELF must match the FLASHED build — rebuild since the crash invalidates the backtrace)")
	cmd := exec.Command(py, ecd, "--chip", "esp32s3", "info_corefile",
		"--gdb", gdb, "-c", binPath, "-t", "raw", elf)
	cmd.Env = append(os.Environ(),
		"PYTHONIOENCODING=utf-8", "PYTHONUTF8=1",
		"IDF_PATH="+filepath.Dir(filepath.Dir(filepath.Dir(ecd)))) // …/framework-espidf
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		printWarning("espcoredump exited: %v", err)
		return false
	}
	return true
}

func printDecodeHint(opts *fw.Options, ctrl fw.Controller, binPath string) {
	elf := firmwareElf(opts, ctrl)
	fmt.Printf("\n  espcoredump.py --chip esp32s3 info_corefile -c %s -t raw \\\n      %s\n", binPath, elf)
}

// ─── Tool resolution (PlatformIO) ───

func platformioHome() string {
	if d := os.Getenv("PLATFORMIO_CORE_DIR"); d != "" {
		return d
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return filepath.Join(home, ".platformio")
}

func platformioPython() string {
	base := platformioHome()
	if base == "" {
		return ""
	}
	cands := []string{
		filepath.Join(base, "penv", "Scripts", "python.exe"),
		filepath.Join(base, "penv", "bin", "python"),
	}
	for _, c := range cands {
		if fileExistsLocal(c) {
			return c
		}
	}
	if p, err := exec.LookPath("python"); err == nil {
		return p
	}
	return ""
}

// findPlatformioFile resolves the first match of a glob under ~/.platformio.
func findPlatformioFile(glob string) string {
	base := platformioHome()
	if base == "" {
		return ""
	}
	matches, _ := filepath.Glob(filepath.Join(base, filepath.FromSlash(glob)))
	for _, m := range matches {
		if fileExistsLocal(m) {
			return m
		}
	}
	return ""
}

func firmwareElf(opts *fw.Options, ctrl fw.Controller) string {
	ctrlPath, err := opts.ControllerPath(ctrl)
	if err != nil {
		return ""
	}
	elf := filepath.Join(ctrlPath, ".pio", "build", ctrl.PIOEnv, "firmware.elf")
	if fileExistsLocal(elf) {
		return elf
	}
	return ""
}

func exeSuffix() string {
	if runtime.GOOS == "windows" {
		return ".exe"
	}
	return ""
}

func fileExistsLocal(p string) bool {
	info, err := os.Stat(p)
	return err == nil && !info.IsDir()
}
