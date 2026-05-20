package main

// Linux-flavoured file-system commands.  The CLI tracks a per-target
// CWD (current working directory) so `ls`, `cat`, `rm`, etc. work
// with relative paths just like a normal shell.  `target` switches
// the active backend (SD ↔ flash); the CWD is preserved per target so
// flipping back doesn't lose your place.
//
// Mapping to the underlying wire surface:
//
//   ls / dir            FILE_LIST
//   tree                FILE_TREE          (recursive)
//   cat                 FILE_DOWNLOAD      (small files only)
//   stat                FILE_INFO
//   rm / rm -r          FILE_DELETE
//   mkdir / mkdir -p    FILE_MKDIR
//   pwd / cd            (local CWD only)
//   target              switch SD ↔ flash
//   df / disk           FLASH_STATUS_REQ + SD_STATUS_REQ
//   sd-init             SD_INIT
//   upload / put        FILE_UPLOAD_BEGIN + FILE_UPLOAD_DATA + END
//   download / get      FILE_DOWNLOAD
//
// All Linux-style commands accept relative paths (resolved against the
// active CWD) or absolute paths starting with "/".  `--flash` (or `-f`)
// on any path command forces the flash backend for one call without
// flipping the global target.

import (
	"encoding/hex"
	"fmt"
	"os"
	"path"
	"strings"
	"time"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/protocol/storage"
)

func init() {
	register(&command{Name: "pwd", Usage: "pwd", Help: "print the active target + working directory", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdPwd})
	register(&command{Name: "cd", Usage: "cd [path]", Help: "change the working directory (no arg = /)", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdCd})
	register(&command{Name: "target", Usage: "target [sd|flash]", Help: "switch active backend (no arg = print current)", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdTarget})

	register(&command{Name: "ls", Usage: "ls [-f] [path]", Help: "list a directory (-f forces flash)", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdLs})
	register(&command{Name: "tree", Usage: "tree [-f] [path]", Help: "recursive listing of a directory", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdTree})
	register(&command{Name: "stat", Usage: "stat [-f] <path>", Help: "metadata for a path", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdStat})
	register(&command{Name: "cat", Usage: "cat [-f] <path>", Help: "print the contents of a small text file", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdCat})

	register(&command{Name: "rm", Usage: "rm [-r] [-f] <path>", Help: "remove a file (-r recursive)", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdRm})
	register(&command{Name: "mkdir", Usage: "mkdir [-p] [-f] <path>", Help: "create a directory (-p makes parents)", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdMkdir})

	register(&command{Name: "df", Usage: "df", Help: "disk usage for flash + SD", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdDf})
	register(&command{Name: "sd-init", Usage: "sd-init [speed_mhz]", Help: "(re-)initialise the SD card driver", Category: catStorage, RequiresConn: true, RequiresCap: core.CapSd, Run: cmdSdInit})

	register(&command{Name: "upload", Usage: "upload [-m sync|stream] [-f] <local> [remote]", Help: "upload a local file (default mode = stream on ESP32)", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdUpload})
	register(&command{Name: "download", Usage: "download [-f] <remote> [local]", Help: "download a remote file", Category: catStorage, RequiresConn: true, RequiresCap: core.CapFlash | core.CapSd, Run: cmdDownload})

	aliasFor("dir", "ls")
	aliasFor("put", "upload")
	aliasFor("get", "download")
	aliasFor("delete", "rm") // legacy
}

// ─── CWD + target plumbing ───────────────────────────────────────────

// fsArgs parses the trailing `[-f] [path]` form used by every Linux-
// style command.  Returns (target, absolutePath, extraArgsRemaining).
// `forceFlash`, `recursive`, `parents` are taken from the leading
// short-flag block.
type fsArgs struct {
	flags  map[byte]bool // 'f', 'r', 'p'
	path   string        // absolute (resolved against CWD if relative)
	target byte          // TargetSD / TargetFlash
}

// parseFsArgs strips short flags, resolves a single path arg against
// the active CWD, and returns the resolved struct.  `accept` is the
// set of allowed short flags ('f', 'r', 'p').  Pass an empty path
// (`needPath=false`) to make the path arg optional (e.g. `ls` with no
// args defaults to ".").
func (a *app) parseFsArgs(args []string, accept string, needPath bool) (fsArgs, error) {
	out := fsArgs{flags: map[byte]bool{}, target: a.activeTarget()}
	rest := args
	for len(rest) > 0 && strings.HasPrefix(rest[0], "-") && rest[0] != "-" {
		for i := 1; i < len(rest[0]); i++ {
			c := rest[0][i]
			if !strings.ContainsRune(accept, rune(c)) {
				return out, fmt.Errorf("unknown flag -%c", c)
			}
			out.flags[c] = true
		}
		rest = rest[1:]
	}
	if out.flags['f'] {
		out.target = storage.TargetFlash
	}
	switch {
	case len(rest) == 0 && needPath:
		return out, fmt.Errorf("missing path argument")
	case len(rest) == 0:
		out.path = a.cwd[out.target]
	default:
		out.path = a.resolvePath(rest[0], out.target)
	}
	return out, nil
}

// resolvePath joins `p` against the CWD for `target` if it's relative.
// Cleans up "." / ".." segments via path.Clean.
func (a *app) resolvePath(p string, target byte) string {
	if p == "" {
		return a.cwd[target]
	}
	if !strings.HasPrefix(p, "/") {
		p = path.Join(a.cwd[target], p)
	}
	cleaned := path.Clean(p)
	if cleaned == "." {
		cleaned = "/"
	}
	if !strings.HasPrefix(cleaned, "/") {
		cleaned = "/" + cleaned
	}
	return cleaned
}

// activeTarget returns the currently-selected backend, defaulting to
// SD before the user has chosen one.
func (a *app) activeTarget() byte {
	if a.cwd == nil {
		a.cwd = map[byte]string{
			storage.TargetSD:    "/",
			storage.TargetFlash: "/",
		}
	}
	return a.target
}

// ─── pwd / cd / target ───────────────────────────────────────────────

func cmdPwd(a *app, _ []string) error {
	t := a.activeTarget()
	fmt.Printf("  %s  %s\n", targetName(t), cBold(a.cwd[t]))
	return nil
}

func cmdCd(a *app, args []string) error {
	t := a.activeTarget()
	if len(args) == 0 {
		a.cwd[t] = "/"
		Ok("cd /  (%s)", targetName(t))
		return nil
	}
	dest := a.resolvePath(args[0], t)

	// Validate via FILE_INFO — refuse to cd into a file or non-existent path.
	info, err := a.c.Storage.FileInfo(dest)
	if err != nil {
		return err
	}
	if !info.Exists {
		return fmt.Errorf("cd: %s: no such file or directory", dest)
	}
	if !info.IsDir {
		return fmt.Errorf("cd: %s: not a directory", dest)
	}
	a.cwd[t] = dest
	Ok("cd %s  (%s)", cBold(dest), targetName(t))
	return nil
}

func cmdTarget(a *app, args []string) error {
	_ = a.activeTarget() // ensure CWD map exists
	if len(args) == 0 {
		Hdr("storage targets")
		mark := cDim("·")
		for _, t := range []byte{storage.TargetSD, storage.TargetFlash} {
			cur := cDim("·")
			if t == a.target {
				cur = cGreen("●")
			}
			fmt.Printf("  %s  %s  cwd=%s\n", cur, targetName(t), cBold(a.cwd[t]))
		}
		_ = mark
		return nil
	}
	t, err := parseTarget(args[0])
	if err != nil {
		return err
	}
	a.target = t
	Ok("target → %s  cwd=%s", targetName(t), cBold(a.cwd[t]))
	return nil
}

// ─── ls / tree / stat / cat ──────────────────────────────────────────

func cmdLs(a *app, args []string) error {
	fs, err := a.parseFsArgs(args, "f", false)
	if err != nil {
		return err
	}
	out, err := a.c.Storage.FileList(fs.path, fs.target)
	if err != nil {
		return err
	}
	if out == "" {
		Note("(empty)")
		return nil
	}
	Hdr(fmt.Sprintf("%s  %s", targetName(fs.target), cBold(fs.path)))
	for _, line := range splitNonEmpty(out, "\n") {
		printFileRow(line)
	}
	return nil
}

func cmdTree(a *app, args []string) error {
	fs, err := a.parseFsArgs(args, "f", false)
	if err != nil {
		return err
	}
	out, err := a.c.Storage.FileTree(fs.path, fs.target)
	if err != nil {
		return err
	}
	if out == "" {
		Note("(empty)")
		return nil
	}
	Hdr(fmt.Sprintf("%s  %s", targetName(fs.target), cBold(fs.path)))
	for _, line := range splitNonEmpty(out, "\n") {
		printTreeRow(line)
	}
	return nil
}

func cmdStat(a *app, args []string) error {
	fs, err := a.parseFsArgs(args, "f", true)
	if err != nil {
		return err
	}
	info, err := a.c.Storage.FileInfo(fs.path)
	if err != nil {
		return err
	}
	if !info.Exists {
		Note("(does not exist)")
		return nil
	}
	Hdr(fmt.Sprintf("%s  %s", targetName(fs.target), cBold(fs.path)))
	if info.IsDir {
		KV("kind", cCyan("directory"))
	} else {
		KV("kind", "file")
	}
	KV("size", humanBytes(uint64(info.Size)))
	return nil
}

func cmdCat(a *app, args []string) error {
	fs, err := a.parseFsArgs(args, "f", true)
	if err != nil {
		return err
	}
	// Refuse pathological catches — protect the terminal.
	info, ierr := a.c.Storage.FileInfo(fs.path)
	if ierr == nil && info.Exists && info.Size > 64*1024 {
		return fmt.Errorf("cat: %s: file is %s; use `download` for large files",
			fs.path, humanBytes(uint64(info.Size)))
	}
	res, err := a.c.Storage.FileDownload(fs.path, 0)
	if err != nil {
		return err
	}
	if len(res.Data) == 0 {
		Note("(empty file)")
		return nil
	}
	os.Stdout.Write(res.Data)
	if res.Data[len(res.Data)-1] != '\n' {
		fmt.Println()
	}
	return nil
}

// ─── rm / mkdir ──────────────────────────────────────────────────────

func cmdRm(a *app, args []string) error {
	fs, err := a.parseFsArgs(args, "fr", true)
	if err != nil {
		return err
	}
	flags := storage.DeleteFlagNone
	if fs.flags['r'] {
		flags |= storage.DeleteFlagRecursive
	}
	if err := a.c.Storage.FileDelete(fs.path, fs.target, flags); err != nil {
		return err
	}
	Ok("removed %s  (%s)", cBold(fs.path), targetName(fs.target))
	return nil
}

func cmdMkdir(a *app, args []string) error {
	fs, err := a.parseFsArgs(args, "fp", true)
	if err != nil {
		return err
	}
	flags := storage.MkdirFlagNone
	if fs.flags['p'] {
		flags |= storage.MkdirFlagParents
	}
	if err := a.c.Storage.FileMkdir(fs.path, fs.target, flags); err != nil {
		return err
	}
	Ok("created %s  (%s)", cBold(fs.path), targetName(fs.target))
	return nil
}

// ─── df / sd-init ────────────────────────────────────────────────────

func cmdDf(a *app, _ []string) error {
	Hdr("disk usage")
	if a.boardCaps&core.CapFlash != 0 {
		if s, err := a.c.Storage.FlashStatus(); err == nil {
			pct := uint64(0)
			if s.TotalBytes > 0 {
				pct = uint64(s.UsedBytes) * 100 / uint64(s.TotalBytes)
			}
			fmt.Printf("  %s  %s used / %s total  (%d%%)  free=%s\n",
				targetName(storage.TargetFlash),
				humanBytes(uint64(s.UsedBytes)),
				humanBytes(uint64(s.TotalBytes)),
				pct,
				humanBytes(uint64(s.FreeBytes)))
		}
	}
	if a.boardCaps&core.CapSd != 0 {
		if s, err := a.c.Storage.SdStatus(); err == nil && s.Initialized {
			pct := uint64(0)
			if s.TotalSpaceMB > 0 {
				pct = uint64(s.UsedSpaceMB) * 100 / uint64(s.TotalSpaceMB)
			}
			fmt.Printf("  %s  %s used / %s total  (%d%%)  free=%s  type=%s/%s\n",
				targetName(storage.TargetSD),
				humanBytes(uint64(s.UsedSpaceMB)*1024*1024),
				humanBytes(uint64(s.TotalSpaceMB)*1024*1024),
				pct,
				humanBytes(uint64(s.FreeSpaceMB)*1024*1024),
				cCyan(sdCardTypeName(s.CardType)),
				cCyan(sdBusModeName(s.BusMode)))
		} else if a.boardCaps&core.CapSd != 0 {
			fmt.Printf("  %s  %s\n", targetName(storage.TargetSD), cDim("(not mounted)"))
		}
	}
	return nil
}

func cmdSdInit(a *app, args []string) error {
	speed := byte(0)
	if len(args) >= 1 {
		v, err := parseU8(args[0])
		if err != nil {
			return err
		}
		speed = v
	}
	if err := a.c.Storage.SdInit(speed); err != nil {
		return err
	}
	Ok("SD card driver re-initialised")
	return nil
}

// ─── upload / download with progress ─────────────────────────────────

// uploadArgs is the parsed form of:
//
//	upload [-m sync|stream] [-f] <local> [remote]
type uploadArgs struct {
	local  string
	remote string
	mode   byte
	target byte
}

func (a *app) parseUploadArgs(args []string) (uploadArgs, error) {
	out := uploadArgs{target: a.activeTarget(), mode: defaultUploadMode(a)}
	positional := []string{}
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-m", "--mode":
			if i+1 >= len(args) {
				return out, fmt.Errorf("-m needs a value (sync|stream)")
			}
			i++
			switch strings.ToLower(args[i]) {
			case "sync":
				out.mode = storage.UploadSync
			case "stream", "batch":
				out.mode = storage.UploadStream
			default:
				return out, fmt.Errorf("mode must be sync|stream, got %q", args[i])
			}
		case "-f", "--flash":
			out.target = storage.TargetFlash
		case "-s", "--sd":
			out.target = storage.TargetSD
		default:
			positional = append(positional, args[i])
		}
	}
	if len(positional) == 0 {
		return out, fmt.Errorf("usage: upload [-m sync|stream] [-f] <local> [remote]")
	}
	out.local = positional[0]
	if len(positional) >= 2 {
		out.remote = a.resolvePath(positional[1], out.target)
	} else {
		// Default remote = CWD + basename(local)
		base := path.Base(strings.ReplaceAll(out.local, "\\", "/"))
		out.remote = path.Join(a.cwd[out.target], base)
	}
	return out, nil
}

// defaultUploadMode picks `stream` on ESP32 peers (the high-throughput
// path the firmware has tuned its 64 KB PSRAM fill buffer for) and
// `sync` on Pico peers (no PSRAM, would starve the SD writer).
func defaultUploadMode(a *app) byte {
	if a.c == nil {
		return storage.UploadSync
	}
	// Peer max payload is set during connect from IDENTIFY.Platform.
	// ESP32 → 2048, Pico → 512.  Stream is a no-op cost on either; use
	// it whenever the firmware advertises FLASH or SD on ESP32.
	if strings.Contains(strings.ToLower(a.boardName), "fx-") &&
		strings.Contains(strings.ToLower(a.boardKind), "hubfx") {
		return storage.UploadStream
	}
	return storage.UploadSync
}

func cmdUpload(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	up, err := a.parseUploadArgs(args)
	if err != nil {
		return err
	}
	stat, err := os.Stat(up.local)
	if err != nil {
		return err
	}
	total := stat.Size()

	Info("uploading %s  →  %s:%s  (%s, mode=%s)",
		cBold(up.local),
		targetName(up.target), cBold(up.remote),
		humanBytes(uint64(total)),
		cCyan(uploadModeName(up.mode)))

	start := time.Now()
	res, err := a.c.Storage.FileUpload(up.local, client.UploadOptions{
		Path:   up.remote,
		Target: up.target,
		Mode:   up.mode,
		OnProgress: func(sent, total int64) {
			fmt.Printf("\r  %s ", ProgressBar(sent, total, start, 30))
		},
	})
	fmt.Println()
	if err != nil {
		return err
	}

	Ok("uploaded %s in %s  (%s)",
		humanBytes(uint64(res.BytesSent)),
		humanDurationSec(res.Elapsed),
		cCyan(fmt.Sprintf("%.1f KB/s", res.SpeedKBs)))
	KV("local MD5", cDim(hex.EncodeToString(res.LocalMD5[:])))
	if res.RemoteMD5 != ([16]byte{}) {
		if res.MD5Match {
			KV("remote MD5", cGreen(hex.EncodeToString(res.RemoteMD5[:]))+" "+cGreen("✓ match"))
		} else {
			KV("remote MD5", cRed(hex.EncodeToString(res.RemoteMD5[:]))+" "+cRed("✗ MISMATCH"))
		}
	}
	return nil
}

func cmdDownload(a *app, args []string) error {
	if err := a.requireClient(); err != nil {
		return err
	}
	// Reuse upload's flag parser shape: [-f] <remote> [local]
	target := a.activeTarget()
	positional := []string{}
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-f", "--flash":
			target = storage.TargetFlash
		case "-s", "--sd":
			target = storage.TargetSD
		default:
			positional = append(positional, args[i])
		}
	}
	if len(positional) == 0 {
		return fmt.Errorf("usage: download [-f] <remote> [local]")
	}
	remote := a.resolvePath(positional[0], target)
	local := positional[0]
	if len(positional) >= 2 {
		local = positional[1]
	} else {
		local = path.Base(remote)
	}
	_ = target // currently FileDownload doesn't switch target; SD is implicit

	Info("downloading %s:%s  →  %s", targetName(target), cBold(remote), cBold(local))

	start := time.Now()
	res, err := a.c.Storage.FileDownload(remote, 0)
	if err != nil {
		return err
	}
	if err := os.WriteFile(local, res.Data, 0o644); err != nil {
		return err
	}
	elapsed := time.Since(start)
	rate := float64(len(res.Data)) / elapsed.Seconds() / 1024.0
	Ok("downloaded %s in %s  (%s)",
		humanBytes(uint64(len(res.Data))),
		humanDurationSec(elapsed),
		cCyan(fmt.Sprintf("%.1f KB/s", rate)))
	KV("md5", cDim(hex.EncodeToString(res.MD5[:])))
	return nil
}

// ─── Helpers ─────────────────────────────────────────────────────────

func uploadModeName(m byte) string {
	switch m {
	case storage.UploadSync:
		return "sync"
	case storage.UploadStream:
		return "stream"
	}
	return fmt.Sprintf("0x%02X", m)
}

func humanDurationSec(d time.Duration) string {
	s := d.Seconds()
	switch {
	case s < 1:
		return fmt.Sprintf("%d ms", d.Milliseconds())
	case s < 60:
		return fmt.Sprintf("%.2f s", s)
	default:
		return fmt.Sprintf("%dm %02ds", int(s)/60, int(s)%60)
	}
}

// firmware's listDirectory emits one row per entry:
//
//	"d\tname\t0"   for directories
//	"f\tname\tsz"  for files
func printFileRow(line string) {
	parts := strings.SplitN(line, "\t", 3)
	if len(parts) < 3 {
		fmt.Println(line)
		return
	}
	kind, name, size := parts[0], parts[1], parts[2]
	switch kind {
	case "d":
		fmt.Printf("  %s  %s\n", cCyan("d"), cBold(name)+cDim("/"))
	case "f":
		fmt.Printf("  %s  %s  %s\n", cDim("f"),
			padRight(name, 32),
			cDim(size))
	default:
		fmt.Println(line)
	}
}

// firmware's recursive tree emits one row per entry with a leading
// depth field:
//
//	"depth\tkind\tname\tsize"
//
// We render it as an indented tree.
func printTreeRow(line string) {
	parts := strings.SplitN(line, "\t", 4)
	if len(parts) < 4 {
		fmt.Println(line)
		return
	}
	depth, kind, name, size := parts[0], parts[1], parts[2], parts[3]
	d := 0
	fmt.Sscanf(depth, "%d", &d)
	indent := strings.Repeat("  ", d)
	switch kind {
	case "d":
		fmt.Printf("  %s%s  %s\n", indent, cCyan("d"), cBold(name)+cDim("/"))
	case "f":
		fmt.Printf("  %s%s  %s  %s\n", indent, cDim("f"),
			padRight(name, 32-d*2),
			cDim(size))
	default:
		fmt.Println(line)
	}
}

func splitNonEmpty(s, sep string) []string {
	var out []string
	for _, p := range strings.Split(s, sep) {
		if p == "" {
			continue
		}
		out = append(out, p)
	}
	return out
}

func sdCardTypeName(t byte) string {
	switch t {
	case 1:
		return "MMC"
	case 2:
		return "SD"
	case 3:
		return "SDHC"
	}
	return fmt.Sprintf("type%d", t)
}

func sdBusModeName(b byte) string {
	switch b {
	case 0:
		return "SPI"
	case 1:
		return "SDIO-1bit"
	case 2:
		return "SDIO-4bit"
	}
	return fmt.Sprintf("bus%d", b)
}

func targetName(t byte) string {
	switch t {
	case storage.TargetSD:
		return cCyan("sd")
	case storage.TargetFlash:
		return cCyan("flash")
	}
	return cDim(fmt.Sprintf("target%d", t))
}
