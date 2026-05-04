// gen_sd_large generates the `sd_large/` test fixture on demand.
//
//   cd tests/upload_fixtures && go run gen_sd_large.go
//
// Produces a deterministic multi-MB tree suitable for testing SD-card
// windowed uploads. Not committed — output is gitignored via the fixture
// directory's .gitignore.

//go:build ignore

package main

import (
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
)

type spec struct {
	path  string
	bytes int
	seed  int64
}

func main() {
	root := "sd_large"
	_ = os.RemoveAll(root)

	files := []spec{
		{"media/clip1.bin", 2 * 1024 * 1024, 0x5f3759df},
		{"media/clip2.bin", 3 * 1024 * 1024, 0xdeadbeef},
		{"media/thumbs/t1.bin", 64 * 1024, 0xcafebabe},
		{"media/thumbs/t2.bin", 64 * 1024, 0xfeedface},
		{"logs/session.log", 1 * 1024 * 1024, 0xbaddcafe},
	}

	readme := []byte(
		"ScaleFX Upload Fixture — sd_large (generated)\n" +
			"=============================================\n\n" +
			"Regenerate with: go run gen_sd_large.go\n\n" +
			"Deterministic seeded content — MD5s stable across regenerations.\n",
	)
	writeFile(filepath.Join(root, "README.txt"), readme)

	var total int64
	for _, f := range files {
		p := filepath.Join(root, filepath.FromSlash(f.path))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			die(err)
		}
		buf := make([]byte, f.bytes)
		rand.New(rand.NewSource(f.seed)).Read(buf)
		writeFile(p, buf)
		total += int64(f.bytes)
		fmt.Printf("  %s  %s\n", humanBytes(uint64(f.bytes)), p)
	}
	fmt.Printf("\nGenerated %d file(s), %s total under %s/\n",
		len(files), humanBytes(uint64(total)), root)
}

func writeFile(path string, data []byte) {
	if err := os.WriteFile(path, data, 0o644); err != nil {
		die(err)
	}
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

func die(err error) {
	fmt.Fprintln(os.Stderr, "error:", err)
	os.Exit(1)
}
