# Build
FROM golang:1.22-alpine AS build
WORKDIR /src
COPY . .
# Inject the production JWT secret at build time:
#   docker build --build-arg JWT_SECRET=... -t scalefx-ai .
ARG JWT_SECRET=scalefx-dev-secret-change-me
RUN CGO_ENABLED=0 go build -trimpath \
    -ldflags "-s -w -X scalefx/ai-assistant/internal/auth.Secret=${JWT_SECRET}" \
    -o /out/ai-assistant .

# Run
FROM gcr.io/distroless/static-debian12
COPY --from=build /out/ai-assistant /ai-assistant
# Provide /config.yaml via a mounted volume / secret at deploy time.
EXPOSE 8080
ENTRYPOINT ["/ai-assistant", "-config", "/config.yaml"]
