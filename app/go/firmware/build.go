package firmware

import (
	"bufio"
	"crypto/md5"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// ─── Build Number Management ───

// findVersionSource locates the source file containing BUILD_NUMBER.
// Searches: version.h (src/, include/) then *.ino files in src/.
// Returns the absolute path to the file containing the define.
func findVersionSource(root string, ctrl Controller) (string, error) {
	ctrlDir := filepath.Join(root, "controllers", ctrl.SubDir)
	candidates := []string{
		filepath.Join(ctrlDir, "src", "version.h"),
		filepath.Join(ctrlDir, "include", "version.h"),
	}

	// Add .ino files from src/
	srcDir := filepath.Join(ctrlDir, "src")
	entries, _ := os.ReadDir(srcDir)
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".ino") {
			candidates = append(candidates, filepath.Join(srcDir, e.Name()))
		}
	}

	reBuild := regexp.MustCompile(`(?m)^#define\s+BUILD_NUMBER\s+\d+`)
	for _, path := range candidates {
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		if reBuild.Match(data) {
			return path, nil
		}
	}

	return "", fmt.Errorf("no source file with BUILD_NUMBER found for %s (searched %s)", ctrl.Name, ctrlDir)
}

// IncrementBuildNumber finds the source containing BUILD_NUMBER, bumps it by 1,
// and writes it back. Returns the new build number.
func IncrementBuildNumber(opts *Options, ctrl Controller) (int, error) {
	root, err := opts.resolveWorkspace()
	if err != nil {
		return 0, err
	}

	versionFile, err := findVersionSource(root, ctrl)
	if err != nil {
		return 0, err
	}

	data, err := os.ReadFile(versionFile)
	if err != nil {
		return 0, fmt.Errorf("cannot read %s: %w", filepath.Base(versionFile), err)
	}

	re := regexp.MustCompile(`(?m)^(\s*#define\s+BUILD_NUMBER\s+)(\d+)(.*)$`)
	content := string(data)

	match := re.FindStringSubmatch(content)
	if match == nil {
		return 0, fmt.Errorf("BUILD_NUMBER not found in %s", versionFile)
	}

	oldNum, _ := strconv.Atoi(match[2])
	newNum := oldNum + 1

	newContent := re.ReplaceAllString(content, fmt.Sprintf("${1}%d${3}", newNum))

	if err := os.WriteFile(versionFile, []byte(newContent), 0644); err != nil {
		return 0, fmt.Errorf("cannot write %s: %w", filepath.Base(versionFile), err)
	}

	opts.info("Build number: %d → %d (%s)", oldNum, newNum, filepath.Base(versionFile))
	return newNum, nil
}

// ExtractVersion reads FIRMWARE_VERSION and BUILD_NUMBER from version.h or .ino source.
func ExtractVersion(opts *Options, ctrl Controller) (version string, buildNum int, err error) {
	root, err := opts.resolveWorkspace()
	if err != nil {
		return "", 0, err
	}

	versionFile, err := findVersionSource(root, ctrl)
	if err != nil {
		return "", 0, err
	}

	data, err := os.ReadFile(versionFile)
	if err != nil {
		return "", 0, fmt.Errorf("cannot read %s: %w", filepath.Base(versionFile), err)
	}

	content := string(data)

	// Match both FIRMWARE_VERSION and VERSION (some controllers use one or the other)
	reVersion := regexp.MustCompile(`(?m)^#define\s+(?:FIRMWARE_)?VERSION\s+"([^"]+)"`)
	reBuild := regexp.MustCompile(`(?m)^#define\s+BUILD_NUMBER\s+(\d+)`)

	if m := reVersion.FindStringSubmatch(content); m != nil {
		version = m[1]
	} else {
		return "", 0, fmt.Errorf("VERSION not found in %s", versionFile)
	}

	if m := reBuild.FindStringSubmatch(content); m != nil {
		buildNum, _ = strconv.Atoi(m[1])
	} else {
		return "", 0, fmt.Errorf("BUILD_NUMBER not found in %s", versionFile)
	}

	return version, buildNum, nil
}

// ─── Build ───

// Build runs PlatformIO to compile the firmware.
// It increments the build number first, then runs the build.
func Build(opts *Options, ctrl Controller) (*BuildInfo, error) {
	ctrlPath, err := opts.ControllerPath(ctrl)
	if err != nil {
		return nil, err
	}

	// Verify controller directory exists
	if _, err := os.Stat(ctrlPath); os.IsNotExist(err) {
		return nil, fmt.Errorf("controller directory not found: %s", ctrlPath)
	}

	// Increment build number
	newBuild, err := IncrementBuildNumber(opts, ctrl)
	if err != nil {
		return nil, err
	}

	// Build PlatformIO command
	pioBin := resolvePIO()
	args := pioArgs(pioBin, "run", "-e", ctrl.PIOEnv)
	if !opts.NoClean {
		// Clean first (default behavior — matches Python script)
		opts.info("Running clean build...")
		cleanArgs := pioArgs(pioBin, "run", "-e", ctrl.PIOEnv, "-t", "clean", "-d", ctrlPath)
		cleanCmd := exec.Command(cleanArgs[0], cleanArgs[1:]...)
		cleanCmd.Dir = ctrlPath
		cleanCmd.Env = append(os.Environ(), "PYTHONIOENCODING=utf-8")
		if out, err := cleanCmd.CombinedOutput(); err != nil {
			opts.warn("Clean failed (continuing): %s", strings.TrimSpace(string(out)))
		}
	}

	args = append(args, "-d", ctrlPath)
	opts.info("Building %s (%s)...", ctrl.Name, ctrl.PIOEnv)

	cmd := exec.Command(args[0], args[1:]...)
	cmd.Dir = ctrlPath
	cmd.Env = append(os.Environ(), "PYTHONIOENCODING=utf-8")

	// Stream build output line by line
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("cannot pipe stdout: %w", err)
	}
	cmd.Stderr = cmd.Stdout // merge stderr into stdout

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("cannot start PlatformIO: %w", err)
	}

	var flashUsed, ramUsed string
	scanner := bufio.NewScanner(stdout)
	for scanner.Scan() {
		line := scanner.Text()

		// Parse resource usage from PlatformIO output
		if strings.Contains(line, "Flash:") || strings.Contains(line, "RAM:") {
			// e.g. "RAM:   [==        ]  18.4% (used 48336 bytes from 262144 bytes)"
			// e.g. "Flash: [====      ]  43.2% (used 451276 bytes from 1044480 bytes)"
			if strings.Contains(line, "Flash:") {
				flashUsed = strings.TrimSpace(line)
			}
			if strings.Contains(line, "RAM:") {
				ramUsed = strings.TrimSpace(line)
			}
		}

		opts.info("%s", line)
	}

	if err := cmd.Wait(); err != nil {
		return nil, fmt.Errorf("build failed: %w", err)
	}

	// Get firmware path and info
	fwPath, err := opts.FirmwarePath(ctrl)
	if err != nil {
		return nil, err
	}

	stat, err := os.Stat(fwPath)
	if err != nil {
		return nil, fmt.Errorf("firmware file not found after build: %s", fwPath)
	}

	// MD5 hash
	hash, err := fileMD5(fwPath)
	if err != nil {
		return nil, fmt.Errorf("cannot hash firmware: %w", err)
	}

	// Extract version
	version, _, err := ExtractVersion(opts, ctrl)
	if err != nil {
		opts.warn("Could not extract version: %s", err)
		version = "unknown"
	}

	bi := &BuildInfo{
		FirmwarePath: fwPath,
		FirmwareSize: stat.Size(),
		FirmwareMD5:  hash,
		Version:      version,
		BuildNumber:  newBuild,
		FlashUsed:    flashUsed,
		RAMUsed:      ramUsed,
	}

	opts.ok("Build successful!")
	opts.info("Firmware: %s (%d bytes)", filepath.Base(fwPath), stat.Size())
	if flashUsed != "" {
		opts.info("%s", flashUsed)
	}
	if ramUsed != "" {
		opts.info("%s", ramUsed)
	}

	return bi, nil
}

// ─── Firmware File Verification ───

// VerifyFirmwareFile checks that the firmware binary exists and is valid.
// Used both after build and for --no-build runs.
func VerifyFirmwareFile(opts *Options, ctrl Controller) (*BuildInfo, error) {
	fwPath, err := opts.FirmwarePath(ctrl)
	if err != nil {
		return nil, err
	}

	stat, err := os.Stat(fwPath)
	if err != nil {
		return nil, fmt.Errorf("firmware file not found: %s", fwPath)
	}

	if stat.Size() == 0 {
		return nil, fmt.Errorf("firmware file is empty: %s", fwPath)
	}

	hash, err := fileMD5(fwPath)
	if err != nil {
		return nil, fmt.Errorf("cannot hash firmware: %w", err)
	}

	version, buildNum, err := ExtractVersion(opts, ctrl)
	if err != nil {
		opts.warn("Could not extract version: %s", err)
		version = "unknown"
	}

	return &BuildInfo{
		FirmwarePath: fwPath,
		FirmwareSize: stat.Size(),
		FirmwareMD5:  hash,
		Version:      version,
		BuildNumber:  buildNum,
	}, nil
}

// ─── Helpers ───

func fileMD5(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()

	h := md5.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return fmt.Sprintf("%x", h.Sum(nil)), nil
}
