// Package auth implements minimal HS256 JWT signing + verification (no external
// dependency).  The service holds the shared Secret; the ScaleFX Studio client
// holds a token signed with it and sends it as `Authorization: Bearer <token>`.
package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

// Secret is the HS256 signing secret, shared between this service and the
// client.  Override at build time: -ldflags "-X scalefx/ai-assistant/internal/auth.Secret=...".
// The committed default makes localhost work out of the box — CHANGE for prod.
var Secret = "scalefx-dev-secret-change-me"

// Claims is the (small) JWT payload.
type Claims struct {
	Sub string `json:"sub,omitempty"`
	Iss string `json:"iss,omitempty"`
	Exp int64  `json:"exp,omitempty"` // unix seconds; 0 = no expiry
}

// Sign mints an HS256 JWT for the claims using the given secret (gen-token tool).
func Sign(secret string, claims Claims) string {
	header := base64url([]byte(`{"alg":"HS256","typ":"JWT"}`))
	pj, _ := json.Marshal(claims)
	signing := header + "." + base64url(pj)
	return signing + "." + base64url(hmacSHA256(secret, signing))
}

// Verify checks the token's HS256 signature against Secret and the expiry.
func Verify(token string) (*Claims, error) {
	parts := strings.Split(strings.TrimSpace(token), ".")
	if len(parts) != 3 {
		return nil, errors.New("malformed token")
	}
	signing := parts[0] + "." + parts[1]
	want := base64url(hmacSHA256(Secret, signing))
	if subtle.ConstantTimeCompare([]byte(want), []byte(parts[2])) != 1 {
		return nil, errors.New("bad signature")
	}
	pj, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, errors.New("bad payload encoding")
	}
	var c Claims
	if err := json.Unmarshal(pj, &c); err != nil {
		return nil, errors.New("bad claims")
	}
	if c.Exp != 0 && time.Now().Unix() > c.Exp {
		return nil, errors.New("token expired")
	}
	return &c, nil
}

func hmacSHA256(secret, msg string) []byte {
	m := hmac.New(sha256.New, []byte(secret))
	m.Write([]byte(msg))
	return m.Sum(nil)
}

func base64url(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }
