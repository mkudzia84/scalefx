//go:build ignore

// Port silkscreen / connector detector for the GearControl top render —
// sibling of analyze_hubfx.go.  Emits connector bounding boxes in raw image
// pixel coordinates + image-percent centroids for calibrating the Studio
// board-overlay (pcb.ts gearcontrol entry).
//
// The render highlights the three H-bridge screw terminals (J6/J10/J11) in
// MAGENTA — perfect for a colour mask.  The seven servo headers + the IN
// header are gold/yellow pin grids — caught by a YELLOW mask and clustered.
//
//   go run tools/analyze_gearcontrol.go
package main

import (
	"fmt"
	"image"
	_ "image/png"
	"os"
	"sort"
)

const png = `c:\data\code\scalefx\media\pcb\gearcontrol_top.png`

type box struct{ x, y, w, h, area int }

func main() {
	f, err := os.Open(png)
	if err != nil {
		panic(err)
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		panic(err)
	}
	b := img.Bounds()
	W, H := b.Dx(), b.Dy()
	fmt.Printf("# image: %dx%d\n", W, H)

	at := func(x, y int) (int, int, int) {
		r, g, bl, _ := img.At(b.Min.X+x, b.Min.Y+y).RGBA()
		return int(r >> 8), int(g >> 8), int(bl >> 8)
	}

	magenta := make([]bool, W*H) // pink-highlighted H-bridge terminals
	yellow := make([]bool, W*H)  // gold header pins (servos + IN)
	for y := 0; y < H; y++ {
		for x := 0; x < W; x++ {
			r, g, bl := at(x, y)
			i := y*W + x
			// magenta: strong red+blue, weak green (same as hubfx detector)
			if r >= 170 && bl >= 130 && g <= 140 && (r+bl)-2*g > 140 {
				magenta[i] = true
			}
			// gold/yellow pins: red+green clearly above blue (looser — pins are
			// a darker amber than the magenta highlight).  Exclude the bottom
			// silkscreen text band (y>82%) so "ServoN" labels don't chain into
			// one giant cluster.
			if y < (H*82)/100 && r >= 110 && g >= 80 && (r+g)-2*bl > 60 && r > bl && g > bl {
				yellow[i] = true
			}
		}
	}

	comps := func(mask []bool, minArea int) []box {
		vis := make([]bool, W*H)
		var out []box
		var q [][2]int
		for sy := 0; sy < H; sy++ {
			for sx := 0; sx < W; sx++ {
				si := sy*W + sx
				if !mask[si] || vis[si] {
					continue
				}
				q = q[:0]
				q = append(q, [2]int{sx, sy})
				vis[si] = true
				minx, miny, maxx, maxy, area := sx, sy, sx, sy, 0
				for len(q) > 0 {
					p := q[len(q)-1]
					q = q[:len(q)-1]
					x, y := p[0], p[1]
					area++
					if x < minx {
						minx = x
					}
					if y < miny {
						miny = y
					}
					if x > maxx {
						maxx = x
					}
					if y > maxy {
						maxy = y
					}
					for _, d := range [4][2]int{{1, 0}, {-1, 0}, {0, 1}, {0, -1}} {
						nx, ny := x+d[0], y+d[1]
						if nx >= 0 && nx < W && ny >= 0 && ny < H {
							ni := ny*W + nx
							if mask[ni] && !vis[ni] {
								vis[ni] = true
								q = append(q, [2]int{nx, ny})
							}
						}
					}
				}
				if area >= minArea {
					out = append(out, box{minx, miny, maxx - minx + 1, maxy - miny + 1, area})
				}
			}
		}
		return out
	}

	dump := func(name string, g []box) {
		// sort left→right then top→bottom (connectors sit in horizontal rows)
		sort.Slice(g, func(i, j int) bool {
			if g[i].x != g[j].x {
				return g[i].x < g[j].x
			}
			return g[i].y < g[j].y
		})
		fmt.Printf("\n## %s (%d)\n", name, len(g))
		for _, c := range g {
			cx, cy := c.x+c.w/2, c.y+c.h/2
			fmt.Printf("   cx=%4d cy=%4d  w=%3d h=%3d  area=%6d  (%.1f%%, %.1f%%)\n",
				cx, cy, c.w, c.h, c.area,
				float64(cx)/float64(W)*100, float64(cy)/float64(H)*100)
		}
	}

	// Merge yellow pins into header clusters: a single servo header is a grid of
	// small gold pins, so individual pins detected separately must be grouped.
	cluster := func(g []box, gapX, gapY int) []box {
		used := make([]bool, len(g))
		var out []box
		for i := range g {
			if used[i] {
				continue
			}
			cur := g[i]
			used[i] = true
			changed := true
			for changed {
				changed = false
				for j := range g {
					if used[j] {
						continue
					}
					// bounding-box proximity test
					nx0, ny0 := cur.x-gapX, cur.y-gapY
					nx1, ny1 := cur.x+cur.w+gapX, cur.y+cur.h+gapY
					ox0, oy0 := g[j].x, g[j].y
					ox1, oy1 := g[j].x+g[j].w, g[j].y+g[j].h
					if ox1 >= nx0 && ox0 <= nx1 && oy1 >= ny0 && oy0 <= ny1 {
						minx := min(cur.x, g[j].x)
						miny := min(cur.y, g[j].y)
						maxx := max(cur.x+cur.w, g[j].x+g[j].w)
						maxy := max(cur.y+cur.h, g[j].y+g[j].h)
						cur = box{minx, miny, maxx - minx, maxy - miny, cur.area + g[j].area}
						used[j] = true
						changed = true
					}
				}
			}
			out = append(out, cur)
		}
		return out
	}

	mc := comps(magenta, 40)
	yc := comps(yellow, 4)
	fmt.Printf("# raw yellow blobs: %d\n", len(yc))
	// tight gap so adjacent servo headers (~30px pitch, ~10px gap) stay separate
	yClustered := cluster(yc, 4, 6)
	// keep only sizeable header clusters (drop stray solder dots)
	var headers []box
	for _, c := range yClustered {
		if c.area >= 70 && c.w >= 8 && c.h >= 8 && c.w < 120 {
			headers = append(headers, c)
		}
	}

	dump("MAGENTA H-bridge terminals (expect 3: J6 J10 J11)", mc)
	dump("YELLOW header clusters (servos + IN)", headers)

	// Debug: raw yellow blobs in the servo zone (right half, mid-height) — bin
	// by x to reveal the 7 header columns.
	colCount := map[int]int{}
	colCx := map[int][]int{}
	for _, c := range yc {
		cx, cy := c.x+c.w/2, c.y+c.h/2
		if cx > W/2 && cy > H*40/100 && cy < H*82/100 {
			bin := cx / 10
			colCount[bin]++
			colCx[bin] = append(colCx[bin], cx)
		}
	}
	type col struct{ cx, n int }
	var cols []col
	for bin, n := range colCount {
		sum := 0
		for _, v := range colCx[bin] {
			sum += v
		}
		cols = append(cols, col{sum / len(colCx[bin]), n})
	}
	sort.Slice(cols, func(i, j int) bool { return cols[i].cx < cols[j].cx })
	fmt.Printf("\n## RAW yellow columns in servo zone (x>50%%, y 40-82%%)\n")
	for _, c := range cols {
		fmt.Printf("   cx=%4d  n=%2d  (%.1f%%)\n", c.cx, c.n, float64(c.cx)/float64(W)*100)
	}
}
