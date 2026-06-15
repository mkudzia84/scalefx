// Package genai is a small, provider-agnostic interface for talking to a
// generative-AI chat model.  It is intentionally decoupled from anything
// ScaleFX-specific so it can be reused by any Studio feature wanting an LLM
// round-trip (the config assistant is the first consumer).
//
// A Provider takes a system prompt + a conversation and returns the model's
// reply.  Today there is one implementation (Gemini); the interface keeps the
// door open for others (an OpenAI-compatible endpoint, a local Ollama server,
// a hosted proxy) without touching callers.
package genai

import (
	"context"
	"errors"
)

// Role identifies who authored a message in a conversation.
type Role string

const (
	RoleUser  Role = "user"
	RoleModel Role = "model"
)

// Message is one turn in a conversation.
type Message struct {
	Role    Role   `json:"role"`
	Content string `json:"content"`
}

// Provider is a generative chat backend.  Implementations must be safe for
// concurrent use.
type Provider interface {
	// Name is a short human label ("Gemini") for status / UX.
	Name() string
	// Model is the concrete model id in use (for status display).
	Model() string
	// Generate sends the system prompt + conversation and returns the reply
	// text.  It blocks until the model responds or ctx is cancelled.
	Generate(ctx context.Context, system string, history []Message) (string, error)
}

// ErrNoKey is returned when a provider has no API key configured.
var ErrNoKey = errors.New("genai: no API key configured")
