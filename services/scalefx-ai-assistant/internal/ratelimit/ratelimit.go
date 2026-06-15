// Package ratelimit is a tiny per-key (per-IP) rolling-window limiter for the
// assistant service's model requests.
package ratelimit

import (
	"sync"
	"time"
)

// Limiter allows up to perMinute events per key in any rolling 60-second window.
type Limiter struct {
	perMinute int
	mu        sync.Mutex
	hits      map[string][]time.Time
}

// New builds a limiter (perMinute <= 0 disables limiting).
func New(perMinute int) *Limiter {
	return &Limiter{perMinute: perMinute, hits: map[string][]time.Time{}}
}

// Allow records a hit for key and returns false if it would exceed the limit.
func (l *Limiter) Allow(key string) bool {
	if l.perMinute <= 0 {
		return true
	}
	now := time.Now()
	cutoff := now.Add(-time.Minute)

	l.mu.Lock()
	defer l.mu.Unlock()

	kept := l.hits[key][:0]
	for _, t := range l.hits[key] {
		if t.After(cutoff) {
			kept = append(kept, t)
		}
	}
	if len(kept) >= l.perMinute {
		l.hits[key] = kept
		return false
	}
	l.hits[key] = append(kept, now)
	return true
}
