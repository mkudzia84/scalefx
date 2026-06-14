package assistant

import (
	"regexp"
	"strings"
)

// FAQItem is one question + its (markdown) answer.
type FAQItem struct {
	Question string
	Answer   string
}

var faqQuestion = regexp.MustCompile(`^\*\*Q:\s*(.+?)\*\*\s*$`)

// FAQItems parses the embedded FAQ (knowledge/40-faq.md) into Q&A pairs.  This
// is the source for the non-LLM FAQ tab — deterministic, offline, no key.  The
// file format is a `**Q: …**` line followed by the answer until the next `**Q:`.
func FAQItems() []FAQItem {
	b, err := knowledgeFS.ReadFile("knowledge/40-faq.md")
	if err != nil {
		return nil
	}

	var items []FAQItem
	var q string
	var answer strings.Builder
	flush := func() {
		if q != "" {
			items = append(items, FAQItem{Question: q, Answer: strings.TrimSpace(answer.String())})
		}
		q = ""
		answer.Reset()
	}

	for _, line := range strings.Split(string(b), "\n") {
		if m := faqQuestion.FindStringSubmatch(line); m != nil {
			flush()
			q = strings.TrimSpace(m[1])
			continue
		}
		if q != "" { // accumulate answer lines (ignore the heading + intro before the first Q)
			answer.WriteString(line)
			answer.WriteByte('\n')
		}
	}
	flush()
	return items
}
