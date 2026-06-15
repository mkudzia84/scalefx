// Command gen-token prints an HS256 secret + a long-lived JWT signed with it,
// for build-time injection (service gets the secret, Studio gets the token).
//
//	go run ./tools/gen-token                 # random secret + 5-year token
//	go run ./tools/gen-token -secret X       # token for an existing secret
package main

import (
	"crypto/rand"
	"encoding/hex"
	"flag"
	"fmt"
	"time"

	"scalefx-ai-assistant/internal/auth"
)

func main() {
	secret := flag.String("secret", "", "HS256 secret (random if empty)")
	years := flag.Int("years", 5, "token validity in years (0 = no expiry)")
	flag.Parse()

	s := *secret
	if s == "" {
		b := make([]byte, 32)
		_, _ = rand.Read(b)
		s = hex.EncodeToString(b)
	}
	var exp int64
	if *years > 0 {
		exp = time.Now().AddDate(*years, 0, 0).Unix()
	}
	tok := auth.Sign(s, auth.Claims{Sub: "scalefx-studio", Iss: "scalefx-ai-assistant", Exp: exp})

	fmt.Printf("secret: %s\n", s)
	fmt.Printf("token:  %s\n", tok)
	fmt.Println()
	fmt.Println("Service:  -ldflags \"-X scalefx-ai-assistant/internal/auth.Secret=<secret>\"")
	fmt.Println("Studio:   -ldflags \"-X studio/aiclient.AuthToken=<token>\"")
}
