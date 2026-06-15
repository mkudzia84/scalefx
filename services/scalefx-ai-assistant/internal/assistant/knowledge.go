package assistant

import (
	"embed"
	"io/fs"
	"sort"
	"strings"
)

// knowledgeFS is the embedded textbook corpus — markdown shipped INSIDE the
// Studio binary (it is non-secret, so embedding is correct; the API key is the
// opposite — never embedded).  Files are ordered by name, so prefix them
// numerically (00-, 10-, …) to control concatenation order.
//
//go:embed knowledge/*.md
var knowledgeFS embed.FS

// Corpus returns the full embedded textbook concatenated in filename order,
// ready to drop into a system prompt.  This is the SAME corpus the (future)
// FAQ responder indexes — one source, both consumers.
func Corpus() string {
	entries, err := fs.ReadDir(knowledgeFS, "knowledge")
	if err != nil {
		return ""
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".md") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)

	var sb strings.Builder
	for _, n := range names {
		b, err := knowledgeFS.ReadFile("knowledge/" + n)
		if err != nil {
			continue
		}
		sb.Write(b)
		sb.WriteString("\n\n")
	}
	return sb.String()
}

// CorpusFiles returns the embedded textbook split per file (name → content) —
// for the FAQ responder to index by section, and for diagnostics.
func CorpusFiles() map[string]string {
	out := map[string]string{}
	entries, err := fs.ReadDir(knowledgeFS, "knowledge")
	if err != nil {
		return out
	}
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".md") {
			continue
		}
		if b, err := knowledgeFS.ReadFile("knowledge/" + e.Name()); err == nil {
			out[e.Name()] = string(b)
		}
	}
	return out
}
