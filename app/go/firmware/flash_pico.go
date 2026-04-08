package firmware

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"go.bug.st/serial"
)

// ─── Pico Flash Pipeline ───
// 1. Enter BOOTSEL mode via 1200-baud DTR toggle
// 2. Wait for BOOTSEL USB mass storage drive to appear
// 3. Copy UF2 firmware file to the drive

// FlashPico executes the full Pico flash sequence: BOOTSEL entry → drive wait → UF2 copy.
func FlashPico(opts *Options, ctrl Controller) error {
	fwPath, err := opts.FirmwarePath(ctrl)
	if err != nil {
		return err
	}

	if _, err := os.Stat(fwPath); os.IsNotExist(err) {
		return fmt.Errorf("firmware file not found: %s", fwPath)
	}

	// Use provided port or auto-detect
	port := opts.Port
	if port == "" {
		port, err = DetectPicoPort()
		if err != nil {
			return fmt.Errorf("cannot find Pico serial port: %w", err)
		}
		opts.info("Detected Pico port: %s", port)
	}

	// Step 1: Enter BOOTSEL via 1200-baud DTR toggle
	// The board may reboot instantly on the 1200-baud open, causing the
	// serial library to report an error. That's OK — check for the drive.
	opts.info("Entering BOOTSEL mode via %s...", port)
	if err := enterBootsel(port); err != nil {
		opts.warn("BOOTSEL serial trigger: %v (checking for drive anyway)", err)
	}

	// Step 2: Wait for BOOTSEL drive
	opts.info("Waiting for BOOTSEL drive...")
	drive, err := waitForBootselDrive(opts.Timeout)
	if err != nil {
		return fmt.Errorf("BOOTSEL drive not found: %w", err)
	}
	opts.info("Found BOOTSEL drive: %s", drive)

	// Step 3: Copy firmware
	opts.info("Copying firmware to %s...", drive)
	if err := copyFirmware(fwPath, drive); err != nil {
		return fmt.Errorf("firmware copy failed: %w", err)
	}

	opts.ok("UF2 copied to %s", drive)

	// Wait for device to reboot
	opts.info("Waiting for device to reboot...")
	time.Sleep(3 * time.Second)

	return nil
}

// ─── BOOTSEL Entry ───

// enterBootsel opens the serial port at 1200 baud, toggles DTR, and closes.
// This triggers the Pico's built-in USB bootloader.
//
// The Pico may reboot so fast that the serial library reports an error even
// though BOOTSEL was successfully triggered. Callers should treat errors as
// non-fatal and check for the BOOTSEL drive regardless.
func enterBootsel(port string) error {
	mode := &serial.Mode{
		BaudRate: 1200,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}

	p, err := serial.Open(port, mode)
	if err != nil {
		// The board may have already rebooted into BOOTSEL from the
		// connection attempt alone — return the error so callers can
		// decide whether to proceed.
		return fmt.Errorf("cannot open %s at 1200 baud: %w", port, err)
	}

	// Toggle DTR to trigger BOOTSEL
	_ = p.SetDTR(true)
	time.Sleep(100 * time.Millisecond)
	_ = p.SetDTR(false)
	time.Sleep(100 * time.Millisecond)

	p.Close()
	time.Sleep(500 * time.Millisecond)

	return nil
}

// ─── BOOTSEL Drive Detection ───

// findBootselDrive scans mounted drives for a Pico BOOTSEL mass storage device.
// Returns the drive path (e.g. "E:\\") or empty string if not found.
func findBootselDrive() string {
	if runtime.GOOS != "windows" {
		// TODO: Linux/macOS support (check /media or /Volumes)
		return ""
	}

	for letter := 'D'; letter <= 'Z'; letter++ {
		drive := fmt.Sprintf("%c:\\", letter)
		infoPath := filepath.Join(drive, "INFO_UF2.TXT")

		data, err := os.ReadFile(infoPath)
		if err != nil {
			continue
		}

		content := string(data)
		if strings.Contains(content, "RPI-RP2") ||
			strings.Contains(content, "RP2040") ||
			strings.Contains(content, "RP2350") {
			return drive
		}
	}

	return ""
}

// waitForBootselDrive polls for a BOOTSEL drive up to timeoutSec seconds.
func waitForBootselDrive(timeoutSec int) (string, error) {
	deadline := time.Now().Add(time.Duration(timeoutSec) * time.Second)

	for time.Now().Before(deadline) {
		drive := findBootselDrive()
		if drive != "" {
			return drive, nil
		}
		time.Sleep(500 * time.Millisecond)
	}

	return "", fmt.Errorf("timed out after %ds waiting for BOOTSEL drive", timeoutSec)
}

// ─── UF2 Copy ───

// copyFirmware copies the firmware file to the BOOTSEL drive.
func copyFirmware(fwPath, drive string) error {
	src, err := os.Open(fwPath)
	if err != nil {
		return fmt.Errorf("cannot open firmware: %w", err)
	}
	defer src.Close()

	dstPath := filepath.Join(drive, "firmware.uf2")
	dst, err := os.Create(dstPath)
	if err != nil {
		return fmt.Errorf("cannot create %s: %w", dstPath, err)
	}
	defer dst.Close()

	n, err := io.Copy(dst, src)
	if err != nil {
		return fmt.Errorf("copy failed: %w", err)
	}

	// Sync to flush writes
	if err := dst.Sync(); err != nil {
		// Sync may fail as drive disconnects during flash — that's expected
		return nil
	}

	_ = n // bytes copied
	return nil
}
