package main

// Per-board YAML config round-trip + native open/save dialogs. Mirrors
// firmware schemas' defaultPath() — Rule 26.  Built on client.Storage +
// client.Config.

import (
	"fmt"
	"os"
	"strings"
	"time"

	"scalefx/client"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// OpenedFile is the result of OpenTextFile — Path is the absolute disk path,
// Content is the UTF-8 file body. Empty Path + nil error = user cancelled.
type OpenedFile struct {
	Path    string `json:"path"`
	Content string `json:"content"`
}

// configPathFor returns the canonical per-board YAML path on the device's
// flash. Mirrors firmware schemas' defaultPath(). Empty controller type
// falls back to /config.yaml.
func (a *App) configPathFor(ct string) string {
	switch ct {
	case "gearcontrol":
		return "/gearcontrol.yaml"
	case "lightfx":
		return "/lightfx.yaml"
	case "hubfx":
		return "/hubfx.yaml"
	case "gunfx":
		return "/gunfx.yaml"
	default:
		return "/config.yaml"
	}
}

// DownloadConfig reads the connected board's config YAML from flash.
// Returns "" + nil when the file does not exist (fresh board → defaults).
func (a *App) DownloadConfig() (string, error) {
	defer a.diag.Around("DownloadConfig", nil)()
	a.mu.Lock()
	defer a.mu.Unlock()

	path := a.configPathFor(a.kind)
	a.diag.Info("CFG", "loading %s from device", path)
	a.echoCommand(fmt.Sprintf("config.load (%s)", path))
	if a.c == nil {
		a.echoError("not connected")
		return "", fmt.Errorf("not connected")
	}
	result, err := a.c.Storage.FileDownload(path, 10*time.Second)
	if err != nil {
		msg := err.Error()
		if strings.Contains(msg, "not found") || strings.Contains(msg, "NOT_FOUND") {
			a.echoOutput(fmt.Sprintf("%s not found — using defaults", path))
			return "", nil
		}
		a.diag.Warn("CFG", "config load failed: %s", msg)
		a.echoError("config load failed: %s", msg)
		return "", err
	}
	a.diag.With(LvlInfo, "CFG", "config loaded", map[string]any{"path": path, "bytes": len(result.Data)})
	a.echoOK("Loaded %s (%d bytes)", path, len(result.Data))
	return string(result.Data), nil
}

// UploadConfig writes the supplied YAML to the board's config path on flash,
// then triggers a config reload so values take effect without a reboot.
// The client uploader reads from disk, so the YAML is staged to a temp file.
func (a *App) UploadConfig(yaml string) error {
	defer a.diag.Around("UploadConfig", map[string]any{"bytes": len(yaml)})()
	a.mu.Lock()
	defer a.mu.Unlock()

	path := a.configPathFor(a.kind)
	data := []byte(yaml)
	a.diag.Info("CFG", "saving %d bytes → %s", len(data), path)
	a.echoCommand(fmt.Sprintf("config.save (%d bytes → %s)", len(data), path))
	if a.c == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}

	// Stage to a temp file — client.Storage.FileUpload reads from disk.
	tmp, err := os.CreateTemp("", "scalefx-cfg-*.yaml")
	if err != nil {
		a.echoError("temp file: %v", err)
		return err
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		a.echoError("temp write: %v", err)
		return err
	}
	tmp.Close()

	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "uploading", "path": path, "sent": 0, "total": len(data),
	})
	res, err := a.c.Storage.FileUpload(tmpPath, client.UploadOptions{
		Path:   path,
		Target: client.TargetFlash,
		Mode:   client.UploadSync,
		OnProgress: func(sent, total int64) {
			wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
				"phase": "uploading", "path": path, "sent": int(sent), "total": int(total),
			})
		},
	})
	if err != nil {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": err.Error()})
		a.diag.Warn("CFG", "config upload failed: %v", err)
		a.echoError("upload failed: %v", err)
		return err
	}

	a.echoCommand("config.reload")
	if err := a.c.Config.Reload(); err != nil {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": err.Error()})
		a.diag.Warn("CFG", "uploaded but config.reload failed: %v", err)
		a.echoError("uploaded but reload failed: %v", err)
		return fmt.Errorf("uploaded but reload failed: %w", err)
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "done", "path": path,
		"sent": int(res.BytesSent), "total": int(res.BytesSent),
		"md5Match": res.MD5Match, "speedKBs": res.SpeedKBs,
	})
	a.diag.With(LvlInfo, "CFG", "config saved + reloaded",
		map[string]any{"path": path, "bytes": res.BytesSent, "kbps": res.SpeedKBs, "md5": res.MD5Match})
	a.echoOK("Saved %s (%d bytes, %.1f KB/s) — reloaded", path, res.BytesSent, res.SpeedKBs)
	return nil
}

// SaveTextFile prompts for a destination via the native save dialog and
// writes `content` there. Empty string return = user cancelled.
func (a *App) SaveTextFile(content, defaultName, title string) (string, error) {
	if title == "" {
		title = "Save File"
	}
	path, err := wailsRT.SaveFileDialog(a.ctx, wailsRT.SaveDialogOptions{
		Title:           title,
		DefaultFilename: defaultName,
	})
	if err != nil {
		return "", err
	}
	if path == "" {
		return "", nil
	}
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		return "", err
	}
	return path, nil
}

// OpenTextFile prompts for a file via the native open dialog and returns its
// contents. Empty path + nil error = cancelled.
func (a *App) OpenTextFile(title string) (OpenedFile, error) {
	if title == "" {
		title = "Open File"
	}
	path, err := wailsRT.OpenFileDialog(a.ctx, wailsRT.OpenDialogOptions{Title: title})
	if err != nil {
		return OpenedFile{}, err
	}
	if path == "" {
		return OpenedFile{}, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return OpenedFile{}, err
	}
	return OpenedFile{Path: path, Content: string(data)}, nil
}
