# ScaleFX AI-assistant service

A standalone Go service that fronts the GenAI providers for ScaleFX Studio. It
holds the provider API tokens, the embedded textbook + guardrail, model routing,
JWT auth, and per-IP rate limiting, so the desktop client never carries provider
secrets. Studio talks to it over a small REST API.

## API (JSON)

| Method | Path | Auth | Purpose |
|--------|------|------|---------|
| `GET`  | `/healthz`    | no  | liveness |
| `GET`  | `/v1/models`  | yes | aggregated `[{id, provider, label}]` across configured providers |
| `GET`  | `/v1/faq`     | yes | curated FAQ `[{question, answer}]` (from the textbook) |
| `POST` | `/v1/chat`    | yes + rate-limited | `{messages, context, model}` → `{text, model}` (grounded in textbook + guardrail + the client's live context) |
| `POST` | `/v1/summarize` | yes + rate-limited | `{messages, model}` → `{summary, model}` — compresses a run of turns into a compact summary the client keeps as its new history (long-chat cost control). Stateless: nothing is stored. |

Auth = `Authorization: Bearer <JWT>` (HS256, see below). `/v1/chat` is rate-limited
per client IP (default 5/min, configurable).

**Errors are sanitized.** Upstream/provider failures are logged in full
server-side (provider, status, raw body) but the client only ever receives a
generic, human-friendly `{"error": "…"}` — never a provider name, status code,
token state, or raw upstream text. E.g. a bad provider key → the client sees
"The assistant is unavailable right now due to a server configuration issue…",
while the log shows `[chat] model=… error: Mistral: http 401`.

## Configure

Copy `config.example.yaml` → `config.yaml` (gitignored) and fill in real tokens:

```yaml
listen: ":8080"
trustedProxies: []          # OVHcloud LB IP(s) → trust X-Forwarded-For for the real client IP
rateLimit: { perMinute: 5 }
providers:
  - { id: gemini,  token: "AIza…", models: [gemini-2.5-flash, gemini-3.5-flash] }
  - { id: mistral, token: "…",     models: [mistral-small-latest, mistral-medium-latest, mistral-large-latest] }
```

## JWT (shared with Studio)

HS256. The service verifies the bearer token's signature with a secret; Studio
ships a token signed with that secret. Generate a production pair:

```sh
go run ./tools/gen-token            # prints a random secret + a 5-year token
```

Build with the secret injected (the client builds with the token injected):

```sh
go build -ldflags "-X scalefx-ai-assistant/internal/auth.Secret=<secret>" -o scalefx-ai-assistant .
```

A committed **dev default** (`scalefx-dev-secret-change-me` + a matching token in
Studio) makes `localhost` work out of the box. **Rotate for any real deployment** —
an embedded secret is extractable from a shipped binary.

## Run

```sh
go build -o scalefx-ai-assistant .
./scalefx-ai-assistant -config config.yaml          # add -verbose for full request logging
```

Concurrency: the server handles many simultaneous users out of the box —
`net/http` serves each request on its own goroutine; shared state is read-only
after startup except the mutex-guarded rate limiter (which also self-evicts idle
IPs to bound memory).

## Logging

The log records **who, how much, and the outcome — never the content**. The
question text and the assistant's reply are never written to the log, in any mode.

Every request logs one completion line with a correlation id, client IP, status,
latency, and byte count, e.g. `[req #42] POST /v1/chat ip=… -> 200 (1310ms, 980B)`.
Chat adds `[chat #42] ip=… model=… rate=3/5 msgs=… ctx=…B` (rate = that IP's
requests used in the current minute / the limit) and an `OK`/`FAIL` line (latency
+, on failure, the full upstream error). `verbose: true` (or `-verbose`) only adds
request-arrival lines and `/healthz` probes for tracing — still no content. Client
responses are always sanitized regardless of log verbosity.

## Deploy (Docker / OVHcloud)

```sh
docker build --build-arg JWT_SECRET=<secret> -t scalefx-ai .
docker run -p 8080:8080 -v /path/config.yaml:/config.yaml scalefx-ai
```

Mount `config.yaml` (with the real tokens) as a secret/volume — never bake it
into the image. Behind a load balancer, set `trustedProxies` to the LB IP so the
rate limiter sees real client IPs.

## Tests

```sh
go test ./...
```

`internal/server` covers model aggregation, chat routing (stub provider), the
FAQ, auth rejection, and the rate limiter.
