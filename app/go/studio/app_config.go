package main

// Per-board YAML config round-trip + native open/save dialogs. Mirrors
// firmware schemas' defaultPath() — Rule 26.

import (
	"fmt"
	"os"
	"scalefx/api"
	hfxp "scalefx/protocol/hubfx"
	"strings"
	"time"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// OpenedFile is the result of OpenTextFile — Path is the absolute disk path,
// Content is the UTF-8 file body. Empty Path + nil error = user cancelled.
type OpenedFile struct {
	Path    string `json:"path"`
	Content string `json:"content"`
}

// configPathFor returns the canonical per-board YAML path on the device's
// flash. Mirrors firmware schemas' defaultPath(): gearcontrol → /gearcontrol.yaml,
// lightfx → /lightfx.yaml, hubfx → /hubfx.yaml, gunfx → /gunfx.yaml (reserved;
// no schema yet). Empty controller type falls back to /config.yaml so legacy
// boards still round-trip until they are re-flashed with the new firmware.
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

// DownloadConfig reads /config.yaml from the connected board's flash and
// returns its contents as a string. Returns an empty string + nil error when
// the file does not exist (fresh board) so the caller can treat that case as
// "start from defaults" rather than propagating an error.
func (a *App) DownloadConfig() (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	path := a.configPathFor(a.eng.ControllerType)
	a.echoCommand(fmt.Sprintf("config.load (%s)", path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return "", fmt.Errorf("not connected")
	}
	result, err := a.eng.API.Files.Download(hfxp.StorageTargetFlash, path, 10*time.Second)
	if err != nil {
		msg := err.Error()
		if strings.Contains(msg, "not found") || strings.Contains(msg, "NOT_FOUND") {
			a.echoOutput(fmt.Sprintf("%s not found — using defaults", path))
			return "", nil
		}
		a.echoError("config load failed: %s", msg)
		return "", err
	}
	a.echoOK("Loaded %s (%d bytes)", path, len(result.Data))
	return string(result.Data), nil
}

// UploadConfig writes the supplied YAML to /config.yaml on the connected
// board's flash, then triggers a config reload so the new values take effect
// without a reboot. Emits "fs:progress" events so the Studio can show the
// shared UploadProgressDialog.
func (a *App) UploadConfig(yaml string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	data := []byte(yaml)
	path := a.configPathFor(a.eng.ControllerType)
	a.echoCommand(fmt.Sprintf("config.save (%d bytes → %s)", len(data), path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "uploading", "path": path, "sent": 0, "total": len(data),
	})
	res := a.eng.API.Files.Upload(hfxp.StorageTargetFlash, path,
		data, api.UploadSync, func(sent, total int) {
			wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
				"phase": "uploading", "path": path, "sent": sent, "total": total,
			})
		})
	if !res.OK {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": res.Error})
		a.echoError("upload failed: %s", res.Error)
		return fmt.Errorf("upload failed: %s", res.Error)
	}
	a.echoCommand("config.reload")
	reload := a.eng.API.HubFx.ConfigReload("")
	if !reload.OK {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": reload.Error})
		a.echoError("uploaded but reload failed: %s", reload.Error)
		return fmt.Errorf("uploaded but reload failed: %s", reload.Error)
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase":    "done",
		"path":     path,
		"sent":     int(res.BytesTransferred),
		"total":    int(res.BytesTransferred),
		"md5Match": res.MD5Match,
		"speedKBs": res.SpeedKBs,
	})
	a.echoOK("Saved %s (%d bytes, %.1f KB/s) — reloaded",
		path, res.BytesTransferred, res.SpeedKBs)
	return nil
}

// SaveTextFile prompts the user for a destination via the native save dialog
// and writes `content` there. Returns the chosen path; empty string if the
// user cancelled (treated as non-error).
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

// OpenTextFile prompts the user for a file via the native open dialog and
// returns the file's contents as a string. Path is returned so the caller
// can show "imported from …" feedback. Empty path + nil error = cancelled.
func (a *App) OpenTextFile(title string) (OpenedFile, error) {
	if title == "" {
		title = "Open File"
	}
	path, err := wailsRT.OpenFileDialog(a.ctx, wailsRT.OpenDialogOptions{
		Title: title,
	})
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
