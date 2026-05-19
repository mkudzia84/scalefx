package hubfx

// File-system commands: list / mkdir / delete / info / tree / cat / download
// / upload / upload-batch / cancel for both SD and on-board flash. Helpers
// for path joining and human-readable byte sizes live here too.

import (
	"fmt"
	"os"
	"path/filepath"
	"scalefx/api"
	"scalefx/engine"
	"scalefx/protocol"
	hfxp "scalefx/protocol/hubfx"
	"strings"
	"time"
)

func (h *Handler) cmdFileList(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	text, err := h.E.API.Files.List(target, path)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}
	h.FormatListing(text, fmt.Sprintf("%s:%s", StorageTargetName(target), path))
}

func (h *Handler) cmdFileDelete(args []string) {
	recursive := false
	filtered := make([]string, 0, len(args))
	for _, a := range args {
		if a == "-r" || a == "-R" || a == "--recursive" {
			recursive = true
			continue
		}
		filtered = append(filtered, a)
	}
	target, path := h.parseStorageArgs(filtered, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.delete [-r] <sd|flash> <path>")
		return
	}
	label := fmt.Sprintf("Delete %s:%s", StorageTargetName(target), path)
	if recursive {
		label += " (recursive)"
	}
	h.E.Ack(h.E.API.Files.Delete(target, path, recursive), label)
}

func (h *Handler) cmdFileMkdir(args []string) {
	parents := false
	filtered := make([]string, 0, len(args))
	for _, a := range args {
		if a == "-p" || a == "--parents" {
			parents = true
			continue
		}
		filtered = append(filtered, a)
	}
	target, path := h.parseStorageArgs(filtered, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.mkdir [-p] <sd|flash> <path>")
		return
	}
	label := fmt.Sprintf("Mkdir %s:%s", StorageTargetName(target), path)
	if parents {
		label += " (parents)"
	}
	h.E.Ack(h.E.API.Files.Mkdir(target, path, parents), label)
}

func (h *Handler) cmdFileInfo(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.info <sd|flash> <path>")
		return
	}
	r := h.E.API.Files.Info(target, path)
	if !r.OK {
		h.E.Out.Error("%s", r.Error)
		return
	}
	if len(r.Response.Payload) >= 6 {
		exists := r.Response.Payload[0]
		isDir := r.Response.Payload[1]
		size := protocol.ReadU32LE(r.Response.Payload, 2)
		display := fmt.Sprintf("%s:%s", StorageTargetName(target), path)
		h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, display))
		if exists != 0 {
			kind := "file"
			if isDir != 0 {
				kind = "directory"
			}
			h.E.Out.Printf("    Type: %s\n", kind)
			if isDir == 0 {
				h.E.Out.Printf("    Size: %s (%d bytes)\n", FormatSize(size), size)
			}
		} else {
			h.E.Out.Printf("    %s\n", h.E.Out.C(engine.ColorRed, "Not found"))
		}
		h.E.Out.Println()
	} else {
		h.E.Out.Error("Response too short")
	}
}

func (h *Handler) cmdFileTree(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "/")
	if target == 255 {
		return
	}
	text, err := h.E.API.Files.Tree(target, path)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}
	h.RenderTree(text, fmt.Sprintf("%s:%s", StorageTargetName(target), path))
}

func (h *Handler) cmdFileCat(args []string) {
	if !h.E.RequireConn() {
		return
	}
	target, path := h.parseStorageArgs(args, "")
	if target == 255 || path == "" {
		h.E.Out.Error("Usage: file.cat <sd|flash> <path>")
		return
	}
	h.E.Out.Info("Reading %s:%s ...", StorageTargetName(target), path)
	text, err := h.E.API.Files.Cat(target, path)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}
	h.E.Out.Println()
	h.E.Out.Printf("%s", text)
	h.E.Out.Printf("\n    (%d bytes)\n", len(text))
}

func (h *Handler) cmdFileDownload(args []string) {
	if !h.E.RequireConn() {
		return
	}
	if len(args) < 3 {
		h.E.Out.Error("Usage: file.download <sd|flash> <remote> <local>")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hfxp.StorageTargetSd
	case "flash":
		target = hfxp.StorageTargetFlash
	default:
		h.E.Out.Error("Storage target must be 'sd' or 'flash'")
		return
	}
	remotePath := args[1]
	localPath := args[2]

	h.E.Out.Info("Downloading %s:%s ...", StorageTargetName(target), remotePath)
	result, err := h.E.API.Files.Download(target, remotePath, 60*time.Second)
	if err != nil {
		h.E.Out.Error("%v", err)
		return
	}

	if dir := filepath.Dir(localPath); dir != "" && dir != "." {
		os.MkdirAll(dir, 0755)
	}
	if err := os.WriteFile(localPath, result.Data, 0644); err != nil {
		h.E.Out.Error("Failed to write local file: %v", err)
		return
	}
	h.E.Out.OK("Downloaded %d bytes → %s", len(result.Data), localPath)
}

func (h *Handler) cmdFileUpload(args []string) {
	if !h.E.RequireConn() {
		return
	}
	if len(args) < 3 {
		h.E.Out.Error("Usage: file.upload <sd|flash> <local> <remote> [--stream]")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hfxp.StorageTargetSd
	case "flash":
		target = hfxp.StorageTargetFlash
	default:
		h.E.Out.Error("Storage target must be 'sd' or 'flash'")
		return
	}
	localPath := args[1]
	remotePath := args[2]

	mode := api.UploadSync
	for _, a := range args[3:] {
		switch strings.ToLower(a) {
		case "--stream":
			mode = api.UploadStream
		}
	}

	fileData, err := os.ReadFile(localPath)
	if err != nil {
		h.E.Out.Error("Cannot read local file: %v", err)
		return
	}

	fileSize := len(fileData)
	modeName := "sync"
	switch mode {
	case api.UploadStream:
		modeName = "stream"
	}
	h.E.Out.Info("Uploading %s (%d bytes) → %s:%s [%s]", localPath, fileSize,
		StorageTargetName(target), remotePath, modeName)

	uploadStart := time.Now()
	result := h.E.API.Files.Upload(target, remotePath, fileData, mode,
		func(sent, total int) {
			h.E.Out.Printf("\r%s  ", engine.FormatProgressBar(sent, total, uploadStart, 30))
		})
	h.E.Out.Println()

	if !result.OK {
		h.E.Out.Error("Upload failed: %s", result.Error)
		return
	}

	h.E.Out.OK("Uploaded %d bytes in %.1fs (%.1f KB/s)", result.BytesTransferred,
		result.Elapsed.Seconds(), result.SpeedKBs)

	if result.LocalMD5 != "" {
		if result.MD5Match {
			h.E.Out.OK("MD5 verified: %s", result.RemoteMD5)
		} else {
			h.E.Out.Error("MD5 MISMATCH! local=%s remote=%s", result.LocalMD5, result.RemoteMD5)
		}
	}
}

func (h *Handler) cmdFileCancel(_ []string) {
	h.E.Ack(h.E.API.Files.CancelUpload(), "Upload cancelled")
}

// cmdFileUploadBatch uploads multiple files / directory trees under a remote
// cwd, preserving directory structure (so dropping "a/b/log.txt" while
// remote-cwd is /tmp lands the file at /tmp/a/b/log.txt). Mirrors the
// Studio FsUploadBatch behavior so CLI and GUI upload identically.
func (h *Handler) cmdFileUploadBatch(args []string) {
	if !h.E.RequireConn() {
		return
	}
	if len(args) < 3 {
		h.E.Out.Error("Usage: file.upload-batch <sd|flash> <remote-cwd> <local1> [local2 ...] [--stream]")
		return
	}
	target := byte(255)
	switch strings.ToLower(args[0]) {
	case "sd":
		target = hfxp.StorageTargetSd
	case "flash":
		target = hfxp.StorageTargetFlash
	default:
		h.E.Out.Error("Storage target must be 'sd' or 'flash'")
		return
	}
	remoteCwd := args[1]

	mode := api.UploadSync
	modeName := "sync"
	locals := make([]string, 0, len(args)-2)
	for _, a := range args[2:] {
		if strings.EqualFold(a, "--stream") {
			mode = api.UploadStream
			modeName = "stream"
			continue
		}
		locals = append(locals, a)
	}
	if len(locals) == 0 {
		h.E.Out.Error("No local paths provided")
		return
	}

	type job struct {
		local, remote string
		size          int64
	}
	var jobs []job
	dirSeen := map[string]bool{}
	var dirs []string
	var total int64

	// addDir records a directory we'll create via mkdir -p (idempotent, and
	// the firmware handles all ancestors). We dedupe to skip redundant calls.
	addDir := func(d string) {
		d = normalizeSlash(d)
		if d == "" || d == "/" || dirSeen[d] {
			return
		}
		dirSeen[d] = true
		dirs = append(dirs, d)
	}

	for _, lp := range locals {
		info, err := os.Stat(lp)
		if err != nil {
			h.E.Out.Error("stat %s: %v", lp, err)
			return
		}
		base := filepath.Base(lp)
		if info.IsDir() {
			root := joinSlash(remoteCwd, base)
			addDir(root)
			if err := filepath.Walk(lp, func(p string, fi os.FileInfo, werr error) error {
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
				remote := joinSlash(root, rel)
				if fi.IsDir() {
					addDir(remote)
					return nil
				}
				jobs = append(jobs, job{local: p, remote: remote, size: fi.Size()})
				total += fi.Size()
				return nil
			}); err != nil {
				h.E.Out.Error("walk %s: %v", lp, err)
				return
			}
		} else {
			jobs = append(jobs, job{local: lp, remote: joinSlash(remoteCwd, base), size: info.Size()})
			total += info.Size()
			// Top-level file → ensure its parent (remoteCwd) exists.
			addDir(remoteCwd)
		}
	}
	if len(jobs) == 0 {
		h.E.Out.Error("no files found in input paths")
		return
	}

	h.E.Out.Info("Batch upload → %s:%s — %d file(s), %s, mode=%s",
		StorageTargetName(target), remoteCwd, len(jobs), humanBytes(uint64(total)), modeName)

	// Create remote directories via mkdir -p (firmware resolves ancestors;
	// order no longer matters — the call is idempotent on existing dirs).
	for _, d := range dirs {
		res := h.E.API.Files.Mkdir(target, d, true)
		if !res.OK {
			h.E.Out.Error("mkdir -p %s failed: %s", d, res.Error)
			return
		}
	}

	batchStart := time.Now()
	var bytesDone int64
	var filesOk, filesFail int
	for i, j := range jobs {
		data, err := os.ReadFile(j.local)
		if err != nil {
			h.E.Out.Error("read %s: %v", j.local, err)
			filesFail++
			continue
		}
		h.E.Out.Info("[%d/%d] %s → %s (%s)",
			i+1, len(jobs), j.local, j.remote, humanBytes(uint64(len(data))))
		fileStart := time.Now()
		res := h.E.API.Files.Upload(target, j.remote, data, mode, func(sent, totalBytes int) {
			h.E.Out.Printf("\r%s  ",
				engine.FormatProgressBar(sent, totalBytes, fileStart, 30))
		})
		h.E.Out.Println()
		if !res.OK {
			h.E.Out.Error("  upload failed: %s", res.Error)
			filesFail++
			continue
		}
		filesOk++
		bytesDone += int64(res.BytesTransferred)
		md5tag := ""
		if res.LocalMD5 != "" {
			if res.MD5Match {
				md5tag = " ✓ MD5"
			} else {
				md5tag = " ⚠ MD5 MISMATCH"
			}
		}
		h.E.Out.OK("  %d bytes in %.1fs (%.1f KB/s)%s",
			res.BytesTransferred, res.Elapsed.Seconds(), res.SpeedKBs, md5tag)
	}

	batchElapsed := time.Since(batchStart).Seconds()
	avgKBs := 0.0
	if batchElapsed > 0 {
		avgKBs = float64(bytesDone) / 1024.0 / batchElapsed
	}
	h.E.Out.OK("Batch done: %d ok / %d fail · %s in %.1fs (avg %.1f KB/s)",
		filesOk, filesFail, humanBytes(uint64(bytesDone)), batchElapsed, avgKBs)
}

func joinSlash(dir, name string) string {
	name = strings.TrimLeft(filepath.ToSlash(name), "/")
	if dir == "" || dir == "/" {
		return "/" + name
	}
	return strings.TrimRight(filepath.ToSlash(dir), "/") + "/" + name
}

func normalizeSlash(p string) string {
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
