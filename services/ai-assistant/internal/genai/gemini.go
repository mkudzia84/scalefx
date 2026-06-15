package genai

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// DefaultGeminiModel is a fast, inexpensive model well-suited to interactive
// config Q&A.  Overridable per-machine in Settings (Gemini model field) if the
// API spells the id differently or you want a deeper model.
const DefaultGeminiModel = "gemini-2.5-flash"

// GeminiProvider talks to Google's Generative Language REST API
// (generativelanguage.googleapis.com).  Reusable + swappable behind Provider.
type GeminiProvider struct {
	apiKey string
	model  string
	http   *http.Client
}

// NewGemini builds a Gemini provider.  An empty model uses DefaultGeminiModel.
func NewGemini(apiKey, model string) *GeminiProvider {
	if model == "" {
		model = DefaultGeminiModel
	}
	return &GeminiProvider{
		apiKey: apiKey,
		model:  model,
		http:   &http.Client{Timeout: 60 * time.Second},
	}
}

func (g *GeminiProvider) Name() string  { return "Gemini" }
func (g *GeminiProvider) Model() string { return g.model }

// --- wire types (subset of the generateContent schema) ---

type gemPart struct {
	Text string `json:"text"`
}
type gemContent struct {
	Role  string    `json:"role,omitempty"`
	Parts []gemPart `json:"parts"`
}
type gemSystem struct {
	Parts []gemPart `json:"parts"`
}
type gemRequest struct {
	SystemInstruction *gemSystem   `json:"systemInstruction,omitempty"`
	Contents          []gemContent `json:"contents"`
}
type gemResponse struct {
	Candidates []struct {
		Content gemContent `json:"content"`
	} `json:"candidates"`
	PromptFeedback *struct {
		BlockReason string `json:"blockReason"`
	} `json:"promptFeedback"`
	Error *struct {
		Message string `json:"message"`
	} `json:"error"`
}

// Generate implements Provider via a single (non-streaming) generateContent
// call.  Streaming can be layered on later behind the same interface.
func (g *GeminiProvider) Generate(ctx context.Context, system string, history []Message) (string, error) {
	if g.apiKey == "" {
		return "", ErrNoKey
	}
	req := gemRequest{Contents: make([]gemContent, 0, len(history))}
	if strings.TrimSpace(system) != "" {
		req.SystemInstruction = &gemSystem{Parts: []gemPart{{Text: system}}}
	}
	for _, m := range history {
		role := "user"
		if m.Role == RoleModel {
			role = "model"
		}
		req.Contents = append(req.Contents, gemContent{Role: role, Parts: []gemPart{{Text: m.Content}}})
	}

	body, err := json.Marshal(req)
	if err != nil {
		return "", fmt.Errorf("gemini: marshal: %w", err)
	}
	url := fmt.Sprintf("https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s", g.model, g.apiKey)
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return "", fmt.Errorf("gemini: request: %w", err)
	}
	httpReq.Header.Set("Content-Type", "application/json")

	resp, err := g.http.Do(httpReq)
	if err != nil {
		return "", fmt.Errorf("gemini: http: %w", err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)

	var out gemResponse
	if err := json.Unmarshal(raw, &out); err != nil {
		return "", fmt.Errorf("gemini: decode (http %d): %w", resp.StatusCode, err)
	}
	if out.Error != nil {
		return "", fmt.Errorf("gemini: %s", out.Error.Message)
	}
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("gemini: http %d", resp.StatusCode)
	}
	if out.PromptFeedback != nil && out.PromptFeedback.BlockReason != "" {
		return "", fmt.Errorf("gemini: blocked (%s)", out.PromptFeedback.BlockReason)
	}
	if len(out.Candidates) == 0 || len(out.Candidates[0].Content.Parts) == 0 {
		return "", fmt.Errorf("gemini: empty response")
	}
	var sb strings.Builder
	for _, p := range out.Candidates[0].Content.Parts {
		sb.WriteString(p.Text)
	}
	return sb.String(), nil
}
