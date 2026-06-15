package genai

import (
	"errors"
	"fmt"
)

// APIError is a structured upstream-provider failure. Status drives the server's
// user-facing classification; Provider + Body are for SERVER LOGS ONLY and must
// never be forwarded to the client (they expose which provider/token failed and
// can echo raw upstream text).
type APIError struct {
	Provider string
	Status   int
	Body     string // raw upstream message, if any
}

func (e *APIError) Error() string {
	if e.Body != "" {
		return fmt.Sprintf("%s: http %d: %s", e.Provider, e.Status, e.Body)
	}
	return fmt.Sprintf("%s: http %d", e.Provider, e.Status)
}

// Sentinels for non-HTTP outcomes the server surfaces with a distinct message.
var (
	// ErrBlocked: the provider's safety filter refused to answer.
	ErrBlocked = errors.New("genai: response blocked by provider safety filter")
	// ErrEmpty: the provider returned a 200 with no usable content.
	ErrEmpty = errors.New("genai: provider returned an empty response")
)
