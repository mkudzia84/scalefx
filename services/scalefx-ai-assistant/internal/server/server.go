// Package server is the AI-assistant HTTP API: model aggregation, chat
// forwarding (grounded in the embedded textbook), the curated FAQ, JWT auth, and
// per-IP rate limiting.
package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"sort"
	"strings"
	"sync/atomic"
	"time"

	"scalefx-ai-assistant/internal/assistant"
	"scalefx-ai-assistant/internal/auth"
	"scalefx-ai-assistant/internal/config"
	"scalefx-ai-assistant/internal/genai"
	"scalefx-ai-assistant/internal/ratelimit"
)

type modelInfo struct {
	ID       string `json:"id"`
	Provider string `json:"provider"`
	Label    string `json:"label"`
}

// Server holds the built provider map + aggregated model list.
//
// It is safe for concurrent use: net/http serves each request on its own
// goroutine; the provider/model/trusted maps are read-only after New, the rate
// limiter is mutex-guarded, and seq is atomic.
type Server struct {
	cfg       *config.Config
	limiter   *ratelimit.Limiter
	models    []modelInfo
	providers map[string]genai.Provider // model id -> provider (token + model baked in)
	trusted   map[string]bool
	seq       atomic.Uint64 // per-request id source (for log correlation)
}

// ctxKey namespaces request-scoped values (the request id).
type ctxKey int

const reqIDKey ctxKey = 0

func reqIDOf(ctx context.Context) uint64 {
	if v, ok := ctx.Value(reqIDKey).(uint64); ok {
		return v
	}
	return 0
}

// New builds the server from config (constructs a provider per offered model).
func New(cfg *config.Config) (*Server, error) {
	s := &Server{
		cfg:       cfg,
		limiter:   ratelimit.New(cfg.RateLimit.PerMinute),
		providers: map[string]genai.Provider{},
		trusted:   map[string]bool{},
	}
	for _, ip := range cfg.TrustedProxies {
		s.trusted[ip] = true
	}
	for _, p := range cfg.Providers {
		for _, m := range p.Models {
			prov, err := buildProvider(p.ID, p.Token, m)
			if err != nil {
				return nil, err
			}
			if _, dup := s.providers[m]; dup {
				return nil, fmt.Errorf("model %q is configured under more than one provider", m)
			}
			s.providers[m] = prov
			s.models = append(s.models, modelInfo{ID: m, Provider: p.ID, Label: m})
		}
	}
	sort.Slice(s.models, func(i, j int) bool { return s.models[i].ID < s.models[j].ID })
	return s, nil
}

func buildProvider(providerID, token, model string) (genai.Provider, error) {
	switch providerID {
	case "gemini":
		return genai.NewGemini(token, model), nil
	case "mistral":
		return genai.NewMistral(token, model), nil
	default:
		return nil, fmt.Errorf("unknown provider %q (supported: gemini, mistral)", providerID)
	}
}

// Handler returns the routed + middleware-wrapped HTTP handler. The logging
// middleware is outermost so every request — including auth/ratelimit rejections
// — gets one completion line with a correlation id, status, and latency.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", s.handleHealth)
	mux.Handle("/v1/models", s.protect(false, http.HandlerFunc(s.handleModels)))
	mux.Handle("/v1/faq", s.protect(false, http.HandlerFunc(s.handleFAQ)))
	mux.Handle("/v1/chat", s.protect(true, http.HandlerFunc(s.handleChat)))
	return s.withLogging(mux)
}

// statusRec captures the status code + bytes written so the logging middleware
// can report them after the handler returns.
type statusRec struct {
	http.ResponseWriter
	status int
	bytes  int
}

func (r *statusRec) WriteHeader(c int) { r.status = c; r.ResponseWriter.WriteHeader(c) }
func (r *statusRec) Write(b []byte) (int, error) {
	n, err := r.ResponseWriter.Write(b)
	r.bytes += n
	return n, err
}

// withLogging assigns each request a correlation id, times it, and logs a
// completion line. In verbose mode it also logs an arrival line and the
// liveness probe (/healthz, otherwise suppressed to keep the log readable).
func (s *Server) withLogging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := s.seq.Add(1)
		r = r.WithContext(context.WithValue(r.Context(), reqIDKey, id))
		ip := s.clientIP(r)
		if s.cfg.Verbose {
			log.Printf("[req #%d] >> %s %s ip=%s", id, r.Method, r.URL.Path, ip)
		}
		rec := &statusRec{ResponseWriter: w, status: http.StatusOK}
		start := time.Now()
		next.ServeHTTP(rec, r)
		if r.URL.Path == "/healthz" && !s.cfg.Verbose {
			return // don't spam the log with liveness probes
		}
		log.Printf("[req #%d] %s %s ip=%s -> %d (%dms, %dB)",
			id, r.Method, r.URL.Path, ip, rec.status, time.Since(start).Milliseconds(), rec.bytes)
	})
}

// protect = auth (always) + rate limit (chat). Logging is handled by withLogging.
func (s *Server) protect(limited bool, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := reqIDOf(r.Context())
		ip := s.clientIP(r)
		if _, err := auth.Verify(bearer(r)); err != nil {
			log.Printf("[auth #%d] ip=%s %s rejected: %v", id, ip, r.URL.Path, err)
			httpError(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		if limited && !s.limiter.Allow(ip) {
			log.Printf("[ratelimit #%d] ip=%s %s over %d/min", id, ip, r.URL.Path, s.cfg.RateLimit.PerMinute)
			httpError(w, http.StatusTooManyRequests, "rate limit exceeded — try again shortly")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ok"))
}

func (s *Server) handleModels(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.models)
}

func (s *Server) handleFAQ(w http.ResponseWriter, _ *http.Request) {
	items := assistant.FAQItems()
	type faqDTO struct {
		Question string `json:"question"`
		Answer   string `json:"answer"`
	}
	out := make([]faqDTO, 0, len(items))
	for _, it := range items {
		out = append(out, faqDTO{Question: it.Question, Answer: it.Answer})
	}
	writeJSON(w, http.StatusOK, out)
}

type chatReq struct {
	Messages []struct {
		Role    string `json:"role"`
		Content string `json:"content"`
	} `json:"messages"`
	Context string `json:"context"`
	Model   string `json:"model"`
}

func (s *Server) handleChat(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		httpError(w, http.StatusMethodNotAllowed, "POST only")
		return
	}
	var req chatReq
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		httpError(w, http.StatusBadRequest, "bad request body")
		return
	}
	model := req.Model
	prov := s.providers[model]
	if prov == nil {
		if len(s.models) == 0 {
			httpError(w, http.StatusServiceUnavailable, "The assistant is not configured yet. Please contact the administrator.")
			return
		}
		model = s.models[0].ID // requested model not offered — fall back to the first
		prov = s.providers[model]
	}

	msgs := make([]genai.Message, 0, len(req.Messages))
	for _, m := range req.Messages {
		role := genai.RoleUser
		if m.Role == "model" || m.Role == "assistant" {
			role = genai.RoleModel
		}
		msgs = append(msgs, genai.Message{Role: role, Content: m.Content})
	}

	// Privacy: we log WHO + HOW MUCH + outcome, never WHAT. The question text and
	// the reply are never written to the log (no content, even in verbose mode).
	id := reqIDOf(r.Context())
	ip := s.clientIP(r)
	log.Printf("[chat #%d] ip=%s model=%s rate=%d/%d msgs=%d ctx=%dB",
		id, ip, model, s.limiter.Count(ip), s.cfg.RateLimit.PerMinute, len(msgs), len(req.Context))

	start := time.Now()
	text, err := assistant.New(prov).Ask(r.Context(), req.Context, msgs)
	took := time.Since(start)
	if err != nil {
		// Log the full detail server-side (incl. the raw upstream body carried
		// by genai.APIError); return only a sanitized, human-friendly message so
		// backend internals (provider, token state, status, upstream text) never
		// reach the client.
		code, msg := friendlyChatError(r.Context(), err)
		log.Printf("[chat #%d] FAIL ip=%s model=%s after %dms -> %d: %v",
			id, ip, model, took.Milliseconds(), code, err)
		httpError(w, code, msg)
		return
	}
	log.Printf("[chat #%d] OK ip=%s model=%s replyBytes=%d in %dms", id, ip, model, len(text), took.Milliseconds())
	writeJSON(w, http.StatusOK, map[string]string{"text": text, "model": model})
}

// friendlyChatError maps an internal chat failure to a (status, user-message)
// pair safe to send to the client. It never echoes err.Error().
func friendlyChatError(ctx context.Context, err error) (int, string) {
	// We ran out of time, or the client went away mid-request.
	if errors.Is(err, context.DeadlineExceeded) || ctx.Err() != nil {
		return http.StatusGatewayTimeout, "The assistant took too long to respond. Please try again."
	}
	if errors.Is(err, context.Canceled) {
		return http.StatusGatewayTimeout, "The request was cancelled before the assistant could answer."
	}
	// Provider safety filter refused, or returned nothing usable.
	if errors.Is(err, genai.ErrBlocked) {
		return http.StatusUnprocessableEntity, "The assistant couldn't answer that. Try rephrasing your question."
	}
	if errors.Is(err, genai.ErrEmpty) {
		return http.StatusBadGateway, "The assistant didn't return an answer. Please try again."
	}
	// Structured upstream HTTP failure — classify by status.
	var apiErr *genai.APIError
	if errors.As(err, &apiErr) {
		switch {
		case apiErr.Status == http.StatusUnauthorized || apiErr.Status == http.StatusForbidden:
			// Our credentials are bad — a server config problem, not the user's.
			return http.StatusBadGateway, "The assistant is unavailable right now due to a server configuration issue. Please try again later or contact the administrator."
		case apiErr.Status == http.StatusTooManyRequests:
			return http.StatusServiceUnavailable, "The assistant is busy at the moment. Please wait a few seconds and try again."
		case apiErr.Status >= 500:
			return http.StatusBadGateway, "The AI provider is temporarily unavailable. Please try again shortly."
		default:
			return http.StatusBadGateway, "The assistant couldn't complete your request. Please try again."
		}
	}
	// Anything else (marshal / transport / decode) — generic, no internals.
	return http.StatusBadGateway, "The assistant couldn't process your request. Please try again."
}

// clientIP honors X-Forwarded-For only when the direct peer is a trusted proxy.
func (s *Server) clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	if s.trusted[host] {
		if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
			return strings.TrimSpace(strings.Split(xff, ",")[0])
		}
	}
	return host
}

func bearer(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if strings.HasPrefix(h, "Bearer ") {
		return strings.TrimSpace(strings.TrimPrefix(h, "Bearer "))
	}
	return ""
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func httpError(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}
