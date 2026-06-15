// Command ai-assistant is the standalone ScaleFX AI-assistant service: it holds
// the provider API tokens, the embedded textbook, model routing, JWT auth, and
// per-IP rate limiting, and exposes a small REST API the ScaleFX Studio client
// talks to.
package main

import (
	"flag"
	"log"
	"net/http"
	"strings"
	"time"

	"scalefx-ai-assistant/internal/config"
	"scalefx-ai-assistant/internal/server"
)

func main() {
	cfgPath := flag.String("config", "config.yaml", "path to the YAML config")
	verbose := flag.Bool("verbose", false, "verbose logging (overrides config.verbose when set)")
	flag.Parse()

	cfg, err := config.Load(*cfgPath)
	if err != nil {
		log.Fatalf("config: %v", err)
	}
	// -verbose flag forces verbose on regardless of config.
	if *verbose {
		cfg.Verbose = true
	}
	srv, err := server.New(cfg)
	if err != nil {
		log.Fatalf("server: %v", err)
	}

	httpSrv := &http.Server{
		Addr:         cfg.Listen,
		Handler:      srv.Handler(),
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 90 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	// Startup summary: what's loaded + the knobs in effect.
	models := 0
	provIDs := make([]string, 0, len(cfg.Providers))
	for _, p := range cfg.Providers {
		models += len(p.Models)
		provIDs = append(provIDs, p.ID)
	}
	log.Printf("ScaleFX AI-assistant starting: listen=%s providers=[%s] models=%d rateLimit=%d/min/IP verbose=%t",
		cfg.Listen, strings.Join(provIDs, ","), models, cfg.RateLimit.PerMinute, cfg.Verbose)
	log.Printf("listening on %s", cfg.Listen)
	if err := httpSrv.ListenAndServe(); err != nil {
		log.Fatalf("listen: %v", err)
	}
}
