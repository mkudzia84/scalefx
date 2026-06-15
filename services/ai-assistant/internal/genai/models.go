package genai

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"
)

// ModelInfo is one chat-capable model a key can access.
type ModelInfo struct {
	ID          string `json:"id"`          // e.g. "gemini-2.5-flash" (used in requests)
	DisplayName string `json:"displayName"` // e.g. "Gemini 2.5 Flash"
}

// ListGeminiModels queries the key's available models and returns those that
// support generateContent (i.e. usable for chat), newest-id first.  Paginates
// defensively.  Lets the UI offer a real dropdown instead of a guessed id.
func ListGeminiModels(ctx context.Context, apiKey string) ([]ModelInfo, error) {
	if apiKey == "" {
		return nil, ErrNoKey
	}
	client := &http.Client{Timeout: 20 * time.Second}

	var result []ModelInfo
	pageToken := ""
	for page := 0; page < 8; page++ { // cap pages — the catalog is small
		url := fmt.Sprintf("https://generativelanguage.googleapis.com/v1beta/models?key=%s&pageSize=200", apiKey)
		if pageToken != "" {
			url += "&pageToken=" + pageToken
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
		if err != nil {
			return nil, err
		}
		resp, err := client.Do(req)
		if err != nil {
			return nil, fmt.Errorf("list models: %w", err)
		}
		raw, _ := io.ReadAll(resp.Body)
		resp.Body.Close()

		var out struct {
			Models []struct {
				Name                       string   `json:"name"`
				DisplayName                string   `json:"displayName"`
				SupportedGenerationMethods []string `json:"supportedGenerationMethods"`
			} `json:"models"`
			NextPageToken string `json:"nextPageToken"`
			Error         *struct {
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

		for _, m := range out.Models {
			if !supportsGenerate(m.SupportedGenerationMethods) {
				continue
			}
			result = append(result, ModelInfo{
				ID:          strings.TrimPrefix(m.Name, "models/"),
				DisplayName: m.DisplayName,
			})
		}
		if out.NextPageToken == "" {
			break
		}
		pageToken = out.NextPageToken
	}

	// Newest id first (gemini-2.5-* above gemini-1.5-*).
	sort.Slice(result, func(i, j int) bool { return result[i].ID > result[j].ID })
	return result, nil
}

func supportsGenerate(methods []string) bool {
	for _, m := range methods {
		if m == "generateContent" {
			return true
		}
	}
	return false
}
