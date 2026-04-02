package main

// ScaleFX CLI - Storage Output Formatting
// File listing, tree rendering, and size formatting for SD/Flash storage.

import (
	"fmt"
	"strings"
)

// formatSize returns a human-readable size string (B, KB, MB, GB).
func formatSize(size uint32) string {
	switch {
	case size < 1024:
		return fmt.Sprintf("%d B", size)
	case size < 1024*1024:
		return fmt.Sprintf("%.1f KB", float64(size)/1024)
	case size < 1024*1024*1024:
		return fmt.Sprintf("%.1f MB", float64(size)/(1024*1024))
	default:
		return fmt.Sprintf("%.1f GB", float64(size)/(1024*1024*1024))
	}
}

// formatListing prints a formatted directory listing from tab-separated stream data.
// Each line: "type\tname\tsize" where type is 'd' (directory) or 'f' (file).
func formatListing(text string, rootPath string) {
	type entry struct {
		typeChar string
		name     string
		size     int
	}

	var entries []entry
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "ERROR") {
			PrintError(line)
			continue
		}
		parts := strings.SplitN(line, "\t", 3)
		if len(parts) < 3 {
			parts = strings.SplitN(line, " ", 3)
		}
		if len(parts) < 3 {
			entries = append(entries, entry{"?", line, 0})
			continue
		}
		size := 0
		fmt.Sscanf(parts[2], "%d", &size)
		entries = append(entries, entry{parts[0], parts[1], size})
	}

	fmt.Printf("\n  %s%s%s\n", colorCyan, rootPath, colorReset)
	if len(entries) == 0 {
		fmt.Println("  (empty)")
		fmt.Println()
		return
	}

	// Find max name width for alignment
	maxName := 0
	for _, e := range entries {
		if len(e.name) > maxName {
			maxName = len(e.name)
		}
	}

	for _, e := range entries {
		if e.typeChar == "d" {
			fmt.Printf("    %s%-*s%s\n", colorBlue, maxName+1, e.name+"/", colorReset)
		} else {
			sizeStr := ""
			if e.typeChar == "f" {
				sizeStr = "  " + formatSize(uint32(e.size))
			}
			fmt.Printf("    %-*s%s\n", maxName+1, e.name, sizeStr)
		}
	}
	fmt.Println()
}

// renderTree prints a tree view with box-drawing characters from tab-separated stream data.
// Each line: "depth\ttype\tname\tsize" where depth is 0-based, type is 'd' or 'f'.
func renderTree(text string, rootPath string) {
	type treeEntry struct {
		depth int
		isDir bool
		name  string
		size  int
	}

	var entries []treeEntry
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "ERROR") {
			PrintError(line)
			continue
		}
		parts := strings.SplitN(line, "\t", 4)
		if len(parts) < 4 {
			parts = strings.SplitN(line, " ", 4)
		}
		if len(parts) < 4 {
			continue
		}
		var depth, size int
		fmt.Sscanf(parts[0], "%d", &depth)
		fmt.Sscanf(parts[3], "%d", &size)
		entries = append(entries, treeEntry{depth, parts[1] == "d", parts[2], size})
	}

	fmt.Printf("\n  %s%s%s\n", colorCyan, rootPath, colorReset)
	if len(entries) == 0 {
		fmt.Println("  (empty)")
		fmt.Println()
		return
	}

	// Determine whether each entry is the last at its depth
	isLast := make([]bool, len(entries))
	for i, e := range entries {
		isLast[i] = true
		for j := i + 1; j < len(entries); j++ {
			if entries[j].depth == e.depth {
				isLast[i] = false
				break
			}
			if entries[j].depth < e.depth {
				break
			}
		}
	}

	depthContinues := make(map[int]bool)
	dirCount, fileCount := 0, 0

	for i, e := range entries {
		last := isLast[i]

		// Build line prefix from depth bars
		prefix := "  "
		for d := 0; d < e.depth; d++ {
			if depthContinues[d] {
				prefix += "│   "
			} else {
				prefix += "    "
			}
		}

		connector := "├── "
		if last {
			connector = "└── "
		}

		var display string
		if e.isDir {
			dirCount++
			display = fmt.Sprintf("%s%s/%s", colorBlue, e.name, colorReset)
		} else {
			fileCount++
			display = fmt.Sprintf("%s (%s)", e.name, formatSize(uint32(e.size)))
		}

		fmt.Printf("%s%s%s\n", prefix, connector, display)

		depthContinues[e.depth] = !last
		// Clear deeper levels
		for d := range depthContinues {
			if d > e.depth {
				delete(depthContinues, d)
			}
		}
	}

	// Summary
	var parts []string
	if dirCount > 0 {
		suffix := "y"
		if dirCount != 1 {
			suffix = "ies"
		}
		parts = append(parts, fmt.Sprintf("%d director%s", dirCount, suffix))
	}
	if fileCount > 0 {
		suffix := ""
		if fileCount != 1 {
			suffix = "s"
		}
		parts = append(parts, fmt.Sprintf("%d file%s", fileCount, suffix))
	}
	fmt.Printf("\n  %s\n", strings.Join(parts, ", "))
	fmt.Println()
}
