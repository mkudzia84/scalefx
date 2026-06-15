package main

import (
	"context"
	"time"

	"studio/aiclient"
)

// The AI assistant is a standalone service now (services/scalefx-ai-assistant).  These
// bindings are a thin proxy: Studio builds the live-config context in the
// frontend and forwards it + the conversation to the service over REST (the
// service owns the provider tokens, textbook, model routing, and rate limiting).

type AssistantModelDTO struct {
	ID       string `json:"id"`
	Provider string `json:"provider"`
	Label    string `json:"label"`
}

// AssistantStatusDTO tells the panel whether the service is reachable + its models.
type AssistantStatusDTO struct {
	Available bool                `json:"available"`
	Endpoint  string              `json:"endpoint"`
	Models    []AssistantModelDTO `json:"models"`
	Error     string              `json:"error,omitempty"`
}

type AssistantMessage struct {
	Role    string `json:"role"` // "user" | "model"
	Content string `json:"content"`
}

type AssistantReply struct {
	Text  string `json:"text"`
	Error string `json:"error"`
}

type FAQItemDTO struct {
	Question string `json:"question"`
	Answer   string `json:"answer"`
}

func (a *App) reqCtx(seconds int) (context.Context, context.CancelFunc) {
	parent := a.ctx
	if parent == nil {
		parent = context.Background()
	}
	return context.WithTimeout(parent, time.Duration(seconds)*time.Second)
}

func toModelDTOs(models []aiclient.Model) []AssistantModelDTO {
	out := make([]AssistantModelDTO, 0, len(models))
	for _, m := range models {
		out = append(out, AssistantModelDTO{ID: m.ID, Provider: m.Provider, Label: m.Label})
	}
	return out
}

// AssistantStatus reports whether the assistant service is reachable + its models.
func (a *App) AssistantStatus() AssistantStatusDTO {
	ctx, cancel := a.reqCtx(15)
	defer cancel()
	models, err := aiclient.Models(ctx)
	st := AssistantStatusDTO{Endpoint: aiclient.Endpoint}
	if err != nil {
		st.Error = err.Error()
		return st
	}
	st.Models = toModelDTOs(models)
	st.Available = len(st.Models) > 0
	return st
}

// ListAssistantModels returns the models the service offers (panel dropdown).
func (a *App) ListAssistantModels() ([]AssistantModelDTO, error) {
	ctx, cancel := a.reqCtx(15)
	defer cancel()
	models, err := aiclient.Models(ctx)
	if err != nil {
		return nil, err
	}
	return toModelDTOs(models), nil
}

// AssistantFAQ returns the curated FAQ from the service (the FAQ tab).
func (a *App) AssistantFAQ() []FAQItemDTO {
	ctx, cancel := a.reqCtx(15)
	defer cancel()
	items, err := aiclient.FAQ(ctx)
	if err != nil {
		a.diag.Error("FE.AI", "FAQ fetch failed: %v", err)
		return nil
	}
	out := make([]FAQItemDTO, 0, len(items))
	for _, it := range items {
		out = append(out, FAQItemDTO{Question: it.Question, Answer: it.Answer})
	}
	return out
}

// AssistantAsk forwards the conversation + live context + chosen model to the
// service and returns the grounded reply.
func (a *App) AssistantAsk(history []AssistantMessage, liveContext, model string) AssistantReply {
	msgs := make([]aiclient.Message, 0, len(history))
	for _, m := range history {
		msgs = append(msgs, aiclient.Message{Role: m.Role, Content: m.Content})
	}
	ctx, cancel := a.reqCtx(65)
	defer cancel()
	text, err := aiclient.Chat(ctx, msgs, liveContext, model)
	if err != nil {
		a.diag.Error("FE.AI", "assistant ask failed: %v", err)
		return AssistantReply{Error: err.Error()}
	}
	return AssistantReply{Text: text}
}
