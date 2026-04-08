package firmware

// ScaleFX Firmware — GitHub Release Integration
// Fetches available firmware releases from GitHub, downloads zipped firmware
// assets, and flashes them to connected boards.
//
// Release convention:
//   Tag:   {controller}-v{version}  (e.g. gunfx-v0.3.0)
//   Asset: {controller}-firmware-v{version}.zip
//   Zip:   contains firmware.uf2 (Pico) or firmware.bin (ESP32-S3)

import (
	"archive/zip"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	defaultOwner = "mkudzia84"
	defaultRepo  = "scalefx"
	apiBase      = "https://api.github.com"
	apiTimeout   = 15 * time.Second
)

// ─── Release Types ───

// Release represents a firmware release for a specific controller.
type Release struct {
	Controller string `json:"controller"` // e.g. "gunfx"
	Version    string `json:"version"`    // e.g. "0.3.0"
	Tag        string `json:"tag"`        // e.g. "gunfx-v0.3.0"
	Name       string `json:"name"`       // release title
	Body       string `json:"body"`       // release notes (markdown)
	Prerelease bool   `json:"prerelease"`
	Published  string `json:"published"`  // ISO 8601 timestamp
	AssetName  string `json:"assetName"`  // e.g. "gunfx-firmware-v0.3.0.zip"
	AssetURL   string `json:"assetUrl"`   // browser download URL
	AssetSize  int64  `json:"assetSize"`  // bytes
}

// ReleaseOptions configures release fetching.
type ReleaseOptions struct {
	Owner string // GitHub owner (default: mkudzia84)
	Repo  string // GitHub repo (default: scalefx)
}

func (o *ReleaseOptions) owner() string {
	if o != nil && o.Owner != "" {
		return o.Owner
	}
	return defaultOwner
}

func (o *ReleaseOptions) repo() string {
	if o != nil && o.Repo != "" {
		return o.Repo
	}
	return defaultRepo
}

// ─── GitHub API Types (minimal) ───

type ghRelease struct {
	TagName    string    `json:"tag_name"`
	Name       string    `json:"name"`
	Body       string    `json:"body"`
	Prerelease bool      `json:"prerelease"`
	Draft      bool      `json:"draft"`
	Published  time.Time `json:"published_at"`
	Assets     []ghAsset `json:"assets"`
}

type ghAsset struct {
	Name               string `json:"name"`
	BrowserDownloadURL string `json:"browser_download_url"`
	Size               int64  `json:"size"`
	ContentType        string `json:"content_type"`
}

// ─── Fetch Releases ───

// FetchReleases returns all firmware releases from GitHub, optionally filtered
// to a specific controller. Pass controller="" to get releases for all controllers.
func FetchReleases(controller string, opts *ReleaseOptions) ([]Release, error) {
	url := fmt.Sprintf("%s/repos/%s/%s/releases?per_page=100", apiBase, opts.owner(), opts.repo())

	client := &http.Client{Timeout: apiTimeout}
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, fmt.Errorf("create request: %w", err)
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "ScaleFX-Studio/1.0")

	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("fetch releases: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("GitHub API returned %d: %s", resp.StatusCode, string(body))
	}

	var ghReleases []ghRelease
	if err := json.NewDecoder(resp.Body).Decode(&ghReleases); err != nil {
		return nil, fmt.Errorf("parse releases: %w", err)
	}

	var releases []Release
	for _, gh := range ghReleases {
		if gh.Draft {
			continue
		}

		// Parse tag: {controller}-v{version}
		ctrl, version := parseTag(gh.TagName)
		if ctrl == "" || version == "" {
			continue
		}

		// Filter by controller if specified
		if controller != "" && ctrl != controller {
			continue
		}

		// Find the firmware zip asset
		asset := findFirmwareAsset(gh.Assets, ctrl, version)
		if asset == nil {
			continue
		}

		releases = append(releases, Release{
			Controller: ctrl,
			Version:    version,
			Tag:        gh.TagName,
			Name:       gh.Name,
			Body:       gh.Body,
			Prerelease: gh.Prerelease,
			Published:  gh.Published.Format(time.RFC3339),
			AssetName:  asset.Name,
			AssetURL:   asset.BrowserDownloadURL,
			AssetSize:  asset.Size,
		})
	}

	// Sort: newest first (by tag version, descending)
	sort.Slice(releases, func(i, j int) bool {
		if releases[i].Controller != releases[j].Controller {
			return releases[i].Controller < releases[j].Controller
		}
		return compareVersions(releases[i].Version, releases[j].Version) > 0
	})

	return releases, nil
}

// FetchReleasesForBoard returns releases for the controller matching the
// currently connected board type. Returns an empty slice if the controller
// type has no published releases.
func FetchReleasesForBoard(controllerType string, opts *ReleaseOptions) ([]Release, error) {
	return FetchReleases(controllerType, opts)
}

// ─── Download & Extract ───

// DownloadRelease downloads the firmware zip from a release and extracts
// the firmware file to a temp directory. Returns the path to the extracted
// firmware file. The caller is responsible for cleaning up the temp dir.
func DownloadRelease(rel Release, onProgress func(downloaded, total int64)) (fwPath string, cleanupDir string, err error) {
	ctrl, ok := Controllers[rel.Controller]
	if !ok {
		return "", "", fmt.Errorf("unknown controller: %s", rel.Controller)
	}

	// Create temp directory
	tmpDir, err := os.MkdirTemp("", "scalefx-firmware-*")
	if err != nil {
		return "", "", fmt.Errorf("create temp dir: %w", err)
	}

	zipPath := filepath.Join(tmpDir, rel.AssetName)

	// Download zip
	client := &http.Client{Timeout: 5 * time.Minute}
	resp, err := client.Get(rel.AssetURL)
	if err != nil {
		os.RemoveAll(tmpDir)
		return "", "", fmt.Errorf("download release: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		os.RemoveAll(tmpDir)
		return "", "", fmt.Errorf("download returned HTTP %d", resp.StatusCode)
	}

	f, err := os.Create(zipPath)
	if err != nil {
		os.RemoveAll(tmpDir)
		return "", "", fmt.Errorf("create zip file: %w", err)
	}

	var downloaded int64
	buf := make([]byte, 32*1024)
	for {
		n, readErr := resp.Body.Read(buf)
		if n > 0 {
			if _, wErr := f.Write(buf[:n]); wErr != nil {
				f.Close()
				os.RemoveAll(tmpDir)
				return "", "", fmt.Errorf("write zip: %w", wErr)
			}
			downloaded += int64(n)
			if onProgress != nil {
				onProgress(downloaded, rel.AssetSize)
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			f.Close()
			os.RemoveAll(tmpDir)
			return "", "", fmt.Errorf("download error: %w", readErr)
		}
	}
	f.Close()

	// Extract firmware from zip
	fwFile := "firmware." + ctrl.FwExt
	extractedPath, err := extractFromZip(zipPath, fwFile, tmpDir)
	if err != nil {
		os.RemoveAll(tmpDir)
		return "", "", fmt.Errorf("extract firmware: %w", err)
	}

	return extractedPath, tmpDir, nil
}

// ─── Flash from Release ───

// FlashRelease downloads a release and flashes it to the board.
// This integrates with the existing flash pipeline.
func FlashRelease(rel Release, opts *Options) error {
	ctrl, ok := Controllers[rel.Controller]
	if !ok {
		return fmt.Errorf("unknown controller: %s", rel.Controller)
	}

	if opts.Timeout == 0 {
		opts.Timeout = 15
	}

	// Plan steps
	steps := []string{"download", "flash"}
	if !opts.SkipVerify {
		steps = append(steps, "verify")
	}
	total := len(steps)
	stepNum := 0

	// Step 1: Download
	stepNum++
	opts.step(stepNum, total, "Downloading %s v%s...", rel.Controller, rel.Version)

	fwPath, tmpDir, err := DownloadRelease(rel, func(downloaded, totalBytes int64) {
		if totalBytes > 0 {
			pct := int(downloaded * 100 / totalBytes)
			opts.emit(Event{
				Type:     EventProgress,
				Message:  fmt.Sprintf("Downloading... %d%%", pct),
				Progress: pct,
			})
		}
	})
	if err != nil {
		return fmt.Errorf("download failed: %w", err)
	}
	defer os.RemoveAll(tmpDir)

	opts.ok("Downloaded %s (%s)", rel.AssetName, formatSize(rel.AssetSize))

	// Step 2: Flash
	stepNum++
	if ctrl.IsESP32() {
		opts.step(stepNum, total, "Uploading via esptool...")
		if err := flashESP32FromFile(opts, ctrl, fwPath); err != nil {
			return fmt.Errorf("flash failed: %w", err)
		}
	} else {
		opts.step(stepNum, total, "Flashing via UF2...")
		if err := flashPicoFromFile(opts, ctrl, fwPath); err != nil {
			return fmt.Errorf("flash failed: %w", err)
		}
	}

	// Step 3: Verify
	if !opts.SkipVerify {
		stepNum++
		opts.step(stepNum, total, "Verifying device...")
		if err := VerifyDevice(opts, ctrl); err != nil {
			opts.warn("Verification: %s", err)
		} else {
			opts.ok("Device verified")
		}
	}

	opts.ok("Flash complete!")
	return nil
}

// ─── Internal Helpers ───

// parseTag extracts controller name and version from a release tag.
// Expected format: {controller}-v{version} e.g. "gunfx-v0.3.0"
func parseTag(tag string) (controller, version string) {
	// Find last occurrence of "-v" to split
	idx := strings.LastIndex(tag, "-v")
	if idx < 1 || idx >= len(tag)-2 {
		return "", ""
	}
	controller = tag[:idx]
	version = tag[idx+2:]

	// Validate controller is known
	if _, ok := Controllers[controller]; !ok {
		return "", ""
	}

	return controller, version
}

// findFirmwareAsset finds the zip asset matching the expected naming convention.
func findFirmwareAsset(assets []ghAsset, controller, version string) *ghAsset {
	expected := fmt.Sprintf("%s-firmware-v%s.zip", controller, version)
	for i, a := range assets {
		if strings.EqualFold(a.Name, expected) {
			return &assets[i]
		}
	}
	// Fallback: any zip with the controller name
	for i, a := range assets {
		if strings.HasPrefix(strings.ToLower(a.Name), controller) && strings.HasSuffix(strings.ToLower(a.Name), ".zip") {
			return &assets[i]
		}
	}
	return nil
}

// extractFromZip extracts a named file from a zip archive to the given directory.
func extractFromZip(zipPath, fileName, destDir string) (string, error) {
	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return "", fmt.Errorf("open zip: %w", err)
	}
	defer r.Close()

	for _, f := range r.File {
		// Match by base name (zip may or may not have directory structure)
		if filepath.Base(f.Name) == fileName {
			rc, err := f.Open()
			if err != nil {
				return "", fmt.Errorf("open %s in zip: %w", f.Name, err)
			}
			defer rc.Close()

			destPath := filepath.Join(destDir, fileName)
			out, err := os.Create(destPath)
			if err != nil {
				return "", fmt.Errorf("create %s: %w", destPath, err)
			}
			if _, err := io.Copy(out, rc); err != nil {
				out.Close()
				return "", fmt.Errorf("extract %s: %w", fileName, err)
			}
			out.Close()
			return destPath, nil
		}
	}

	return "", fmt.Errorf("firmware file %s not found in zip", fileName)
}

// compareVersions compares two semver strings. Returns >0 if a > b, <0 if a < b, 0 if equal.
func compareVersions(a, b string) int {
	aParts := strings.Split(a, ".")
	bParts := strings.Split(b, ".")
	maxLen := len(aParts)
	if len(bParts) > maxLen {
		maxLen = len(bParts)
	}
	for i := 0; i < maxLen; i++ {
		var ai, bi int
		if i < len(aParts) {
			fmt.Sscanf(aParts[i], "%d", &ai)
		}
		if i < len(bParts) {
			fmt.Sscanf(bParts[i], "%d", &bi)
		}
		if ai != bi {
			return ai - bi
		}
	}
	return 0
}

// formatSize returns a human-readable byte size.
func formatSize(bytes int64) string {
	if bytes >= 1048576 {
		return fmt.Sprintf("%.1f MB", float64(bytes)/1048576)
	}
	if bytes >= 1024 {
		return fmt.Sprintf("%.1f KB", float64(bytes)/1024)
	}
	return fmt.Sprintf("%d B", bytes)
}

// flashPicoFromFile flashes a specific firmware file to a Pico board.
func flashPicoFromFile(opts *Options, ctrl Controller, fwPath string) error {
	port := opts.Port
	var err error
	if port == "" {
		port, err = DetectPicoPort()
		if err != nil {
			return fmt.Errorf("cannot find Pico serial port: %w", err)
		}
		opts.info("Detected Pico port: %s", port)
	}

	// The board may reboot instantly on the 1200-baud open, causing the
	// serial library to report an error. That's OK — check for the drive.
	opts.info("Entering BOOTSEL mode via %s...", port)
	if err := enterBootsel(port); err != nil {
		opts.warn("BOOTSEL serial trigger: %v (checking for drive anyway)", err)
	}

	opts.info("Waiting for BOOTSEL drive...")
	drive, err := waitForBootselDrive(opts.Timeout)
	if err != nil {
		return fmt.Errorf("BOOTSEL drive not found: %w", err)
	}
	opts.info("Found BOOTSEL drive: %s", drive)

	opts.info("Copying firmware to %s...", drive)
	if err := copyFirmware(fwPath, drive); err != nil {
		return fmt.Errorf("firmware copy failed: %w", err)
	}
	opts.ok("UF2 copied to %s", drive)

	opts.info("Waiting for device to reboot...")
	time.Sleep(3 * time.Second)
	return nil
}

// flashESP32FromFile flashes a specific firmware file to an ESP32-S3 via esptool.
// Delegates to FlashESP32FromBinary which prefers standalone esptool.
func flashESP32FromFile(opts *Options, ctrl Controller, fwPath string) error {
	return FlashESP32FromBinary(opts, opts.Port, fwPath)
}
