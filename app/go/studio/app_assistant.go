package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"studio/assistant"
	"studio/genai"
)

// ── Assistant settings (local, user-machine only) ────────────────────
//
// Multi-provider: the active provider ("gemini" | "groq") selects which key +
// model are used.  Keys resolve as saved-key → build-time builtin → none.  The
// settings file lives in %APPDATA%/ScaleFX/settings.json — the operator's
// machine, never the repo, never the device config.

type studioSettings struct {
	AIProvider    string `json:"aiProvider,omitempty"` // "gemini" | "groq" | "mistral"
	GeminiAPIKey  string `json:"geminiApiKey,omitempty"`
	GeminiModel   string `json:"geminiModel,omitempty"`
	GroqAPIKey    string `json:"groqApiKey,omitempty"`
	GroqModel     string `json:"groqModel,omitempty"`
	MistralAPIKey string `json:"mistralApiKey,omitempty"`
	MistralModel  string `json:"mistralModel,omitempty"`
}

// known providers, in display order.
var aiProviders = []ProviderStatusDTO{
	{ID: "gemini", Label: "Google Gemini"},
	{ID: "groq", Label: "Groq"},
	{ID: "mistral", Label: "Mistral"},
}

func isKnownProvider(id string) bool {
	for _, p := range aiProviders {
		if p.ID == id {
			return true
		}
	}
	return false
}

func assistantSettingsPath() (string, error) {
	cfgDir, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	dir := filepath.Join(cfgDir, "ScaleFX")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	return filepath.Join(dir, "settings.json"), nil
}

func loadStudioSettings() studioSettings {
	var s studioSettings
	p, err := assistantSettingsPath()
	if err != nil {
		return s
	}
	if b, err := os.ReadFile(p); err == nil {
		_ = json.Unmarshal(b, &s)
	}
	return s
}

func saveStudioSettings(s studioSettings) error {
	p, err := assistantSettingsPath()
	if err != nil {
		return err
	}
	b, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(p, b, 0o600)
}

func activeProvider() string {
	p := loadStudioSettings().AIProvider
	if isKnownProvider(p) {
		return p
	}
	return "gemini"
}

// providerKey returns the effective key + its source for a provider.
func providerKey(provider string) (key, source string) {
	s := loadStudioSettings()
	switch provider {
	case "groq":
		if s.GroqAPIKey != "" {
			return s.GroqAPIKey, "settings"
		}
		if genai.BuiltinGroqKey != "" {
			return genai.BuiltinGroqKey, "builtin"
		}
	case "mistral":
		if s.MistralAPIKey != "" {
			return s.MistralAPIKey, "settings"
		}
		if genai.BuiltinMistralKey != "" {
			return genai.BuiltinMistralKey, "builtin"
		}
	default: // gemini
		if s.GeminiAPIKey != "" {
			return s.GeminiAPIKey, "settings"
		}
		if genai.BuiltinKey != "" {
			return genai.BuiltinKey, "builtin"
		}
	}
	return "", "none"
}

func providerModel(provider string) string {
	s := loadStudioSettings()
	switch provider {
	case "groq":
		if s.GroqModel != "" {
			return s.GroqModel
		}
		return genai.DefaultGroqModel
	case "mistral":
		if s.MistralModel != "" {
			return s.MistralModel
		}
		return genai.DefaultMistralModel
	default:
		if s.GeminiModel != "" {
			return s.GeminiModel
		}
		return genai.DefaultGeminiModel
	}
}

func buildProvider(provider, key, model string) genai.Provider {
	switch provider {
	case "groq":
		return genai.NewGroq(key, model)
	case "mistral":
		return genai.NewMistral(key, model)
	default:
		return genai.NewGemini(key, model)
	}
}

// ── Bindings ─────────────────────────────────────────────────────────

// ProviderStatusDTO describes one provider's key state (for the Settings UI).
type ProviderStatusDTO struct {
	ID        string `json:"id"`
	Label     string `json:"label"`
	HasKey    bool   `json:"hasKey"`
	KeySource string `json:"keySource"`
}

// AssistantStatusDTO tells the UI whether the active provider is usable.
type AssistantStatusDTO struct {
	Provider  string              `json:"provider"`  // active provider id
	Model     string              `json:"model"`     // active provider's model
	Available bool                `json:"available"` // active provider has a key
	KeySource string              `json:"keySource"`
	Providers []ProviderStatusDTO `json:"providers"`
}

// AssistantMessage is one chat turn from the frontend.
type AssistantMessage struct {
	Role    string `json:"role"` // "user" | "model"
	Content string `json:"content"`
}

// AssistantReply is the model's answer (or an error string).
type AssistantReply struct {
	Text  string `json:"text"`
	Error string `json:"error"`
}

// AssistantModelDTO is one selectable model (for the panel dropdown).
type AssistantModelDTO struct {
	ID          string `json:"id"`
	DisplayName string `json:"displayName"`
}

func providerStatuses() []ProviderStatusDTO {
	out := make([]ProviderStatusDTO, 0, len(aiProviders))
	for _, p := range aiProviders {
		key, src := providerKey(p.ID)
		out = append(out, ProviderStatusDTO{ID: p.ID, Label: p.Label, HasKey: key != "", KeySource: src})
	}
	return out
}

// AssistantStatus reports the active provider + per-provider key state.
func (a *App) AssistantStatus() AssistantStatusDTO {
	prov := activeProvider()
	key, src := providerKey(prov)
	return AssistantStatusDTO{
		Provider:  prov,
		Model:     providerModel(prov),
		Available: key != "",
		KeySource: src,
		Providers: providerStatuses(),
	}
}

// SetAssistantProvider sets the active provider ("gemini" | "groq").
func (a *App) SetAssistantProvider(provider string) error {
	if !isKnownProvider(provider) {
		provider = "gemini"
	}
	s := loadStudioSettings()
	s.AIProvider = provider
	return saveStudioSettings(s)
}

// SetAssistantKey stores a provider's API key locally.  Empty key clears it.
func (a *App) SetAssistantKey(provider, key string) error {
	s := loadStudioSettings()
	switch provider {
	case "groq":
		s.GroqAPIKey = key
	case "mistral":
		s.MistralAPIKey = key
	default:
		s.GeminiAPIKey = key
	}
	return saveStudioSettings(s)
}

// SetAssistantModel stores the model id for the ACTIVE provider.  Empty falls
// back to that provider's default.
func (a *App) SetAssistantModel(model string) error {
	s := loadStudioSettings()
	switch activeProvider() {
	case "groq":
		s.GroqModel = model
	case "mistral":
		s.MistralModel = model
	default:
		s.GeminiModel = model
	}
	return saveStudioSettings(s)
}

// ListAssistantModels lists the chat-capable models the ACTIVE provider's key
// can access — drives the panel model dropdown.
func (a *App) ListAssistantModels() ([]AssistantModelDTO, error) {
	prov := activeProvider()
	key, _ := providerKey(prov)
	if key == "" {
		return nil, fmt.Errorf("no API key configured for %s", prov)
	}
	parent := a.ctx
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, 20*time.Second)
	defer cancel()

	var models []genai.ModelInfo
	var err error
	switch prov {
	case "groq":
		models, err = genai.ListGroqModels(ctx, key)
	case "mistral":
		models, err = genai.ListMistralModels(ctx, key)
	default:
		models, err = genai.ListGeminiModels(ctx, key)
	}
	if err != nil {
		return nil, err
	}
	out := make([]AssistantModelDTO, 0, len(models))
	for _, m := range models {
		out = append(out, AssistantModelDTO{ID: m.ID, DisplayName: m.DisplayName})
	}
	return out, nil
}

// FAQItemDTO is one curated question + markdown answer (the non-LLM FAQ tab).
type FAQItemDTO struct {
	Question string `json:"question"`
	Answer   string `json:"answer"`
}

// AssistantFAQ returns the curated FAQ parsed from the embedded textbook — a
// deterministic, offline, no-key answer source (the FAQ tab).
func (a *App) AssistantFAQ() []FAQItemDTO {
	items := assistant.FAQItems()
	out := make([]FAQItemDTO, 0, len(items))
	for _, it := range items {
		out = append(out, FAQItemDTO{Question: it.Question, Answer: it.Answer})
	}
	return out
}

// AssistantAsk runs one advisory turn through the active provider.  liveContext
// is the operator's current setup, assembled by the frontend (device model +
// effect drafts, with human-readable names).  history is the full conversation
// (oldest first), ending with the latest user message.
func (a *App) AssistantAsk(history []AssistantMessage, liveContext string) AssistantReply {
	prov := activeProvider()
	key, _ := providerKey(prov)
	if key == "" {
		return AssistantReply{Error: fmt.Sprintf("No API key configured for %s. Add one in Settings (View → Settings).", prov)}
	}

	provider := buildProvider(prov, key, providerModel(prov))
	asst := assistant.New(provider)

	msgs := make([]genai.Message, 0, len(history))
	for _, m := range history {
		role := genai.RoleUser
		if m.Role == "model" {
			role = genai.RoleModel
		}
		msgs = append(msgs, genai.Message{Role: role, Content: m.Content})
	}

	parent := a.ctx
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, 60*time.Second)
	defer cancel()

	text, err := asst.Ask(ctx, liveContext, msgs)
	if err != nil {
		a.diag.Error("FE.AI", "assistant ask failed: %v", err)
		return AssistantReply{Error: err.Error()}
	}
	return AssistantReply{Text: text}
}
