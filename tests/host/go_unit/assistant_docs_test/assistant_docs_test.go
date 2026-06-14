// assistant_docs_test guards Rule 64: the AI assistant's grounding must stay in
// sync with the features it describes.  Concretely, it asserts the embedded
// assistant TEXTBOOK documents every EFFECT channel function (by its
// human-readable label).  Add a channel function to
// devicemodel.ChannelFunctions() and forget to document it -> this fails the
// pre-merge gate.
//
// (The live-context builder context.ts and the FAQ are also part of the Rule 64
// grounding; they're convention-guarded — extend this test the same way when a
// new class of grounding is worth automating.)
package assistant_docs_test

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"scalefx/devicemodel"
)

// effectGroups are the channel-function groups tied to ScaleFX effects (the ones
// the textbook must cover).  Flight / Helicopter / Fixed-wing are passthrough RC
// channels ScaleFX reads but doesn't act on, so they don't need documenting.
var effectGroups = map[string]bool{
	"Engine": true, "Lights": true, "Gear": true, "Gun": true, "Audio": true,
}

const knowledgeDir = "../../../../app/go/studio/assistant/knowledge"

var nonAlnum = regexp.MustCompile(`[^a-z0-9]+`)

// normalize lowercases + collapses any run of non-alphanumerics to a single
// space, so "Engine On / Off" and "engine on/off" compare equal.
func normalize(s string) string {
	return strings.TrimSpace(nonAlnum.ReplaceAllString(strings.ToLower(s), " "))
}

func loadCorpus(t *testing.T) string {
	t.Helper()
	entries, err := os.ReadDir(knowledgeDir)
	if err != nil {
		t.Fatalf("read knowledge dir %s: %v", knowledgeDir, err)
	}
	var sb strings.Builder
	n := 0
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".md") {
			continue
		}
		b, err := os.ReadFile(filepath.Join(knowledgeDir, e.Name()))
		if err != nil {
			t.Fatalf("read %s: %v", e.Name(), err)
		}
		sb.Write(b)
		sb.WriteString("\n")
		n++
	}
	if n == 0 {
		t.Fatalf("no .md files found in %s — knowledge corpus missing?", knowledgeDir)
	}
	return sb.String()
}

func TestTextbookDocumentsEffectChannelFunctions(t *testing.T) {
	corpus := normalize(loadCorpus(t))
	checked := 0
	for _, cf := range devicemodel.ChannelFunctions() {
		if !effectGroups[cf.Group] {
			continue
		}
		checked++
		if !strings.Contains(corpus, normalize(cf.Label)) {
			t.Errorf("channel function %q (label %q, group %q) is NOT documented in the assistant textbook.\n"+
				"  Rule 64: add its label to app/go/studio/assistant/knowledge/50-glossary.md (and describe it where relevant).",
				cf.ID, cf.Label, cf.Group)
		}
	}
	if checked == 0 {
		t.Fatal("no effect channel functions found — did ChannelFunctions() or the groups change?")
	}
	t.Logf("verified %d effect channel functions are documented", checked)
}
