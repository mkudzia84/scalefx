module scalefx/tests/host/go_integration/lifecycle_test

go 1.21

require scalefx v0.0.0

require scalefx/tests/host/ports v0.0.0

require (
	github.com/creack/goselect v0.1.2 // indirect
	go.bug.st/serial v1.6.2 // indirect
	golang.org/x/sys v0.19.0 // indirect
)

replace scalefx => ../../../../app/go

replace scalefx/tests/host/ports => ../../ports
