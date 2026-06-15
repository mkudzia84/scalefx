// Package config loads the AI-assistant service config from YAML.
package config

import (
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// Provider is one upstream model provider + its API token + the models it offers.
type Provider struct {
	ID     string   `yaml:"id"`     // "gemini" | "mistral"
	Token  string   `yaml:"token"`  // the provider API key (secret — keep out of git)
	Models []string `yaml:"models"` // the allow-listed model ids exposed for this provider
}

// RateLimit caps model requests per client IP.
type RateLimit struct {
	PerMinute int `yaml:"perMinute"`
}

// Config is the full service configuration.
type Config struct {
	Listen         string     `yaml:"listen"`         // e.g. ":8080"
	TrustedProxies []string   `yaml:"trustedProxies"` // proxy IPs whose X-Forwarded-For we trust
	RateLimit      RateLimit  `yaml:"rateLimit"`
	Providers      []Provider `yaml:"providers"`
}

// Load reads + validates the YAML config, applying sane defaults.
func Load(path string) (*Config, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config %s: %w", path, err)
	}
	var c Config
	if err := yaml.Unmarshal(b, &c); err != nil {
		return nil, fmt.Errorf("parse config %s: %w", path, err)
	}
	if c.Listen == "" {
		c.Listen = ":8080"
	}
	if c.RateLimit.PerMinute <= 0 {
		c.RateLimit.PerMinute = 5
	}
	if len(c.Providers) == 0 {
		return nil, fmt.Errorf("config %s: no providers configured", path)
	}
	for i, p := range c.Providers {
		if p.ID == "" || p.Token == "" || len(p.Models) == 0 {
			return nil, fmt.Errorf("config %s: provider[%d] needs id, token, and models", path, i)
		}
	}
	return &c, nil
}
