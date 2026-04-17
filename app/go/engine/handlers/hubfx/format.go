package hubfx

// ScaleFX Engine - HubFX CLI Formatters
// Console rendering for StatusBroadcast + storage output (listing, tree,
// size). Called by the engine when the user issues `status` or a file op.

import (
	"fmt"
	"scalefx/engine"
	"strings"
)

// FormatStatusBroadcast renders a decoded HubFX STATUS_BROADCAST to the CLI.
func (h *Handler) FormatStatusBroadcast(s *StatusBroadcast) {
	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, "━━━ HubFX Status ━━━"))

	// Core 1
	c1Color, c1Text := engine.ColorRed, "NOT READY"
	if s.Core1Ready {
		c1Color, c1Text = engine.ColorGreen, "Ready"
	}
	h.E.Out.Printf("  Core 1:    %s\n", h.E.Out.C(c1Color, c1Text))
	if s.Loop1Count > 0 {
		h.E.Out.Printf("             %d iterations\n", s.Loop1Count)
	}

	// Audio / Flash / SD / USB
	printFlag := func(label string, ok bool, okText, nokText string, nokColor engine.Color) {
		color, text := nokColor, nokText
		if ok {
			color, text = engine.ColorGreen, okText
		}
		h.E.Out.Printf("  %-10s %s\n", label+":", h.E.Out.C(color, text))
	}
	printFlag("Audio", s.AudioInit, "Initialized", "Not initialized", engine.ColorYellow)
	printFlag("Flash", s.FlashReady, "Ready", "Not available", engine.ColorYellow)
	printFlag("SD Card", s.SdReady, "Ready", "Not available", engine.ColorYellow)
	printFlag("USB Host", s.UsbReady, "Active", "Not active", engine.ColorYellow)

	// Slaves
	slaves := []struct {
		name  string
		ready bool
	}{
		{"GunFX", s.GunFxReady},
		{"LightFX", s.LightFxReady},
		{"GearControl", s.GearCtrlReady},
	}
	hasSlaves := false
	for _, sl := range slaves {
		if sl.ready {
			hasSlaves = true
			break
		}
	}
	if hasSlaves {
		h.E.Out.Printf("  Slaves:\n")
		for _, sl := range slaves {
			color, status := engine.ColorRed, "not connected"
			if sl.ready {
				color, status = engine.ColorGreen, "connected"
			}
			h.E.Out.Printf("    %s: %s\n", sl.name, h.E.Out.C(color, status))
		}
	} else {
		h.E.Out.Printf("  Slaves:    %s\n", h.E.Out.C(engine.ColorYellow, "None connected"))
	}

	// I2C devices (extended payload)
	if s.I2C.Present {
		h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan,
			fmt.Sprintf("━━━ I2C Devices (%d/8) ━━━", s.I2C.DetectedCount)))

		pcalColor, pcalText := engine.ColorRed, "not found"
		if s.I2C.PCALPresent {
			pcalColor, pcalText = engine.ColorGreen, "OK"
		}
		h.E.Out.Printf("  PCAL6416A: %s  (0x20 GPIO expander)\n", h.E.Out.C(pcalColor, pcalText))

		for i, bus := range s.I2C.INA226 {
			addr := 0x40 + i
			if bus.Present {
				v := float64(bus.Voltage_mV) / 1000.0
				h.E.Out.Printf("  INA226[%d]: %s  (0x%02X)\n",
					i, h.E.Out.C(engine.ColorGreen, fmt.Sprintf("%.3fV (%d mV)", v, bus.Voltage_mV)), addr)
			} else {
				h.E.Out.Printf("  INA226[%d]: %s  (0x%02X)\n",
					i, h.E.Out.C(engine.ColorRed, "not found"), addr)
			}
		}

		if s.I2C.TASPresent {
			h.E.Out.Printf("  TAS5825M:  %s  (0x4C audio codec)\n", h.E.Out.C(engine.ColorGreen, "OK"))
		}
	}
}

// FormatSize returns a human-readable size string (B, KB, MB, GB).
func FormatSize(size uint32) string {
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

// FormatListing prints a formatted directory listing from tab-separated stream data.
func (h *Handler) FormatListing(text string, rootPath string) {
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
			h.E.Out.Error(line)
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

	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, rootPath))
	if len(entries) == 0 {
		h.E.Out.Println("  (empty)")
		h.E.Out.Println("")
		return
	}

	maxName := 0
	for _, en := range entries {
		if len(en.name) > maxName {
			maxName = len(en.name)
		}
	}

	for _, en := range entries {
		if en.typeChar == "d" {
			h.E.Out.Printf("    %s\n", h.E.Out.C(engine.ColorBlue, fmt.Sprintf("%-*s", maxName+1, en.name+"/")))
		} else {
			sizeStr := ""
			if en.typeChar == "f" {
				sizeStr = "  " + FormatSize(uint32(en.size))
			}
			h.E.Out.Printf("    %-*s%s\n", maxName+1, en.name, sizeStr)
		}
	}
	h.E.Out.Println("")
}

// RenderTree prints a tree view with box-drawing characters from tab-separated stream data.
func (h *Handler) RenderTree(text string, rootPath string) {
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
			h.E.Out.Error(line)
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

	h.E.Out.Printf("\n  %s\n", h.E.Out.C(engine.ColorCyan, rootPath))
	if len(entries) == 0 {
		h.E.Out.Println("  (empty)")
		h.E.Out.Println("")
		return
	}

	isLast := make([]bool, len(entries))
	for i, en := range entries {
		isLast[i] = true
		for j := i + 1; j < len(entries); j++ {
			if entries[j].depth == en.depth {
				isLast[i] = false
				break
			}
			if entries[j].depth < en.depth {
				break
			}
		}
	}

	depthContinues := make(map[int]bool)
	dirCount, fileCount := 0, 0

	for i, en := range entries {
		last := isLast[i]

		prefix := "  "
		for d := 0; d < en.depth; d++ {
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
		if en.isDir {
			dirCount++
			display = h.E.Out.C(engine.ColorBlue, en.name+"/")
		} else {
			fileCount++
			display = fmt.Sprintf("%s (%s)", en.name, FormatSize(uint32(en.size)))
		}

		h.E.Out.Printf("%s%s%s\n", prefix, connector, display)

		depthContinues[en.depth] = !last
		for d := range depthContinues {
			if d > en.depth {
				delete(depthContinues, d)
			}
		}
	}

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
	h.E.Out.Printf("\n  %s\n", strings.Join(parts, ", "))
	h.E.Out.Println("")
}
