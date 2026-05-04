// Virtual board — in-memory faux filesystem.
//
// Lets a virtual board respond to the FILE_* protocol surface (list /
// download / mkdir / delete / upload-begin/data/end) without touching
// the host's actual disk. Two roots — flash and sd — match the
// firmware's two storage targets.
//
// Pre-seed content via Seed() so a freshly-launched LightFX virtual
// board boots with `/lightfx.yaml` already on flash, mirroring real
// firmware's flash layout.

package fauxfs

import (
	"fmt"
	"path"
	"sort"
	"strings"
	"sync"
)

// Target identifiers — mirror hubfx.StorageTarget*.
const (
	TargetSd    byte = 0
	TargetFlash byte = 1
)

// Node represents a single file or directory entry.
type Node struct {
	Name     string
	IsDir    bool
	Data     []byte
	Children map[string]*Node
}

func newDir(name string) *Node {
	return &Node{Name: name, IsDir: true, Children: map[string]*Node{}}
}

// FS holds the two roots. Concurrent-safe.
type FS struct {
	mu    sync.Mutex
	roots map[byte]*Node // target → root dir node

	// In-flight upload state — only one upload at a time per FS, matches
	// the firmware's single-channel upload protocol.
	upload *uploadState
}

type uploadState struct {
	target byte
	path   string
	size   uint32
	mode   byte
	buf    []byte
	expSeq uint16
}

// New returns an FS with empty flash + sd roots.
func New() *FS {
	return &FS{
		roots: map[byte]*Node{
			TargetFlash: newDir(""),
			TargetSd:    newDir(""),
		},
	}
}

// Seed installs a file at `path` on `target`, creating intermediate
// directories as needed. Used at startup to drop demo configs into
// flash so Studio's File Manager / config-loader sees them.
func (f *FS) Seed(target byte, path string, data []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.writeFileLocked(target, path, data)
}

// ─── Lookups ───

// Entry is a directory listing item.
type Entry struct {
	Name  string
	IsDir bool
	Size  uint32
}

func (f *FS) List(target byte, p string) ([]Entry, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	dir, err := f.lookupLocked(target, p)
	if err != nil {
		return nil, err
	}
	if !dir.IsDir {
		return nil, fmt.Errorf("not a directory: %s", p)
	}
	out := make([]Entry, 0, len(dir.Children))
	for _, c := range dir.Children {
		size := uint32(0)
		if !c.IsDir {
			size = uint32(len(c.Data))
		}
		out = append(out, Entry{Name: c.Name, IsDir: c.IsDir, Size: size})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

func (f *FS) Read(target byte, p string) ([]byte, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	n, err := f.lookupLocked(target, p)
	if err != nil {
		return nil, err
	}
	if n.IsDir {
		return nil, fmt.Errorf("is a directory: %s", p)
	}
	out := make([]byte, len(n.Data))
	copy(out, n.Data)
	return out, nil
}

func (f *FS) Stat(target byte, p string) (Entry, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	n, err := f.lookupLocked(target, p)
	if err != nil {
		return Entry{}, err
	}
	size := uint32(0)
	if !n.IsDir {
		size = uint32(len(n.Data))
	}
	return Entry{Name: n.Name, IsDir: n.IsDir, Size: size}, nil
}

// ─── Mutations ───

// Mkdir creates `p` under `target`. If recursive, missing parents are
// created (idempotent). Otherwise the parent must exist and the child
// must not exist.
func (f *FS) Mkdir(target byte, p string, recursive bool) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	root, ok := f.roots[target]
	if !ok {
		return fmt.Errorf("unknown target")
	}
	parts := splitPath(p)
	cur := root
	for i, part := range parts {
		if part == "" {
			continue
		}
		child, exists := cur.Children[part]
		if !exists {
			if !recursive && i < len(parts)-1 {
				return fmt.Errorf("missing parent: %s", strings.Join(parts[:i+1], "/"))
			}
			child = newDir(part)
			cur.Children[part] = child
		} else if !child.IsDir {
			return fmt.Errorf("not a directory: %s", part)
		} else if !recursive && i == len(parts)-1 {
			return fmt.Errorf("already exists: %s", p)
		}
		cur = child
	}
	return nil
}

// Delete removes `p` under `target`. recursive=true removes
// non-empty dirs.
func (f *FS) Delete(target byte, p string, recursive bool) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	root, ok := f.roots[target]
	if !ok {
		return fmt.Errorf("unknown target")
	}
	parts := splitPath(p)
	if len(parts) == 0 {
		return fmt.Errorf("cannot delete root")
	}
	parent := root
	for i, part := range parts[:len(parts)-1] {
		child, ok := parent.Children[part]
		if !ok || !child.IsDir {
			return fmt.Errorf("not found: %s", strings.Join(parts[:i+1], "/"))
		}
		parent = child
	}
	last := parts[len(parts)-1]
	target2, ok := parent.Children[last]
	if !ok {
		return fmt.Errorf("not found: %s", p)
	}
	if target2.IsDir && !recursive && len(target2.Children) > 0 {
		return fmt.Errorf("directory not empty: %s", p)
	}
	delete(parent.Children, last)
	return nil
}

// ─── Upload state machine ───
//
// The firmware accepts FILE_UPLOAD_BEGIN → N×FILE_UPLOAD_DATA →
// FILE_UPLOAD_END (or FILE_UPLOAD_CANCEL). The faux FS mirrors the
// same shape: BeginUpload allocates a buffer, AppendUploadChunk grows
// it, EndUpload commits the file. CancelUpload resets state.

func (f *FS) BeginUpload(target byte, p string, size uint32, mode byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.upload != nil {
		return fmt.Errorf("upload already in progress: %s", f.upload.path)
	}
	if _, ok := f.roots[target]; !ok {
		return fmt.Errorf("unknown target")
	}
	f.upload = &uploadState{
		target: target,
		path:   p,
		size:   size,
		mode:   mode,
		buf:    make([]byte, 0, size),
	}
	return nil
}

func (f *FS) AppendUploadChunk(seq uint16, data []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.upload == nil {
		return fmt.Errorf("no upload in progress")
	}
	if seq != f.upload.expSeq {
		return fmt.Errorf("upload seq mismatch: got %d want %d",
			seq, f.upload.expSeq)
	}
	f.upload.buf = append(f.upload.buf, data...)
	f.upload.expSeq++
	return nil
}

func (f *FS) EndUpload() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.upload == nil {
		return fmt.Errorf("no upload in progress")
	}
	u := f.upload
	f.upload = nil
	return f.writeFileLocked(u.target, u.path, u.buf)
}

func (f *FS) CancelUpload() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.upload = nil
}

// UploadInProgress returns the path of the active upload (or "").
func (f *FS) UploadInProgress() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.upload == nil {
		return ""
	}
	return f.upload.path
}

// ─── Internal helpers ───

func (f *FS) writeFileLocked(target byte, p string, data []byte) error {
	root, ok := f.roots[target]
	if !ok {
		return fmt.Errorf("unknown target")
	}
	parts := splitPath(p)
	if len(parts) == 0 {
		return fmt.Errorf("empty path")
	}
	cur := root
	for _, dir := range parts[:len(parts)-1] {
		child, exists := cur.Children[dir]
		if !exists {
			child = newDir(dir)
			cur.Children[dir] = child
		} else if !child.IsDir {
			return fmt.Errorf("path component is not a directory: %s", dir)
		}
		cur = child
	}
	name := parts[len(parts)-1]
	cur.Children[name] = &Node{
		Name: name,
		Data: append([]byte(nil), data...),
	}
	return nil
}

func (f *FS) lookupLocked(target byte, p string) (*Node, error) {
	root, ok := f.roots[target]
	if !ok {
		return nil, fmt.Errorf("unknown target")
	}
	parts := splitPath(p)
	if len(parts) == 0 {
		return root, nil
	}
	cur := root
	for _, part := range parts {
		child, ok := cur.Children[part]
		if !ok {
			return nil, fmt.Errorf("not found: %s", p)
		}
		cur = child
	}
	return cur, nil
}

func splitPath(p string) []string {
	p = path.Clean("/" + p)
	if p == "/" {
		return nil
	}
	return strings.Split(strings.TrimPrefix(p, "/"), "/")
}
