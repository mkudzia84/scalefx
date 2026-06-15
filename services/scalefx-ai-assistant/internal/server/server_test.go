package server

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"scalefx-ai-assistant/internal/auth"
	"scalefx-ai-assistant/internal/config"
	"scalefx-ai-assistant/internal/genai"
)

// stubProvider lets us exercise the chat route without real API calls.
type stubProvider struct{ reply string }

func (stubProvider) Name() string  { return "stub" }
func (stubProvider) Model() string { return "stub-model" }
func (s stubProvider) Generate(_ context.Context, _ string, _ []genai.Message) (string, error) {
	return s.reply, nil
}

// errProvider always fails with a chosen error — for the sanitization tests.
type errProvider struct{ err error }

func (errProvider) Name() string  { return "stub" }
func (errProvider) Model() string { return "stub-model" }
func (p errProvider) Generate(_ context.Context, _ string, _ []genai.Message) (string, error) {
	return "", p.err
}

func testServer(t *testing.T) *Server {
	t.Helper()
	s, err := New(&config.Config{
		Listen:    ":0",
		RateLimit: config.RateLimit{PerMinute: 2},
		Providers: []config.Provider{
			{ID: "gemini", Token: "x", Models: []string{"gemini-2.5-flash"}},
			{ID: "mistral", Token: "y", Models: []string{"mistral-small-latest"}},
		},
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return s
}

func devToken() string { return auth.Sign(auth.Secret, auth.Claims{Sub: "test"}) }

func TestModelsRequiresAuthAndAggregates(t *testing.T) {
	h := testServer(t).Handler()

	// No token -> 401.
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/models", nil))
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("no-token /v1/models: got %d, want 401", rec.Code)
	}

	// With token -> 200 + both models, sorted.
	req := httptest.NewRequest(http.MethodGet, "/v1/models", nil)
	req.Header.Set("Authorization", "Bearer "+devToken())
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("/v1/models: got %d, want 200", rec.Code)
	}
	var models []modelInfo
	if err := json.Unmarshal(rec.Body.Bytes(), &models); err != nil {
		t.Fatal(err)
	}
	if len(models) != 2 {
		t.Fatalf("aggregated models: got %d, want 2 (%+v)", len(models), models)
	}
}

func TestFAQServed(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/v1/faq", nil)
	req.Header.Set("Authorization", "Bearer "+devToken())
	rec := httptest.NewRecorder()
	testServer(t).Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("/v1/faq: got %d, want 200", rec.Code)
	}
	var faq []map[string]string
	if err := json.Unmarshal(rec.Body.Bytes(), &faq); err != nil {
		t.Fatal(err)
	}
	if len(faq) == 0 || faq[0]["question"] == "" {
		t.Fatalf("/v1/faq returned no usable Q&A: %+v", faq)
	}
}

func TestChatRoutesToProvider(t *testing.T) {
	s := testServer(t)
	s.providers["gemini-2.5-flash"] = stubProvider{reply: "stubbed answer"} // inject

	body, _ := json.Marshal(chatReq{Model: "gemini-2.5-flash", Context: "ctx"})
	req := httptest.NewRequest(http.MethodPost, "/v1/chat", bytes.NewReader(body))
	req.Header.Set("Authorization", "Bearer "+devToken())
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("/v1/chat: got %d, want 200 (%s)", rec.Code, rec.Body.String())
	}
	var out map[string]string
	_ = json.Unmarshal(rec.Body.Bytes(), &out)
	if out["text"] != "stubbed answer" {
		t.Fatalf("chat text: got %q, want %q", out["text"], "stubbed answer")
	}
}

func TestRateLimit(t *testing.T) {
	s := testServer(t) // PerMinute: 2
	s.providers["gemini-2.5-flash"] = stubProvider{reply: "ok"}

	codes := make([]int, 3)
	for i := 0; i < 3; i++ {
		body, _ := json.Marshal(chatReq{Model: "gemini-2.5-flash"})
		req := httptest.NewRequest(http.MethodPost, "/v1/chat", bytes.NewReader(body))
		req.Header.Set("Authorization", "Bearer "+devToken())
		req.RemoteAddr = "203.0.113.7:5555" // same IP
		rec := httptest.NewRecorder()
		s.Handler().ServeHTTP(rec, req)
		codes[i] = rec.Code
	}
	if codes[0] != 200 || codes[1] != 200 || codes[2] != http.StatusTooManyRequests {
		t.Fatalf("rate-limit codes = %v, want [200 200 429]", codes)
	}
}

func TestBadTokenRejected(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/v1/models", nil)
	req.Header.Set("Authorization", "Bearer not.a.jwt")
	rec := httptest.NewRecorder()
	testServer(t).Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("bad token: got %d, want 401", rec.Code)
	}
}

// chatErr POSTs a chat request and returns the status + the {error} field.
func chatErr(t *testing.T, s *Server) (int, string) {
	t.Helper()
	body, _ := json.Marshal(chatReq{Model: "gemini-2.5-flash"})
	req := httptest.NewRequest(http.MethodPost, "/v1/chat", bytes.NewReader(body))
	req.Header.Set("Authorization", "Bearer "+devToken())
	rec := httptest.NewRecorder()
	s.Handler().ServeHTTP(rec, req)
	var out map[string]string
	_ = json.Unmarshal(rec.Body.Bytes(), &out)
	return rec.Code, out["error"]
}

func TestChatErrorIsSanitized(t *testing.T) {
	s := testServer(t)
	// An upstream 401 carrying a raw provider message + token hint.
	s.providers["gemini-2.5-flash"] = errProvider{err: &genai.APIError{
		Provider: "mistral", Status: 401, Body: "Invalid API key sk-secret-123",
	}}

	code, msg := chatErr(t, s)
	if code != http.StatusBadGateway {
		t.Fatalf("upstream-401 status: got %d, want 502", code)
	}
	// The client message must NOT leak provider name, status code, token, or
	// the word "http".
	for _, leak := range []string{"mistral", "401", "Invalid API key", "sk-secret", "http"} {
		if strings.Contains(strings.ToLower(msg), strings.ToLower(leak)) {
			t.Fatalf("sanitized message leaks %q: %q", leak, msg)
		}
	}
	if msg == "" {
		t.Fatal("expected a friendly error message, got empty")
	}
}

func TestChatRateLimitUpstreamMapped(t *testing.T) {
	s := testServer(t)
	s.providers["gemini-2.5-flash"] = errProvider{err: &genai.APIError{Provider: "gemini", Status: 429}}
	code, msg := chatErr(t, s)
	if code != http.StatusServiceUnavailable {
		t.Fatalf("upstream-429 status: got %d, want 503", code)
	}
	if strings.Contains(msg, "429") || msg == "" {
		t.Fatalf("unexpected 429 message: %q", msg)
	}
}

func TestChatBlockedMapped(t *testing.T) {
	s := testServer(t)
	s.providers["gemini-2.5-flash"] = errProvider{err: genai.ErrBlocked}
	code, msg := chatErr(t, s)
	if code != http.StatusUnprocessableEntity {
		t.Fatalf("blocked status: got %d, want 422", code)
	}
	if msg == "" {
		t.Fatal("expected a friendly blocked message")
	}
}
