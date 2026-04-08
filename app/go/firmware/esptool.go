package firmware

// Standalone esptool binary resolution and auto-download.
//
// Search order for esptool.exe:
//   1. tools/esptool/esptool.exe  (workspace-relative, development)
//   2. Next to this executable     (co-located, distribution)
//   3. On system PATH              (system-installed)
//   4. python -m esptool           (legacy fallback)
//
// Auto-download fetches the latest release from GitHub:
//   https://github.com/espressif/esptool/releases

import (
	"archive/zip"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

const (
	esptoolVersion = "5.2.0"
	esptoolGitHub  = "https://github.com/espressif/esptool/releases/download"
)

// EsptoolInfo describes a resolved esptool binary.
type EsptoolInfo struct {
	Path   string // absolute path to the binary
	Source string // "workspace", "colocated", "path", or "python"
}

// ResolveEsptool finds the esptool binary, searching known locations.
// Returns nil if not found anywhere (caller should offer to download).
func ResolveEsptool(opts *Options) *EsptoolInfo {
	// 1. Workspace-relative: tools/esptool/esptool.exe
	if root, err := opts.resolveWorkspace(); err == nil {
		candidate := filepath.Join(root, "tools", "esptool", esptoolBinary())
		if fileExists(candidate) {
			return &EsptoolInfo{Path: candidate, Source: "workspace"}
		}
	}

	// 2. Co-located with this executable
	if exe, err := os.Executable(); err == nil {
		candidate := filepath.Join(filepath.Dir(exe), esptoolBinary())
		if fileExists(candidate) {
			return &EsptoolInfo{Path: candidate, Source: "colocated"}
		}
	}

	// 3. System PATH
	if path, err := exec.LookPath(esptoolBinary()); err == nil {
		return &EsptoolInfo{Path: path, Source: "path"}
	}

	return nil
}

// ResolveEsptoolOrPython finds esptool binary, falling back to python -m esptool.
// The Python fallback is returned only if Python + esptool package are available.
func ResolveEsptoolOrPython(opts *Options) *EsptoolInfo {
	if info := ResolveEsptool(opts); info != nil {
		return info
	}

	// 4. Python fallback: python -m esptool --version
	if path, err := exec.LookPath("python"); err == nil {
		cmd := exec.Command(path, "-m", "esptool", "version")
		if cmd.Run() == nil {
			return &EsptoolInfo{Path: path, Source: "python"}
		}
	}

	return nil
}

// DownloadEsptool downloads the standalone esptool binary for the current platform
// into <workspace>/tools/esptool/. Returns the path to the extracted binary.
func DownloadEsptool(opts *Options) (string, error) {
	root, err := opts.resolveWorkspace()
	if err != nil {
		return "", fmt.Errorf("cannot find workspace root: %w", err)
	}

	destDir := filepath.Join(root, "tools", "esptool")
	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return "", fmt.Errorf("cannot create tools/esptool directory: %w", err)
	}

	platform, ext := esptoolPlatform()
	assetName := fmt.Sprintf("esptool-v%s-%s.%s", esptoolVersion, platform, ext)
	url := fmt.Sprintf("%s/v%s/%s", esptoolGitHub, esptoolVersion, assetName)

	opts.info("Downloading esptool v%s for %s...", esptoolVersion, platform)
	opts.info("URL: %s", url)

	// Download to temp file
	tmpFile, err := os.CreateTemp("", "esptool-*."+ext)
	if err != nil {
		return "", fmt.Errorf("cannot create temp file: %w", err)
	}
	tmpPath := tmpFile.Name()
	defer os.Remove(tmpPath)

	resp, err := http.Get(url)
	if err != nil {
		tmpFile.Close()
		return "", fmt.Errorf("download failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		tmpFile.Close()
		return "", fmt.Errorf("download failed: HTTP %d", resp.StatusCode)
	}

	size, err := io.Copy(tmpFile, resp.Body)
	tmpFile.Close()
	if err != nil {
		return "", fmt.Errorf("download failed: %w", err)
	}
	opts.info("Downloaded %.1f MB", float64(size)/1024/1024)

	// Extract
	opts.info("Extracting...")
	binaryPath := filepath.Join(destDir, esptoolBinary())

	if ext == "zip" {
		if err := extractZipEsptool(tmpPath, destDir); err != nil {
			return "", fmt.Errorf("extraction failed: %w", err)
		}
	} else {
		return "", fmt.Errorf("tar.gz extraction not implemented (Windows-only for now)")
	}

	if !fileExists(binaryPath) {
		return "", fmt.Errorf("esptool binary not found after extraction at %s", binaryPath)
	}

	opts.ok("esptool v%s installed to %s", esptoolVersion, destDir)
	return binaryPath, nil
}

// EsptoolInstalled returns true if esptool is available (any source).
func EsptoolInstalled(opts *Options) bool {
	return ResolveEsptoolOrPython(opts) != nil
}

// ─── Internals ───

func esptoolBinary() string {
	if runtime.GOOS == "windows" {
		return "esptool.exe"
	}
	return "esptool"
}

func esptoolPlatform() (platform, ext string) {
	switch runtime.GOOS {
	case "windows":
		return "windows-amd64", "zip"
	case "darwin":
		if runtime.GOARCH == "arm64" {
			return "macos-arm64", "tar.gz"
		}
		return "macos-amd64", "tar.gz"
	default:
		if runtime.GOARCH == "arm64" {
			return "linux-aarch64", "tar.gz"
		}
		return "linux-amd64", "tar.gz"
	}
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

// extractZipEsptool extracts esptool.exe (and siblings) from a zip archive.
// The zip typically contains a subdirectory like "esptool-windows-amd64/esptool.exe".
func extractZipEsptool(zipPath, destDir string) error {
	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer r.Close()

	// Extract all .exe files (esptool, espefuse, espsecure) + LICENSE
	for _, f := range r.File {
		name := filepath.Base(f.Name)
		if f.FileInfo().IsDir() {
			continue
		}

		// Only extract top-level binaries and text files from the archive
		if !strings.HasSuffix(name, ".exe") && name != "LICENSE" && name != "README.md" {
			continue
		}

		destPath := filepath.Join(destDir, name)
		if err := extractZipFile(f, destPath); err != nil {
			return fmt.Errorf("cannot extract %s: %w", name, err)
		}
	}
	return nil
}

func extractZipFile(f *zip.File, destPath string) error {
	rc, err := f.Open()
	if err != nil {
		return err
	}
	defer rc.Close()

	out, err := os.Create(destPath)
	if err != nil {
		return err
	}
	defer out.Close()

	_, err = io.Copy(out, rc)
	return err
}
