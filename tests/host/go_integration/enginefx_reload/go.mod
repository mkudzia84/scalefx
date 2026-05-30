module scalefx/tests/host/go_integration/enginefx_reload

go 1.21

require (
	gopkg.in/yaml.v3 v3.0.1
	scalefx v0.0.0
)

require (
	github.com/creack/goselect v0.1.2 // indirect
	go.bug.st/serial v1.6.2 // indirect
	golang.org/x/sys v0.19.0 // indirect
)

replace scalefx => ../../../../app/go
