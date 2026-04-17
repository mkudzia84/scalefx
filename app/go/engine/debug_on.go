//go:build debug

package engine

// DebugBuild is true when this binary was compiled with `-tags debug`.
// Output.Debug() calls only emit when this is true; release builds compile
// the bodies away via const-folding.
const DebugBuild = true
