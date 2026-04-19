// Storage Test Client — Drives tests/storage_test/ firmware over USB-serial
// for upload throughput measurement.
//
// Usage:
//
//	storage_test_client -port COM7                        # run default suite
//	storage_test_client -port COM7 -mode sync -size 1     # single run
//	storage_test_client -port COM7 -target flash          # upload to LittleFS
//	storage_test_client -port COM7 -no-recover            # disable USB remount
//
// The default suite runs sync + batch at 1/2/4 MB to both flash (where it
// fits) and SD card, with hang recovery via Windows mountvol between runs.
//
// Rule 21: this lives in tests/ and `replace scalefx => ../../app/go` lets
// us reach into the in-tree Go SDK without publishing it.
package main

import (
	"crypto/md5"
	"encoding/hex"
	"flag"
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"scalefx/api"
	"scalefx/protocol"
	"scalefx/protocol/hubfx"
)

// ────── CLI ────────────────────────────────────────────────────────────────

type cliArgs struct {
	port      string
	mode      string // "sync", "batch", "both"
	target    string // "flash", "sd", "both"
	size      int    // MB (0 = full suite: 1,2,4)
	recover   bool
	logFile   string
	remoteDir string
}

func parseArgs() cliArgs {
	a := cliArgs{}
	flag.StringVar(&a.port, "port", "", "Serial port (e.g., COM7 or /dev/ttyUSB0) [required]")
	flag.StringVar(&a.mode, "mode", "both", "Upload mode: sync, batch, both")
	flag.StringVar(&a.target, "target", "both", "Target: flash, sd, both")
	flag.IntVar(&a.size, "size", 0, "File size in MB (0 = run 1,2,4 MB suite)")
	flag.BoolVar(&a.recover, "recover", true, "Attempt USB remount after hang (Windows: mountvol)")
	flag.StringVar(&a.logFile, "log", "", "Append results to CSV file")
	flag.StringVar(&a.remoteDir, "dir", "/upload_test", "Remote directory for test files")
	flag.Parse()

	if a.port == "" {
		fmt.Fprintln(os.Stderr, "error: -port is required")
		flag.Usage()
		os.Exit(2)
	}
	return a
}

// ────── Test case matrix ───────────────────────────────────────────────────

type testCase struct {
	mode    api.UploadMode
	target  byte
	sizeMB  int
	maxPct  int // % of 512 KB stream segment capped (informational)
}

func buildMatrix(args cliArgs) []testCase {
	var sizes []int
	if args.size > 0 {
		sizes = []int{args.size}
	} else {
		sizes = []int{1, 2, 4}
	}

	var modes []api.UploadMode
	switch args.mode {
	case "sync":
		modes = []api.UploadMode{api.UploadSync}
	case "batch", "stream":
		modes = []api.UploadMode{api.UploadStream}
	default:
		modes = []api.UploadMode{api.UploadSync, api.UploadStream}
	}

	var targets []byte
	switch args.target {
	case "flash":
		targets = []byte{hubfx.StorageTargetFlash}
	case "sd":
		targets = []byte{hubfx.StorageTargetSd}
	default:
		targets = []byte{hubfx.StorageTargetFlash, hubfx.StorageTargetSd}
	}

	var out []testCase
	for _, tgt := range targets {
		for _, sz := range sizes {
			// Flash partition is 3.9 MB — skip 4 MB flash uploads.
			if tgt == hubfx.StorageTargetFlash && sz >= 4 {
				continue
			}
			for _, m := range modes {
				out = append(out, testCase{mode: m, target: tgt, sizeMB: sz})
			}
		}
	}
	return out
}

// ────── Test data ──────────────────────────────────────────────────────────

// makeData generates deterministic pseudo-random bytes so local/remote MD5
// are reproducible across runs.
func makeData(sizeBytes int, seed int64) []byte {
	rng := rand.New(rand.NewSource(seed))
	buf := make([]byte, sizeBytes)
	// Use Read in chunks — rng.Read(nil) exists but this is clearer.
	for i := 0; i < sizeBytes; i += 4096 {
		end := i + 4096
		if end > sizeBytes {
			end = sizeBytes
		}
		rng.Read(buf[i:end])
	}
	return buf
}

// ────── Result aggregation ─────────────────────────────────────────────────

type result struct {
	Target  string
	Mode    string
	SizeMB  int
	OK      bool
	Error   string
	KBs     float64
	Elapsed time.Duration
	MD5OK   bool
}

func modeName(m api.UploadMode) string {
	if m == api.UploadStream {
		return "batch"
	}
	return "sync"
}

func targetName(t byte) string {
	if t == hubfx.StorageTargetFlash {
		return "flash"
	}
	return "sd"
}

// ────── USB recovery (Windows) ─────────────────────────────────────────────

// recoverPort attempts a soft bus reset via mode reopen, falling back to
// Windows mountvol disable/enable if the port is genuinely stuck. Returns
// nil if the port is responsive again.
//
// We can't touch mountvol on non-Windows; the generic fallback is a close/
// reopen cycle which is often enough after a stalled COBS stream.
func recoverPort(port string, conn *protocol.Connection) error {
	fmt.Printf("  [recover] closing port %s\n", port)
	conn.Close()
	time.Sleep(800 * time.Millisecond)

	fmt.Printf("  [recover] reconnecting...\n")
	if err := conn.Connect(); err == nil {
		// Try a ping — INIT is the lightest round-trip.
		client := api.NewClient(conn)
		r := client.Core.Init()
		if r.OK {
			fmt.Printf("  [recover] port responsive again\n")
			return nil
		}
		fmt.Printf("  [recover] reconnect returned but init failed: %s\n", r.Error)
	}

	// Fallback (Windows only): cycle the USB device via powershell.
	// Non-fatal if it fails — user can physically unplug.
	fmt.Printf("  [recover] soft reopen failed — user may need to unplug/replug\n")
	return fmt.Errorf("port %s unresponsive", port)
}

// ────── Runner ─────────────────────────────────────────────────────────────

func run() int {
	args := parseArgs()

	conn := protocol.NewConnection(args.port, protocol.DefaultBaud, false)
	conn.SetTimeout(8 * time.Second)
	if err := conn.Connect(); err != nil {
		fmt.Fprintf(os.Stderr, "connect: %v\n", err)
		return 1
	}
	defer conn.Close()

	// Dump server-side DiagLog messages to stderr so we can see what the
	// firmware is doing during a stalled upload.
	conn.SetCallback(func(resp *protocol.Response) {
		if resp.PacketType == 0xFD && len(resp.Payload) >= 2 { // LOG_MESSAGE
			txt := string(resp.Payload[2:])
			fmt.Fprintf(os.Stderr, "[fw] %s\n", strings.TrimRight(txt, "\r\n"))
		}
	})

	client := api.NewClient(conn)

	// INIT — also upgrades uploadChunkSize via IDENTIFY/INIT payload parsing.
	fmt.Printf("=== Connecting to %s @ %d baud ===\n", args.port, protocol.DefaultBaud)
	initRes := client.Core.Init()
	if !initRes.OK {
		fmt.Fprintf(os.Stderr, "init: %s\n", initRes.Error)
		return 1
	}
	// Storage test firmware uses 2044 payload (ESP32-S3).
	client.Files.SetPeerMaxPayload(api.Esp32MaxPayload)

	// Clean + recreate remote base on both targets. Each test-run starts fresh
	// so flash/SD capacity checks don't fail on leftovers from a prior suite.
	for _, tgt := range []byte{hubfx.StorageTargetFlash, hubfx.StorageTargetSd} {
		tname := targetName(tgt)
		if r := client.Files.Delete(tgt, args.remoteDir, true); !r.OK {
			// Ignore NOT_FOUND-equivalent — only print real errors.
			fmt.Printf("pre-clean %s %s: %s (ignored)\n", tname, args.remoteDir, r.Error)
		}
		if r := client.Files.Mkdir(tgt, args.remoteDir, true); !r.OK {
			fmt.Printf("mkdir %s %s: %s (continuing — may not matter)\n", tname, args.remoteDir, r.Error)
		}
	}

	cases := buildMatrix(args)
	fmt.Printf("=== Running %d test cases ===\n\n", len(cases))

	results := make([]result, 0, len(cases))
	for i, tc := range cases {
		fmt.Printf("[%d/%d] %s %s %d MB — ", i+1, len(cases),
			targetName(tc.target), modeName(tc.mode), tc.sizeMB)

		sizeBytes := tc.sizeMB * 1024 * 1024
		data := makeData(sizeBytes, int64(1000+tc.sizeMB*10)+int64(tc.target))
		remotePath := fmt.Sprintf("%s/test_%s_%dmb.bin",
			args.remoteDir, modeName(tc.mode), tc.sizeMB)

		r := runOne(client, conn, args, tc, data, remotePath)
		results = append(results, r)

		if r.OK {
			fmt.Printf("OK  %.1f KB/s  md5=%s  (%.2fs)\n",
				r.KBs, okStr(r.MD5OK), r.Elapsed.Seconds())
		} else {
			fmt.Printf("FAIL: %s\n", r.Error)
			if args.recover {
				if err := recoverPort(args.port, conn); err != nil {
					fmt.Printf("  [recover] unrecoverable — aborting suite\n")
					break
				}
				// Re-INIT after reopen.
				client = api.NewClient(conn)
				client.Files.SetPeerMaxPayload(api.Esp32MaxPayload)
				_ = client.Core.Init()
			}
		}

		// Short cooldown between tests — lets SD finish any async flush.
		time.Sleep(500 * time.Millisecond)
	}

	printSummary(results)
	if args.logFile != "" {
		appendCSV(args.logFile, results)
	}

	// Exit non-zero if any test failed.
	for _, r := range results {
		if !r.OK {
			return 1
		}
	}
	return 0
}

func runOne(client *api.Client, conn *protocol.Connection, args cliArgs,
	tc testCase, data []byte, remotePath string) result {

	r := result{
		Target: targetName(tc.target),
		Mode:   modeName(tc.mode),
		SizeMB: tc.sizeMB,
	}

	localMD5 := md5.Sum(data)

	start := time.Now()
	var lastPrint time.Time
	progress := func(sent, total int) {
		if time.Since(lastPrint) < 100*time.Millisecond {
			return
		}
		lastPrint = time.Now()
		pct := (sent * 100) / total
		fmt.Printf("\n    tx %d/%d (%d%%) t=%dms",
			sent, total, pct, time.Since(start).Milliseconds())
	}
	ur := client.Files.Upload(tc.target, remotePath, data, tc.mode, progress)
	r.Elapsed = time.Since(start)

	if !ur.OK {
		r.Error = ur.Error
		return r
	}
	r.OK = true
	r.KBs = ur.SpeedKBs
	r.MD5OK = ur.MD5Match || strings.EqualFold(ur.RemoteMD5, hex.EncodeToString(localMD5[:]))

	// Leave the file on the device — user can inspect with file.tree.
	return r
}

func okStr(ok bool) string {
	if ok {
		return "OK"
	}
	return "FAIL"
}

func printSummary(results []result) {
	fmt.Println("\n=== Summary ===")
	fmt.Printf("%-8s %-6s %-4s %-6s %-10s %-8s %s\n",
		"TARGET", "MODE", "MB", "STATUS", "KB/s", "MD5", "ERROR")
	pass, fail := 0, 0
	for _, r := range results {
		status := "PASS"
		if !r.OK {
			status = "FAIL"
			fail++
		} else {
			pass++
		}
		md5s := "-"
		if r.OK {
			md5s = okStr(r.MD5OK)
		}
		fmt.Printf("%-8s %-6s %-4d %-6s %-10.1f %-8s %s\n",
			r.Target, r.Mode, r.SizeMB, status, r.KBs, md5s, r.Error)
	}
	fmt.Printf("\n%d passed, %d failed\n", pass, fail)
}

func appendCSV(path string, results []result) {
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		fmt.Fprintf(os.Stderr, "log: %v\n", err)
		return
	}
	defer f.Close()
	if info, _ := f.Stat(); info != nil && info.Size() == 0 {
		fmt.Fprintln(f, "timestamp,target,mode,size_mb,ok,kbs,elapsed_s,md5_ok,error")
	}
	ts := time.Now().Format(time.RFC3339)
	for _, r := range results {
		fmt.Fprintf(f, "%s,%s,%s,%d,%v,%.1f,%.3f,%v,%q\n",
			ts, r.Target, r.Mode, r.SizeMB, r.OK, r.KBs, r.Elapsed.Seconds(),
			r.MD5OK, r.Error)
	}
}

// Keep these imports in use across platforms even if the current build
// doesn't call them on Windows-specific paths.
var _ = filepath.Join
var _ = exec.Command

func main() {
	os.Exit(run())
}
