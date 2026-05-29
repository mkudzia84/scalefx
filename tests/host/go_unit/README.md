# tests/host/go_unit/

Fast Go unit tests for `app/go/*` — pure logic, no hardware required.
Each subdirectory is its own Go module with
`replace scalefx => ../../../../app/go`.

Run all: `for d in */; do (cd "$d" && go test ./...); done`
Run one: `cd protocol_test && go test ./...`

See [../../README.md](../../README.md) for the full test layout.
