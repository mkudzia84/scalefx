//go:build !debug

package engine

// DebugBuild is false in release builds. Build with `-tags debug` to enable
// engine-level Debug() tracing on the Output sink.
const DebugBuild = false
