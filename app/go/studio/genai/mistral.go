package genai

import "context"

// Mistral exposes an OpenAI-compatible API, so it rides the generic
// OpenAIProvider — only the base URL + default model differ.
const (
	MistralBaseURL      = "https://api.mistral.ai/v1"
	DefaultMistralModel = "mistral-small-latest"
)

// BuiltinMistralKey — optional build-time Mistral key (same semantics +
// caveats as BuiltinKey; inject via -ldflags "-X studio/genai.BuiltinMistralKey=...").
var BuiltinMistralKey string

// NewMistral builds a Mistral chat provider.  An empty model uses the default.
func NewMistral(apiKey, model string) *OpenAIProvider {
	if model == "" {
		model = DefaultMistralModel
	}
	return NewOpenAICompat("Mistral", MistralBaseURL, apiKey, model)
}

// ListMistralModels lists the chat models the Mistral key can access.
func ListMistralModels(ctx context.Context, apiKey string) ([]ModelInfo, error) {
	return ListOpenAIModels(ctx, MistralBaseURL, apiKey)
}
