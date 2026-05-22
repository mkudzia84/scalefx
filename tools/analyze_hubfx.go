//go:build ignore

// Port silkscreen detector for the HubFX top render — Go port of
// analyze_gearcontrol_image.py (no Pillow/Python needed).  Emits connector
// bounding boxes in raw image pixel coordinates, grouped by board zone,
// for calibrating the Studio board-overlay.
//
//   go run tools/analyze_hubfx.go
package main

import (
	"fmt"
	"image"
	_ "image/png"
	"os"
	"sort"
)

const png = `c:\data\code\scalefx\media\pcb\hubfx_top.png`

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

	// Masks.
	white := make([]bool, W*H)   // bright housings + silkscreen
	magenta := make([]bool, W*H) // pink silkscreen + markers
	for y := 0; y < H; y++ {
		for x := 0; x < W; x++ {
			r, g, bl := at(x, y)
			i := y*W + x
			if r >= 205 && g >= 205 && bl >= 205 {
				white[i] = true
			}
			if r >= 170 && bl >= 130 && g <= 140 && (r+bl)-2*g > 140 {
				magenta[i] = true
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
		sort.Slice(g, func(i, j int) bool {
			if g[i].y != g[j].y {
				return g[i].y < g[j].y
			}
			return g[i].x < g[j].x
		})
		fmt.Printf("\n## %s (%d)\n", name, len(g))
		for _, c := range g {
			cx, cy := c.x+c.w/2, c.y+c.h/2
			fmt.Printf("   x=%4d y=%4d w=%3d h=%3d  cx=%4d cy=%4d  area=%6d  (%.1f%%, %.1f%%)\n",
				c.x, c.y, c.w, c.h, cx, cy, c.area,
				float64(cx)/float64(W)*100, float64(cy)/float64(H)*100)
		}
	}

	wc := comps(white, 120)
	mc := comps(magenta, 40)

	inZone := func(c box, x0, y0, x1, y1 int) bool {
		cx, cy := c.x+c.w/2, c.y+c.h/2
		return cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1
	}
	var left, right, mleft, mright, top []box
	for _, c := range wc {
		if inZone(c, 0, 0, int(float64(W)*0.22), H) && c.w > 18 && c.h > 14 {
			left = append(left, c)
		}
		if inZone(c, int(float64(W)*0.78), 0, W, H) && c.w > 14 && c.h > 14 {
			right = append(right, c)
		}
		// Top zone (audio/speaker connectors), excluding the far-right IN column.
		if inZone(c, int(float64(W)*0.45), 0, int(float64(W)*0.92), int(float64(H)*0.10)) && c.w > 25 && c.h > 25 {
			top = append(top, c)
		}
	}
	for _, c := range mc {
		if inZone(c, 0, 0, int(float64(W)*0.25), H) {
			mleft = append(mleft, c)
		}
		if inZone(c, int(float64(W)*0.75), 0, W, H) {
			mright = append(mright, c)
		}
	}
	dump("TOP white components (audio/speaker)", top)
	dump("LEFT white components (CH1-8 housings)", left)
	dump("RIGHT white components (IN/SRV headers)", right)
	dump("LEFT magenta markers", mleft)
	dump("RIGHT magenta markers", mright)
}
