package genai

// BuiltinKey is an OPTIONAL API key compiled into the binary at build time.
// It is intentionally EMPTY in source — the repo never carries a key.  Inject
// your own local key at build time:
//
//	go build   -ldflags "-X studio/genai.BuiltinKey=YOUR_KEY" ./...
//	wails build -ldflags "-X studio/genai.BuiltinKey=YOUR_KEY"
//
// (Keep the key in a gitignored local file / env var and pass it through —
// never hard-code it into this source, or GitHub + Google secret-scanning will
// auto-revoke it.)
//
// SECURITY: anything embedded in a distributed binary is extractable.  A
// built-in key is for PERSONAL / trusted-build convenience only — restrict it
// hard in Google Cloud (API-restricted + low daily quota) and treat it as
// disposable.  For public distribution, ship with this empty and let each user
// supply their own key (stored locally via the Studio settings).  See
// instructions/33-CONFIG-WIZARD.md §7.
var BuiltinKey string
