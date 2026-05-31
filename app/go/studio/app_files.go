package main

// Filesystem operations exposed to the GUI File Manager: list/mkdir/delete,
// per-file download/upload, batch upload with progress, plus the storage
// status probe that gates flash/SD UI.  Built on client.Storage.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"scalefx/client"
	"scalefx/protocol/core"

	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

// largeFileBatchThreshold is the SD-upload size above which we switch from
// the per-chunk sync loop to the high-throughput batch/stream path.
const largeFileBatchThreshold = 64 * 1024

// ─── Types ───

// FsEntry is one row of a directory listing.
type FsEntry struct {
	Name  string `json:"name"`
	IsDir bool   `json:"isDir"`
	Size  uint32 `json:"size"`
}

// FsStorageStatus reports which targets are online and their capacity.
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
		return client.TargetFlash, nil
	case "sd":
		return client.TargetSD, nil
	default:
		return 0, fmt.Errorf("unknown storage target: %s", target)
	}
}

// pickUploadMode chooses the wire path: flash always sync; SD switches to
// the batch/stream path for large files.
func pickUploadMode(target byte, size int) client.UploadMode {
	if target == client.TargetSD && size > largeFileBatchThreshold {
		return client.UploadStream
	}
	return client.UploadSync
}

func uploadModeName(m client.UploadMode) string {
	switch m {
	case client.UploadStream:
		return "batch"
	case client.UploadSync:
		return "sync"
	default:
		return fmt.Sprintf("mode=%d", m)
	}
}

// ─── Storage Status / Capabilities ───

// FsStorageStatus queries flash + SD status.  Backends the board doesn't
// advertise in IDENTIFY are skipped (Available=false, no round-trip wasted).
func (a *App) FsStorageStatus() (FsStorageStatus, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	out := FsStorageStatus{}
	a.echoCommand("flash.status")
	if a.c == nil {
		a.echoError("not connected")
		return out, fmt.Errorf("not connected")
	}

	caps := a.id.Capabilities
	probeFlash := caps&core.CapFlash != 0
	probeSd := caps&core.CapSd != 0

	if probeFlash {
		if fs, err := a.c.Storage.FlashStatus(); err == nil && fs.Initialized {
			out.FlashAvailable = true
			out.FlashTotal = fs.TotalBytes
			out.FlashUsed = fs.UsedBytes
			out.FlashFree = fs.FreeBytes
			a.echoOutput(fmt.Sprintf("flash: total=%d used=%d free=%d",
				out.FlashTotal, out.FlashUsed, out.FlashFree))
		} else if err != nil {
			a.echoError("flash.status failed: %v", err)
		} else {
			a.echoOutput("flash: not initialised on device")
		}
	} else {
		a.echoOutput("flash: not advertised by board")
	}

	a.echoCommand("sd.status")
	if probeSd {
		if sd, err := a.c.Storage.SdStatus(); err == nil && sd.Initialized {
			out.SdAvailable = true
			out.SdCardMB = sd.CardSizeMB
			out.SdTotalMB = sd.TotalSpaceMB
			out.SdFreeMB = sd.FreeSpaceMB
			out.SdUsedMB = sd.UsedSpaceMB
			cardTypes := map[byte]string{0: "NONE", 1: "MMC", 2: "SD", 3: "SDHC", 4: "UNKNOWN"}
			busModes := map[byte]string{0: "SPI", 1: "SDIO 1-bit", 2: "SDIO 4-bit"}
			out.SdCardType = cardTypes[sd.CardType]
			out.SdBusMode = busModes[sd.BusMode]
			a.echoOutput(fmt.Sprintf("sd: card=%s bus=%s total=%dMB used=%dMB free=%dMB",
				out.SdCardType, out.SdBusMode, out.SdTotalMB, out.SdUsedMB, out.SdFreeMB))
		} else {
			a.echoOutput("sd: not available")
		}
	} else {
		a.echoOutput("sd: not advertised by board")
	}
	return out, nil
}

// DeviceCapabilities returns the bitmask the board advertised in IDENTIFY.
func (a *App) DeviceCapabilities() uint32 {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.id.Capabilities
}

// ─── List / Mkdir / Delete ───

func (a *App) FsList(target, path string) ([]FsEntry, error) {
	defer a.diag.Around("FsList", map[string]any{"target": target, "path": path})()
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.list %s %s", target, path))
	if a.c == nil {
		a.echoError("not connected")
		return nil, fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return nil, err
	}
	text, err := a.c.Storage.FileList(path, tb)
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
		entries = append(entries, FsEntry{Name: parts[1], IsDir: parts[0] == "d", Size: size})
	}
	return entries, nil
}

func (a *App) FsMkdir(target, path string) error {
	defer a.diag.Around("FsMkdir", map[string]any{"target": target, "path": path})()
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.mkdir %s %s", target, path))
	if a.c == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return err
	}
	if err := a.c.Storage.FileMkdir(path, tb, client.MkdirFlagParents); err != nil {
		a.echoError("mkdir failed: %v", err)
		return err
	}
	a.echoOK("Mkdir %s:%s", target, path)
	return nil
}

func (a *App) FsDelete(target, path string) error {
	defer a.diag.Around("FsDelete", map[string]any{"target": target, "path": path})()
	a.mu.Lock()
	defer a.mu.Unlock()

	a.echoCommand(fmt.Sprintf("file.delete -r %s %s", target, path))
	if a.c == nil {
		a.echoError("not connected")
		return fmt.Errorf("not connected")
	}
	tb, err := targetByte(target)
	if err != nil {
		a.echoError("%v", err)
		return err
	}
	if err := a.c.Storage.FileDelete(path, tb, client.DeleteFlagRecursive); err != nil {
		a.echoError("delete failed: %v", err)
		return err
	}
	a.echoOK("Delete %s:%s", target, path)
	return nil
}

// ─── Single-file Download / Upload ───

func (a *App) FsDownloadToDisk(target, remotePath, localPath string) error {
	defer a.diag.Around("FsDownloadToDisk",
		map[string]any{"target": target, "remote": remotePath, "local": localPath})()
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.c == nil {
		return fmt.Errorf("not connected")
	}
	if _, err := targetByte(target); err != nil {
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
	result, err := a.c.Storage.FileDownload(remotePath, 60*time.Second)
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

func (a *App) FsUploadFromDisk(target, remotePath, localPath string) error {
	defer a.diag.Around("FsUploadFromDisk",
		map[string]any{"target": target, "remote": remotePath, "local": localPath})()
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.c == nil {
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

	stat, err := os.Stat(localPath)
	if err != nil {
		return err
	}
	size := int(stat.Size())
	mode := pickUploadMode(tb, size)

	a.echoCommand(fmt.Sprintf("file.upload %s %s (%s, %d bytes from %s)",
		target, remotePath, uploadModeName(mode), size, localPath))
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "uploading", "path": remotePath, "sent": 0, "total": size,
	})
	collBefore := a.c.Conn().Collisions()
	res, err := a.c.Storage.FileUpload(localPath, client.UploadOptions{
		Path:   remotePath,
		Target: tb,
		Mode:   mode,
		OnProgress: func(sent, total int64) {
			wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
				"phase": "uploading", "path": remotePath, "sent": int(sent), "total": int(total),
			})
		},
	})
	if err != nil {
		wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": err.Error()})
		a.echoError("upload failed: %v", err)
		return err
	}
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{
		"phase": "done", "path": remotePath,
		"sent": int(res.BytesSent), "total": int(res.BytesSent),
		"md5Match": res.MD5Match, "speedKBs": res.SpeedKBs,
	})
	md5tag := ""
	if res.MD5Match {
		md5tag = " ✓ MD5 match"
	}
	a.echoOK("Uploaded %s (%d bytes, %.1f KB/s)%s", remotePath, res.BytesSent, res.SpeedKBs, md5tag)
	if coll := a.c.Conn().Collisions() - collBefore; coll > 0 {
		a.echoError("⚠ %d wire collision(s) during this upload — a background "+
			"packet corrupted the stream (see WIRE-COLLISION warnings above)", coll)
	}
	return nil
}

// ─── Batch Upload ───

func (a *App) FsUploadBatch(target, remoteCwd string, localPaths []string) error {
	defer a.diag.Around("FsUploadBatch",
		map[string]any{"target": target, "cwd": remoteCwd, "files": len(localPaths)})()
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.c == nil {
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

	jobs := make([]job, 0, len(localPaths))
	dirs := make([]string, 0)
	dirSeen := map[string]bool{}
	var totalBytes int64

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
			addDir(remoteCwd)
		}
	}

	if len(jobs) == 0 {
		a.emitBatchError("no files to upload (only empty directories)")
		return fmt.Errorf("no files to upload")
	}

	a.echoCommand(fmt.Sprintf("file.upload-batch %s %s (%d files, %s, mode=auto)",
		target, remoteCwd, len(jobs), humanBytes(uint64(totalBytes))))
	for _, d := range dirs {
		if err := a.c.Storage.FileMkdir(d, tb, client.MkdirFlagParents); err != nil {
			a.echoError("mkdir -p %s failed: %v", d, err)
			a.emitBatchError(fmt.Sprintf("mkdir %s: %v", d, err))
			return fmt.Errorf("mkdir %s: %w", d, err)
		}
	}

	var bytesDoneBefore int64 = 0
	for idx, j := range jobs {
		mode := pickUploadMode(tb, int(j.size))
		a.echoOutput(fmt.Sprintf("[%d/%d] %s → %s (%s, %s)",
			idx+1, len(jobs), j.local, j.remote, humanBytes(uint64(j.size)), uploadModeName(mode)))
		a.emitBatchProgress("uploading", j.remote, 0, int(j.size),
			idx+1, len(jobs), bytesDoneBefore, totalBytes, 0, nil)

		captureIdx := idx
		res, err := a.c.Storage.FileUpload(j.local, client.UploadOptions{
			Path:   j.remote,
			Target: tb,
			Mode:   mode,
			OnProgress: func(sent, total int64) {
				a.emitBatchProgress("uploading", j.remote, int(sent), int(total),
					captureIdx+1, len(jobs), bytesDoneBefore+sent, totalBytes, 0, nil)
			},
		})
		if err != nil {
			a.echoError("upload %s failed: %v", j.remote, err)
			a.emitBatchError(fmt.Sprintf("%s: %v", j.remote, err))
			return fmt.Errorf("upload %s: %w", j.remote, err)
		}
		bytesDoneBefore += res.BytesSent
		md5 := res.MD5Match
		a.emitBatchProgress("uploading", j.remote,
			int(res.BytesSent), int(res.BytesSent),
			idx+1, len(jobs), bytesDoneBefore, totalBytes, res.SpeedKBs, &md5)
		md5tag := ""
		if res.MD5Match {
			md5tag = " ✓ MD5"
		}
		a.echoOK("  uploaded %s (%.1f KB/s)%s", j.remote, res.SpeedKBs, md5tag)
	}

	a.emitBatchProgress("done", "", int(totalBytes), int(totalBytes),
		len(jobs), len(jobs), totalBytes, totalBytes, 0, nil)
	a.echoOK("batch complete: %d file(s), %s", len(jobs), humanBytes(uint64(totalBytes)))
	return nil
}

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
	wailsRT.EventsEmit(a.ctx, "fs:progress", map[string]any{"phase": "error", "error": msg})
}
