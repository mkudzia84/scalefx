package engine

// Observers is a typed multicast listener set used by module handlers to
// decouple "decode + dispatch" from "format for console" / "emit to GUI".
//
// A handler declares one field per event type (e.g. OnCalibStatus
// engine.Observers[CalibStatus]) and its Register() function appends the
// built-in CLI console formatter. External consumers (Studio) append their
// own listener with Add, so every subscriber receives the decoded struct.
//
// Not goroutine-safe — listeners are wired up at startup and fired on the
// engine's single packet-dispatch goroutine.
type Observers[T any] struct {
	fns []func(*T)
}

// Add appends a listener. nil is ignored.
func (o *Observers[T]) Add(fn func(*T)) {
	if fn == nil {
		return
	}
	o.fns = append(o.fns, fn)
}

// Fire invokes every registered listener in registration order.
func (o *Observers[T]) Fire(v *T) {
	for _, fn := range o.fns {
		fn(v)
	}
}

// Len reports how many listeners are registered.
func (o *Observers[T]) Len() int { return len(o.fns) }

// Dispatch decodes payload with decode(); on success, fires every observer
// with the decoded struct. On nil (malformed, non-empty payload) it prints
// an "(incomplete: N bytes)" diagnostic to out. Empty payloads are ignored.
//
// Lets handler Register() wire async/broadcast packet types inline without
// a separate `parse*` wrapper: one decode + Fire call pattern per packet.
func (o *Observers[T]) Dispatch(out Output, label string, payload []byte, decode func([]byte) *T) {
	if v := decode(payload); v != nil {
		o.Fire(v)
		return
	}
	if len(payload) > 0 {
		out.Printf("  %s: (incomplete: %d bytes)\n", label, len(payload))
	}
}
