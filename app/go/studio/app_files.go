package main

// Filesystem operations exposed to the GUI File Manager: list/mkdir/delete,
// per-file download/upload, batch upload with progress, plus the storage
// status probe that gates flash/SD UI.

import (
	"fmt"
	"os"
	"path/filepath"
	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/core"
	hfxp "scalefx/protocol/hubfx"
	"strings"
	"time"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// ─── Types ───

// FsEntry is one row of a directory listing.
type FsEntry struct {
	Name  string `json:"name"`
	IsDir bool   `json:"isDir"`
	Size  uint32 `json:"size"`
}

// FsStorageStatus reports which targets are online and their capacity.
// Missing/uninitialized targets have Available=false; frontend hides them.
type FsStorageStatus struct {
	FlashAvailable bool   `json:"flashAvailable"`
	FlashTotal     uint32 `json:"flashTotal"`
	FlashUsed      uint32 `json:"flashUsed"`
	FlashFree      uint32 `json:"flashFree"`

	SdAvailable bool   `json:"sdAvailable"`
	SdCardMB    uint32 `json:"sdCardMB"`
	SdTotalMB   uint32 `json:"sdTotalMB"`
	SdUsedMB    uint32 `json:"sdUsedMB"`
	SdFreeMB    uint32 `json:"sdFreeMB"`
	SdCardType  string `json:"sdCardType"`
	SdBusMode   string `json:"sdBusMode"`
}

// targetByte maps the frontend "flash" | "sd" string to the protocol byte.
func targetByte(target string) (byte, error) {
	switch target {
	case "flash":
		return hfxp.StorageTargetFlash, nil
	case "sd":
		return hfxp.StorageTargetSd, nil
	default:
		return 0, fmt.Errorf("unknown storage target: %s", target)
	}
}

// ─── Storage Status / Capabilities ───

// FsStorageStatus queries both flash and SD status. Always returns a status
// struct — unavailable targets simply have Available=false.
//
// Capability gating: when the firmware advertises CapFlash/CapSd in IDENTIFY
// (Rule 11 append-only field), we skip queries for missing interfaces — both
// to avoid round-trips and to give the frontend an authoritative "this board
// has no SD slot" signal. Legacy firmware (caps==0) falls through to the
// probe-everything behaviour for back-compat.
func (a *App) FsStorageStatus() (FsStorageStatus, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	out := FsStorageStatus{}
	a.echoCommand("flash.status")
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return out, fmt.Errorf("not connected")
	}

	caps := a.eng.Capabilities()
	probeFlash := caps == 0 || caps&core.CapFlash != 0
	probeSd := caps == 0 || caps&core.CapSd != 0

	if probeFlash {
		// Flash (present on every controller that registers StorageServer).
		if res := a.eng.API.HubFx.FlashStatus(); res.OK && res.Response != nil {
			p := res.Response.Payload
			switch {
			case len(p) >= 13 && p[0] != 0:
				out.FlashAvailable = true
				out.FlashTotal = protocol.ReadU32LE(p, 1)
				out.FlashUsed = protocol.ReadU32LE(p, 5)
				out.FlashFree = protocol.ReadU32LE(p, 9)
				a.echoOutput(fmt.Sprintf("flash: total=%d used=%d free=%d",
					out.FlashTotal, out.FlashUsed, out.FlashFree))
			case len(p) >= 1 && p[0] == 0:
				a.echoOutput(fmt.Sprintf("flash: not initialised on device (payload len=%d)", len(p)))
			default:
				a.echoOutput(fmt.Sprintf("flash: unexpected response (len=%d)", len(p)))
			}
		} else {
			msg := "no response"
			if res.Error != "" {
				msg = res.Error
			}
			a.echoError("flash.status failed: %s", msg)
		}
	} else {
		a.echoOutput("flash: not advertised by board")
	}

	a.echoCommand("sd.status")
	if probeSd {
		// SD (NACK'd with NOT_SUPPORTED on boards with no slot — treat as unavailable)
		if res := a.eng.API.HubFx.SdStatus(); res.OK && res.Response != nil {
			p := res.Response.Payload
			if len(p) >= 1 && p[0] != 0 {
				out.SdAvailable = true
				if len(p) >= 14 {
					out.SdCardMB = protocol.ReadU32LE(p, 1)
					out.SdTotalMB = protocol.ReadU32LE(p, 5)
					out.SdFreeMB = protocol.ReadU32LE(p, 9)
				}
				if len(p) >= 20 {
					cardTypes := map[byte]string{0: "NONE", 1: "MMC", 2: "SD", 3: "SDHC", 4: "UNKNOWN"}
					busModes := map[byte]string{0: "SPI", 1: "SDIO 1-bit", 2: "SDIO 4-bit"}
					out.SdCardType = cardTypes[p[14]]
					out.SdBusMode = busModes[p[15]]
					out.SdUsedMB = protocol.ReadU32LE(p, 16)
				}
				a.echoOutput(fmt.Sprintf("sd: card=%s bus=%s total=%dMB used=%dMB free=%dMB",
					out.SdCardType, out.SdBusMode, out.SdTotalMB, out.SdUsedMB, out.SdFreeMB))
			} else {
				a.echoOutput("sd: not available")
			}
		} else {
			a.echoOutput("sd: not supported")
		}
	} else {
		a.echoOutput("sd: not advertised by board")
	}
	return out, nil
}

// DeviceCapabilities returns the bitmask of CoreCapability flags the
// connected board advertised in IDENTIFY/INIT_READY. Returns 0 when no
// device is connected, identification hasn't completed yet, or the firmware
// pre-dates the capabilities field — frontends should treat 0 as "unknown,
// fall back to probing" rather than "nothing supported".
func (a *App) DeviceCapabilities() uint32 {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.eng.Capabilities()
}

// ─── List / Mkdir / Delete ───

// FsList returns a directory listing. Parses the server's
// "d|f\tname\tsize\n" text output into structured entries.
func (a *App) FsList(target, path string) ([]FsEntry, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.list %s %s", target, path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return nil, fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return nil, err
	}
	text, err := a.eng.API.Files.List(tb, path)
	if err != nil {
		a.echoError("%v", err)
		return nil, err
	}
	a.echoOutput(text)

	entries := []FsEntry{}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == "" {
			continue
		}
		parts := strings.Split(line, "\t")
		if len(parts) < 3 {
			continue
		}
		var size uint32
		fmt.Sscanf(parts[2], "%d", &size)
		entries = append(entries, FsEntry{
			Name:  parts[1],
			IsDir: parts[0] == "d",
			Size:  size,
		})
	}
	return entries, nil
}

// FsMkdir creates a directory on the selected target.
func (a *App) FsMkdir(target, path string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.mkdir %s %s", target, path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return err
	}
	res := a.eng.API.Files.Mkdir(tb, path, true) // GUI always uses mkdir -p (idempotent, creates ancestors)
	if !res.OK {
		a.echoError("mkdir failed: %s", res.Error)
		return fmt.Errorf("mkdir failed: %s", res.Error)
	}
	a.echoOK("Mkdir %s:%s", target, path)
	return nil
}

// FsDelete removes a file or directory (recursive by default for directories).
func (a *App) FsDelete(target, path string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.delete -r %s %s", target, path))
	if a.eng.Conn == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return err
	}
	res := a.eng.API.Files.Delete(tb, path, true) // GUI delete is always recursive
	if !res.OK {
		a.echoError("delete failed: %s", res.Error)
		return fmt.Errorf("delete failed: %s", res.Error)
	}
	a.echoOK("Delete %s:%s", target, path)
	return nil
}

// ─── Single-file Download / Upload ───

// FsDownloadToDisk pulls a file off the device and writes it to localPath.
// If localPath is empty, opens a Save dialog.
// Emits "fs:progress" events during transfer.
func (a *App) FsDownloadToDisk(target, remotePath, localPath string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.eng.Conn == nil {
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		return err
	}

	if localPath == "" {
		suggested := remotePath
		if i := strings.LastIndex(suggested, "/"); i >= 0 {
			suggested = suggested[i+1:]
		}
		chosen, err := wailsRT.SaveFileDialog(a.ctx, wailsRT.SaveDialogOptions{
			Title:           "Save " + remotePath,
			DefaultFilename: suggested,
		})
		if err != nil {
			return err
		}
		if chosen == "" {
			return fmt.Errorf("cancelled")
		}
		localPath = chosen
	}

	a.echoCommand(fmt.Sprintf("file.download %s %s", target, remotePath))
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "downloading", "path": remotePath, "sent": 0, "total": 0,
	})
	result, err := a.eng.API.Files.Download(tb, remotePath, 60*time.Second)
	if err != nil {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": err.Error()})
		a.echoError("%v", err)
		return err
	}
	if err := os.WriteFile(localPath, result.Data, 0644); err != nil {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": err.Error()})
		a.echoError("write %s: %v", localPath, err)
		return err
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "done", "path": remotePath, "sent": len(result.Data), "total": len(result.Data),
	})
	a.echoOK("Downloaded %s (%d bytes) → %s", remotePath, len(result.Data), localPath)
	return nil
}

// FsUploadFromDisk reads localPath and uploads it to remotePath on the target.
// Mode is auto-picked via api.PickUploadMode: flash always sync, SD switches
// to batch (UploadStream) above api.LargeFileBatchThreshold.
// If localPath is empty, opens an Open dialog.
func (a *App) FsUploadFromDisk(target, remotePath, localPath string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.eng.Conn == nil {
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		return err
	}

	if localPath == "" {
		chosen, err := wailsRT.OpenFileDialog(a.ctx, wailsRT.OpenDialogOptions{
			Title: "Upload file to " + target,
		})
		if err != nil {
			return err
		}
		if chosen == "" {
			return fmt.Errorf("cancelled")
		}
		localPath = chosen
	}

	data, err := os.ReadFile(localPath)
	if err != nil {
		return err
	}

	mode := api.PickUploadMode(tb, len(data))
	modeName := uploadModeName(mode)

	a.echoCommand(fmt.Sprintf("file.upload %s %s (%s, %d bytes from %s)",
		target, remotePath, modeName, len(data), localPath))
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "uploading", "path": remotePath, "sent": 0, "total": len(data),
	})
	res := a.eng.API.Files.Upload(tb, remotePath, data, mode, func(sent, total int) {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
			"phase": "uploading", "path": remotePath, "sent": sent, "total": total,
		})
	})
	if !res.OK {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": res.Error})
		a.echoError("upload failed: %s", res.Error)
		return fmt.Errorf("upload failed: %s", res.Error)
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase":    "done",
		"path":     remotePath,
		"sent":     int(res.BytesTransferred),
		"total":    int(res.BytesTransferred),
		"md5Match": res.MD5Match,
		"speedKBs": res.SpeedKBs,
	})
	md5tag := ""
	if res.MD5Match {
		md5tag = " ✓ MD5 match"
	}
	a.echoOK("Uploaded %s (%d bytes, %.1f KB/s)%s",
		remotePath, res.BytesTransferred, res.SpeedKBs, md5tag)
	return nil
}

// uploadModeName returns the short label used in echoes / logs.
func uploadModeName(m api.UploadMode) string {
	switch m {
	case api.UploadStream:
		return "batch"
	case api.UploadSync:
		return "sync"
	default:
		return fmt.Sprintf("mode=%d", m)
	}
}

// ─── Batch Upload ───

// FsUploadBatch uploads a list of files or directory trees to target under
// remoteCwd. Each entry in localPaths is a disk path; files land at
// remoteCwd/<basename>, directories are walked recursively preserving tree
// structure (e.g. dropping "a/b/log.txt" while cwd=/tmp uploads to
// /tmp/a/b/log.txt). Progress events on "fs:progress" are enriched with
// batchIndex/batchTotal (file count) and batchBytesSent/batchBytesTotal.
// Mode is auto-picked per file via api.PickUploadMode: flash always sync,
// SD switches to batch above api.LargeFileBatchThreshold.
func (a *App) FsUploadBatch(target, remoteCwd string, localPaths []string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.eng.Conn == nil {
		a.emitBatchError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.emitBatchError(err.Error())
		return err
	}
	if len(localPaths) == 0 {
		a.emitBatchError("nothing to upload")
		return fmt.Errorf("nothing to upload")
	}

	type job struct {
		local  string
		remote string
		size   int64
	}

	// Walk every input path, collect files + directories (preserving tree).
	jobs := make([]job, 0, len(localPaths))
	dirs := make([]string, 0)
	dirSeen := map[string]bool{}
	var totalBytes int64

	// addDir records a directory we'll create via mkdir -p (idempotent;
	// firmware resolves missing ancestors). Dedupe skips redundant calls.
	addDir := func(remote string) {
		remote = normalizeRemote(remote)
		if remote == "" || remote == "/" || dirSeen[remote] {
			return
		}
		dirSeen[remote] = true
		dirs = append(dirs, remote)
	}

	for _, lp := range localPaths {
		info, err := os.Stat(lp)
		if err != nil {
			a.emitBatchError(fmt.Sprintf("stat %s: %v", lp, err))
			return err
		}
		base := filepath.Base(lp)
		if info.IsDir() {
			rootRemote := joinRemote(remoteCwd, base)
			addDir(rootRemote)
			err := filepath.Walk(lp, func(p string, fi os.FileInfo, werr error) error {
				if werr != nil {
					return werr
				}
				rel, rerr := filepath.Rel(lp, p)
				if rerr != nil {
					return rerr
				}
				rel = filepath.ToSlash(rel)
				if rel == "." {
					return nil
				}
				remote := joinRemote(rootRemote, rel)
				if fi.IsDir() {
					addDir(remote)
					return nil
				}
				jobs = append(jobs, job{local: p, remote: remote, size: fi.Size()})
				totalBytes += fi.Size()
				return nil
			})
			if err != nil {
				a.emitBatchError(fmt.Sprintf("walk %s: %v", lp, err))
				return err
			}
		} else {
			remote := joinRemote(remoteCwd, base)
			jobs = append(jobs, job{local: lp, remote: remote, size: info.Size()})
			totalBytes += info.Size()
			// Top-level file lands under remoteCwd — ensure the cwd exists.
			addDir(remoteCwd)
		}
	}

	if len(jobs) == 0 {
		a.emitBatchError("no files to upload (only empty directories)")
		return fmt.Errorf("no files to upload")
	}

	a.echoCommand(fmt.Sprintf("file.upload-batch %s %s (%d files, %s, mode=auto)",
		target, remoteCwd, len(jobs), humanBytes(uint64(totalBytes))))
	// mkdir -p per directory — firmware resolves ancestors, and the call is
	// idempotent when the directory already exists.
	for _, d := range dirs {
		res := a.eng.API.Files.Mkdir(tb, d, true)
		if !res.OK {
			a.echoError("mkdir -p %s failed: %s", d, res.Error)
			a.emitBatchError(fmt.Sprintf("mkdir %s: %s", d, res.Error))
			return fmt.Errorf("mkdir %s: %s", d, res.Error)
		}
	}

	// Upload loop. Emit progress with per-file + batch counters.
	var bytesDoneBefore int64 = 0
	for idx, j := range jobs {
		data, err := os.ReadFile(j.local)
		if err != nil {
			a.emitBatchError(fmt.Sprintf("read %s: %v", j.local, err))
			return err
		}
		mode := api.PickUploadMode(tb, len(data))
		a.echoOutput(fmt.Sprintf("[%d/%d] %s → %s (%s, %s)",
			idx+1, len(jobs), j.local, j.remote, humanBytes(uint64(len(data))), uploadModeName(mode)))
		a.emitBatchProgress("uploading", j.remote, 0, len(data),
			idx+1, len(jobs), bytesDoneBefore, totalBytes, 0, nil)

		res := a.eng.API.Files.Upload(tb, j.remote, data, mode, func(sent, total int) {
			a.emitBatchProgress("uploading", j.remote, sent, total,
				idx+1, len(jobs), bytesDoneBefore+int64(sent), totalBytes, 0, nil)
		})
		if !res.OK {
			a.echoError("upload %s failed: %s", j.remote, res.Error)
			a.emitBatchError(fmt.Sprintf("%s: %s", j.remote, res.Error))
			return fmt.Errorf("upload %s: %s", j.remote, res.Error)
		}
		bytesDoneBefore += int64(res.BytesTransferred)
		md5 := res.MD5Match
		a.emitBatchProgress("uploading", j.remote,
			int(res.BytesTransferred), int(res.BytesTransferred),
			idx+1, len(jobs), bytesDoneBefore, totalBytes, res.SpeedKBs, &md5)
		md5tag := ""
		if res.MD5Match {
			md5tag = " ✓ MD5"
		}
		a.echoOK("  uploaded %s (%.1f KB/s)%s", j.remote, res.SpeedKBs, md5tag)
	}

	a.emitBatchProgress("done", "", int(totalBytes), int(totalBytes),
		len(jobs), len(jobs), totalBytes, totalBytes, 0, nil)
	a.echoOK("batch complete: %d file(s), %s",
		len(jobs), humanBytes(uint64(totalBytes)))
	return nil
}

// joinRemote appends a slash-separated segment to a remote path.
func joinRemote(dir, name string) string {
	name = filepath.ToSlash(name)
	name = strings.TrimLeft(name, "/")
	if dir == "" || dir == "/" {
		return "/" + name
	}
	return strings.TrimRight(dir, "/") + "/" + name
}

func normalizeRemote(p string) string {
	p = filepath.ToSlash(p)
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	return p
}

func humanBytes(n uint64) string {
	switch {
	case n < 1024:
		return fmt.Sprintf("%d B", n)
	case n < 1024*1024:
		return fmt.Sprintf("%.1f KB", float64(n)/1024)
	case n < 1024*1024*1024:
		return fmt.Sprintf("%.1f MB", float64(n)/1048576)
	default:
		return fmt.Sprintf("%.2f GB", float64(n)/1073741824)
	}
}

func (a *App) emitBatchProgress(phase, path string, sent, total, batchIdx, batchTotal int,
	batchBytesSent, batchBytesTotal int64, speedKBs float64, md5Match *bool,
) {
	ev := map[string]any{
		"phase":           phase,
		"path":            path,
		"sent":            sent,
		"total":           total,
		"batchIndex":      batchIdx,
		"batchTotal":      batchTotal,
		"batchBytesSent":  batchBytesSent,
		"batchBytesTotal": batchBytesTotal,
	}
	if speedKBs > 0 {
		ev["speedKBs"] = speedKBs
	}
	if md5Match != nil {
		ev["md5Match"] = *md5Match
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", ev)
}

func (a *App) emitBatchError(msg string) {
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "error", "error": msg,
	})
}
