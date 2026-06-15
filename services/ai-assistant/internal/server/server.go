// Package server is the AI-assistant HTTP API: model aggregation, chat
// forwarding (grounded in the embedded textbook), the curated FAQ, JWT auth, and
// per-IP rate limiting.
package server

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"sort"
	"strings"

	"scalefx/ai-assistant/internal/assistant"
	"scalefx/ai-assistant/internal/auth"
	"scalefx/ai-assistant/internal/config"
	"scalefx/ai-assistant/internal/genai"
	"scalefx/ai-assistant/internal/ratelimit"
)

type modelInfo struct {
	ID       string `json:"id"`
	Provider string `json:"provider"`
	Label    string `json:"label"`
}

// Server holds the built provider map + aggregated model list.
type Server struct {
	cfg       *config.Config
	limiter   *ratelimit.Limiter
	models    []modelInfo
	providers map[string]genai.Provider // model id -> provider (token + model baked in)
	trusted   map[string]bool
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

// Handler returns the routed + middleware-wrapped HTTP handler.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", s.handleHealth)
	mux.Handle("/v1/models", s.protect(false, http.HandlerFunc(s.handleModels)))
	mux.Handle("/v1/faq", s.protect(false, http.HandlerFunc(s.handleFAQ)))
	mux.Handle("/v1/chat", s.protect(true, http.HandlerFunc(s.handleChat)))
	return mux
}

// protect = auth (always) + rate limit (chat) + IP logging.
func (s *Server) protect(limited bool, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ip := s.clientIP(r)
		if _, err := auth.Verify(bearer(r)); err != nil {
			log.Printf("[auth] %s %s -> 401 (%v)", ip, r.URL.Path, err)
			httpError(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		if limited && !s.limiter.Allow(ip) {
			log.Printf("[ratelimit] %s %s -> 429", ip, r.URL.Path)
			httpError(w, http.StatusTooManyRequests, "rate limit exceeded — try again shortly")
			return
		}
		log.Printf("[req] %s %s", ip, r.URL.Path)
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
			httpError(w, http.StatusServiceUnavailable, "no models configured")
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

	text, err := assistant.New(prov).Ask(r.Context(), req.Context, msgs)
	if err != nil {
		log.Printf("[chat] model=%s error: %v", model, err)
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"text": text, "model": model})
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
