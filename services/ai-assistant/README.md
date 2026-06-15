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

Auth = `Authorization: Bearer <JWT>` (HS256, see below). `/v1/chat` is rate-limited
per client IP (default 5/min, configurable).

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
go build -ldflags "-X scalefx/ai-assistant/internal/auth.Secret=<secret>" -o ai-assistant .
```

A committed **dev default** (`scalefx-dev-secret-change-me` + a matching token in
Studio) makes `localhost` work out of the box. **Rotate for any real deployment** —
an embedded secret is extractable from a shipped binary.

## Run

```sh
go build -o ai-assistant .
./ai-assistant -config config.yaml
```

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
