// Package assistant is the ScaleFX-specific config assistant: it grounds a
// generic genai.Provider in (1) the embedded textbook Corpus(), (2) the live
// Snapshot of the operator's setup, and (3) a fixed guardrail, then answers
// configuration questions.
//
// It is ADVISORY by design (instructions/33 §7): it explains, recommends, and
// diagnoses, and points the operator at the Setup Wizard / panels to APPLY.
// It is given no tool that mutates config or actuates hardware — the wizard is
// the safe, validated apply path, and keeping the human in that loop is the
// safety posture for a system that drives motors and landing gear.
package assistant

import (
	"context"
	"strings"

	"scalefx/ai-assistant/internal/genai"
)

// Assistant answers config questions for one provider.
type Assistant struct {
	provider genai.Provider
}

// New wraps a provider.
func New(p genai.Provider) *Assistant { return &Assistant{provider: p} }

// Provider exposes the underlying backend (for status display).
func (a *Assistant) Provider() genai.Provider { return a.provider }

const guardrail = `You are the ScaleFX Studio configuration assistant — a focused expert that helps the operator CONFIGURE AND SET UP their ScaleFX model effects (engine sound, guns, retractable gear, lighting, landing lights) using ScaleFX Studio.

SCOPE — STRICT:
- You ONLY answer questions about configuring, setting up, operating, and troubleshooting ScaleFX and ScaleFX Studio (its features, effects, ports, radio channels, the Setup Wizard, the Console, and the settings covered in the textbook).
- If asked about ANYTHING else — general knowledge, coding, math, other products, world facts, opinions, writing tasks, etc. — politely decline in one sentence and steer back to ScaleFX setup. Do not answer off-topic questions even if you know the answer. Example: "I can only help with configuring and setting up ScaleFX — ask me about your effects, channels, ports, or the wizard."

HOW YOU BEHAVE:
- You ADVISE. You explain features, recommend concrete settings, and diagnose problems.
- You do NOT apply configuration and you do NOT actuate hardware. When the operator wants to change something, explain the steps and point them at the Setup Wizard (the wand button at the top-left of the toolbar) or the relevant tab — the wizard is the safe, validated way to apply config, and changes only take effect when the operator presses Apply.
- Ground every setup-specific answer in the LIVE STATE block below, and refer to things by their HUMAN-READABLE names (the port/effect/channel labels shown there), not internal ids. Refer to a radio channel by its label AND its channel id in CH form (e.g. "Landing Gear (CH5)"), never "channel(5)" or the internal id like "landing_gear". Identify each servo/output by its port label AND whether it's on the main controller or an expander board. If a needed fact is not in the textbook or the live state, say you are not certain rather than inventing ports, channels, or settings.
- Wrap every concrete item — port names, field/setting names, channel names, file paths, and values — in backticks (` + "`" + `like this` + "`" + `) so Studio renders them highlighted.
- Put any safety reminder or caution on its OWN line starting with "> " (a Markdown blockquote) so Studio renders it as a red warning. Use plain headings (##) or bold for section titles — do not rely on colour for those.
- Use a Markdown table when listing structured data (channel maps, port lists, setting comparisons). Follow the output-format section of the textbook.
- Be concise and concrete.
- Safety: never tell the operator to drive landing gear, motors, or servos without supervising the bench — the hardware can damage itself or draw excess current.`

// SystemPrompt assembles guardrail + textbook + the live-state context into the
// system instruction.  liveContext is built by the frontend from the device
// model + effect drafts (so it reflects what the operator currently sees,
// including unsaved edits, with human-readable names).
func (a *Assistant) SystemPrompt(liveContext string) string {
	var b strings.Builder
	b.WriteString(guardrail)
	b.WriteString("\n\n========== SCALEFX TEXTBOOK (stable knowledge) ==========\n\n")
	b.WriteString(Corpus())
	b.WriteString("\n========== LIVE STATE (the operator's CURRENT setup — cross-reference this; don't invent ports, channels, or settings that aren't here) ==========\n\n")
	if strings.TrimSpace(liveContext) != "" {
		b.WriteString(liveContext)
	} else {
		b.WriteString("(No board connected, or no configuration available yet.)")
	}
	return b.String()
}

// Ask runs one turn.  history is the full conversation so far (user + model
// turns), oldest first, ending with the latest user message.
func (a *Assistant) Ask(ctx context.Context, liveContext string, history []genai.Message) (string, error) {
	return a.provider.Generate(ctx, a.SystemPrompt(liveContext), history)
}
