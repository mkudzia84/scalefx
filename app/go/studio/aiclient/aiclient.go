// Package aiclient is ScaleFX Studio's thin REST client to the standalone
// AI-assistant service (services/ai-assistant).  Studio no longer carries
// provider API keys — it sends the conversation + the live-config context and
// the service does the grounding + model routing.
package aiclient

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// Endpoint is the AI-assistant service base URL.  Hardcoded to localhost for now;
// override at build time:  -ldflags "-X studio/aiclient.Endpoint=https://…".
var Endpoint = "http://localhost:8080"

// AuthToken is the JWT bearer token.  Override at build time:
//   -ldflags "-X studio/aiclient.AuthToken=…"
// The committed default is signed with the service's dev secret, so a default
// build of both halves works against a default-built service on localhost.
var AuthToken = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJzY2FsZWZ4LXN0dWRpbyIsImlzcyI6InNjYWxlZngtYWktYXNzaXN0YW50IiwiZXhwIjoyMDk3MTMyOTk3fQ.654PbdleQgtWikziO_iTw4jVSgl3NPdvtrvli3zLS00"

var httpClient = &http.Client{Timeout: 70 * time.Second}

// Model is one selectable model from the service.
type Model struct {
	ID       string `json:"id"`
	Provider string `json:"provider"`
	Label    string `json:"label"`
}

// Message is one chat turn.
type Message struct {
	Role    string `json:"role"` // "user" | "model"
	Content string `json:"content"`
}

// FAQItem is one curated question + markdown answer.
type FAQItem struct {
	Question string `json:"question"`
	Answer   string `json:"answer"`
}

func do(ctx context.Context, method, path string, body any, out any) error {
	var r io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return err
		}
		r = bytes.NewReader(b)
	}
	req, err := http.NewRequestWithContext(ctx, method, Endpoint+path, r)
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+AuthToken)
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("assistant service unreachable (%s): %w", Endpoint, err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		var e struct {
			Error string `json:"error"`
		}
		_ = json.Unmarshal(raw, &e)
		if e.Error != "" {
			return fmt.Errorf("%s", e.Error)
		}
		return fmt.Errorf("assistant service: http %d", resp.StatusCode)
	}
	if out != nil {
		return json.Unmarshal(raw, out)
	}
	return nil
}

// Models lists the models the service offers.
func Models(ctx context.Context) ([]Model, error) {
	var m []Model
	return m, do(ctx, http.MethodGet, "/v1/models", nil, &m)
}

// FAQ returns the curated FAQ.
func FAQ(ctx context.Context) ([]FAQItem, error) {
	var f []FAQItem
	return f, do(ctx, http.MethodGet, "/v1/faq", nil, &f)
}

// Chat sends the conversation + live context + chosen model, returns the reply.
func Chat(ctx context.Context, messages []Message, liveContext, model string) (string, error) {
	var out struct {
		Text  string `json:"text"`
		Model string `json:"model"`
	}
	err := do(ctx, http.MethodPost, "/v1/chat", map[string]any{
		"messages": messages, "context": liveContext, "model": model,
	}, &out)
	return out.Text, err
}
