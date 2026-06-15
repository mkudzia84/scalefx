// Command ai-assistant is the standalone ScaleFX AI-assistant service: it holds
// the provider API tokens, the embedded textbook, model routing, JWT auth, and
// per-IP rate limiting, and exposes a small REST API the ScaleFX Studio client
// talks to.
package main

import (
	"flag"
	"log"
	"net/http"
	"time"

	"scalefx-ai-assistant/internal/config"
	"scalefx-ai-assistant/internal/server"
)

func main() {
	cfgPath := flag.String("config", "config.yaml", "path to the YAML config")
	flag.Parse()

	cfg, err := config.Load(*cfgPath)
	if err != nil {
		log.Fatalf("config: %v", err)
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
	log.Printf("ScaleFX AI-assistant listening on %s (rate limit %d/min/IP)", cfg.Listen, cfg.RateLimit.PerMinute)
	if err := httpSrv.ListenAndServe(); err != nil {
		log.Fatalf("listen: %v", err)
	}
}
