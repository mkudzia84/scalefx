package genai

import "context"

// Groq exposes an OpenAI-compatible API, so it rides the generic
// OpenAIProvider — only the base URL + default model differ.
const (
	GroqBaseURL      = "https://api.groq.com/openai/v1"
	DefaultGroqModel = "llama-3.3-70b-versatile"
)

// BuiltinGroqKey — optional build-time Groq key (same semantics + caveats as
// BuiltinKey; inject via -ldflags "-X studio/genai.BuiltinGroqKey=...").
var BuiltinGroqKey string

// NewGroq builds a Groq chat provider.  An empty model uses DefaultGroqModel.
func NewGroq(apiKey, model string) *OpenAIProvider {
	if model == "" {
		model = DefaultGroqModel
	}
	return NewOpenAICompat("Groq", GroqBaseURL, apiKey, model)
}

// ListGroqModels lists the chat models the Groq key can access.
func ListGroqModels(ctx context.Context, apiKey string) ([]ModelInfo, error) {
	return ListOpenAIModels(ctx, GroqBaseURL, apiKey)
}
