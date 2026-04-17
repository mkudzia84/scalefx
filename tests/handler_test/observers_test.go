package handler_test

// Unit tests for engine.Observers[T] — the typed multicast helper that
// decouples decode+dispatch from CLI formatting and Studio event emission.

import (
	"fmt"
	"scalefx/engine"
	"strings"
	"testing"
)

// captureOutput implements engine.Output for inspecting Dispatch's diagnostic
// output path. Only Printf matters here; other methods are no-ops.
type captureOutput struct {
	buf strings.Builder
}

func (c *captureOutput) OK(format string, args ...any)      {}
func (c *captureOutput) Error(format string, args ...any)   {}
func (c *captureOutput) Info(format string, args ...any)    {}
func (c *captureOutput) Warning(format string, args ...any) {}
func (c *captureOutput) Printf(format string, args ...any) {
	fmt.Fprintf(&c.buf, format, args...)
}
func (c *captureOutput) Println(a ...any)                  { fmt.Fprintln(&c.buf, a...) }
func (c *captureOutput) C(_ engine.Color, text string) string { return text }

type sample struct {
	N int
}

func TestObservers_EmptyLen(t *testing.T) {
	var obs engine.Observers[sample]
	if obs.Len() != 0 {
		t.Errorf("Len() on zero value = %d, want 0", obs.Len())
	}
}

func TestObservers_AddIgnoresNil(t *testing.T) {
	var obs engine.Observers[sample]
	obs.Add(nil)
	if obs.Len() != 0 {
		t.Errorf("Len() after Add(nil) = %d, want 0", obs.Len())
	}
}

func TestObservers_FireInvokesAllListenersInOrder(t *testing.T) {
	var obs engine.Observers[sample]
	order := []string{}
	obs.Add(func(s *sample) { order = append(order, fmt.Sprintf("a=%d", s.N)) })
	obs.Add(func(s *sample) { order = append(order, fmt.Sprintf("b=%d", s.N)) })
	obs.Add(func(s *sample) { order = append(order, fmt.Sprintf("c=%d", s.N)) })

	if obs.Len() != 3 {
		t.Fatalf("Len() = %d, want 3", obs.Len())
	}

	obs.Fire(&sample{N: 42})
	want := []string{"a=42", "b=42", "c=42"}
	if strings.Join(order, ",") != strings.Join(want, ",") {
		t.Errorf("Fire order = %v, want %v", order, want)
	}
}

func TestObservers_FireWithNoListenersIsNoOp(t *testing.T) {
	var obs engine.Observers[sample]
	obs.Fire(&sample{N: 1}) // must not panic
}

func TestObservers_DispatchFiresOnSuccess(t *testing.T) {
	var obs engine.Observers[sample]
	var got *sample
	obs.Add(func(s *sample) { got = s })

	out := &captureOutput{}
	decode := func(p []byte) *sample {
		if len(p) < 1 {
			return nil
		}
		return &sample{N: int(p[0])}
	}
	obs.Dispatch(out, "Sample", []byte{7}, decode)

	if got == nil || got.N != 7 {
		t.Errorf("Dispatch did not fire listener with decoded value; got=%v", got)
	}
	if s := out.buf.String(); s != "" {
		t.Errorf("Dispatch emitted diagnostic on success: %q", s)
	}
}

func TestObservers_DispatchMalformedEmitsDiagnostic(t *testing.T) {
	var obs engine.Observers[sample]
	var fired bool
	obs.Add(func(s *sample) { fired = true })

	out := &captureOutput{}
	decode := func(_ []byte) *sample { return nil } // always malformed
	obs.Dispatch(out, "Sample", []byte{1, 2, 3}, decode)

	if fired {
		t.Errorf("Dispatch fired listener despite decode returning nil")
	}
	if !strings.Contains(out.buf.String(), "Sample: (incomplete: 3 bytes)") {
		t.Errorf("Dispatch diagnostic missing or wrong: %q", out.buf.String())
	}
}

func TestObservers_DispatchEmptyPayloadIsSilent(t *testing.T) {
	var obs engine.Observers[sample]
	out := &captureOutput{}
	decode := func(_ []byte) *sample { return nil }
	obs.Dispatch(out, "Sample", nil, decode)
	if s := out.buf.String(); s != "" {
		t.Errorf("Dispatch on empty payload emitted: %q", s)
	}
}
