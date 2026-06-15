package genai

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"
)

// OpenAIProvider talks to any OpenAI-compatible chat-completions endpoint
// (Mistral, local servers, …) — reusable behind Provider.
type OpenAIProvider struct {
	name    string
	baseURL string // e.g. "https://api.mistral.ai/v1"
	apiKey  string
	model   string
	http    *http.Client
}

// NewOpenAICompat builds a provider for an OpenAI-compatible API.
func NewOpenAICompat(name, baseURL, apiKey, model string) *OpenAIProvider {
	return &OpenAIProvider{
		name:    name,
		baseURL: strings.TrimRight(baseURL, "/"),
		apiKey:  apiKey,
		model:   model,
		http:    &http.Client{Timeout: 60 * time.Second},
	}
}

func (p *OpenAIProvider) Name() string  { return p.name }
func (p *OpenAIProvider) Model() string { return p.model }

type oaiMsg struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}
type oaiReq struct {
	Model    string   `json:"model"`
	Messages []oaiMsg `json:"messages"`
}
type oaiResp struct {
	Choices []struct {
		Message oaiMsg `json:"message"`
	} `json:"choices"`
	Error *struct {
		Message string `json:"message"`
	} `json:"error"`
}

func (p *OpenAIProvider) Generate(ctx context.Context, system string, history []Message) (string, error) {
	if p.apiKey == "" {
		return "", ErrNoKey
	}
	msgs := make([]oaiMsg, 0, len(history)+1)
	if strings.TrimSpace(system) != "" {
		msgs = append(msgs, oaiMsg{Role: "system", Content: system})
	}
	for _, m := range history {
		role := "user"
		if m.Role == RoleModel {
			role = "assistant"
		}
		msgs = append(msgs, oaiMsg{Role: role, Content: m.Content})
	}

	body, err := json.Marshal(oaiReq{Model: p.model, Messages: msgs})
	if err != nil {
		return "", fmt.Errorf("%s: marshal: %w", p.name, err)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, p.baseURL+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return "", fmt.Errorf("%s: request: %w", p.name, err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+p.apiKey)

	resp, err := p.http.Do(req)
	if err != nil {
		return "", fmt.Errorf("%s: http: %w", p.name, err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)

	var out oaiResp
	if err := json.Unmarshal(raw, &out); err != nil {
		return "", fmt.Errorf("%s: decode (http %d): %w", p.name, resp.StatusCode, err)
	}
	if out.Error != nil || resp.StatusCode != http.StatusOK {
		return "", &APIError{Provider: p.name, Status: resp.StatusCode, Body: truncateBody(string(raw))}
	}
	if len(out.Choices) == 0 {
		return "", ErrEmpty
	}
	return out.Choices[0].Message.Content, nil
}

// ListOpenAIModels lists models from an OpenAI-compatible /models endpoint.
func ListOpenAIModels(ctx context.Context, baseURL, apiKey string) ([]ModelInfo, error) {
	if apiKey == "" {
		return nil, ErrNoKey
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, strings.TrimRight(baseURL, "/")+"/models", nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+apiKey)

	client := &http.Client{Timeout: 20 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("list models: %w", err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)

	var out struct {
		Data []struct {
			ID string `json:"id"`
		} `json:"data"`
		Error *struct {
			Message string `json:"message"`
		} `json:"error"`
	}
	if err := json.Unmarshal(raw, &out); err != nil {
		return nil, fmt.Errorf("list models decode (http %d): %w", resp.StatusCode, err)
	}
	if out.Error != nil {
		return nil, fmt.Errorf("%s", out.Error.Message)
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("list models: http %d", resp.StatusCode)
	}

	models := make([]ModelInfo, 0, len(out.Data))
	for _, m := range out.Data {
		models = append(models, ModelInfo{ID: m.ID, DisplayName: m.ID})
	}
	sort.Slice(models, func(i, j int) bool { return models[i].ID < models[j].ID })
	return models, nil
}
