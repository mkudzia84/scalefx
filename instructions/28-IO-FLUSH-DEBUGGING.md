# 28 — Low-Level I/O Flush Debugging (Make No Assumptions)

When data "isn't arriving", "stalls partway", or "uploaded OK but isn't there",
the instinct is to blame the obvious peripheral — the SD card, the cable, the
host. **Resist it.** Every byte on this system crosses *several* buffers between
where it's produced and where it's consumed, and at each boundary it can be
**stuck** (waiting for a flush that never fires) or **dropped** (overrun, or a
device that ACKs then discards). This is baremetal: there is no OS to paper over
a mis-configured FIFO threshold or an unchecked write return. The only reliable
method is to **localize the failing boundary empirically — instrument each layer
and measure; never infer from a symptom one layer away.**

Two real bugs (2026-05-31) motivated this guide and recur as worked examples
below: a UART RX FIFO that didn't flush a segment tail (looked exactly like a
slow SD card), and a failing SD card that ACK'd writes but dropped them (passed
an MD5 check). Both wasted time because an assumption was made instead of a
measurement.

## The three states of a byte

A byte is in exactly one of these — and they are NOT the same thing:

1. **Sent** — the producer handed it to *its* output buffer (`write()` returned).
   Says nothing about whether it left the chip.
2. **Delivered** — it crossed every transport buffer and the consumer's
   application code has read it.
3. **Persisted / acted-on** — it reached its final destination *and that
   destination actually kept it* (on disk, in the register, on the wire).

Most I/O bugs are a byte that's "sent" (so the producer thinks it's done) but
not "delivered" (stuck in a transport buffer) or not "persisted" (the device
lied). Always ask which of the three you've actually proven, and how.

## Every boundary is a buffer with a flush trigger

Map the buffer stack for the path you're debugging. For ScaleFX the two main
paths are:

### Inbound wire → application (UART RX)
```
host TX → USB-UART bridge → MCU UART HW FIFO (128 B) → driver RX ring (32 KB)
        → app read (NativeUartStream::readBytes) → COBS framer / stream consumer
```
- **HW FIFO → driver ring** flushes on `rx_full_threshold` crossing **OR**
  `rx_timeout` (line-idle). `available()` (`uart_get_buffered_data_len`) sees
  ONLY the ring — bytes in the FIFO are invisible until the ISR moves them.
  → If the sender pauses with < threshold bytes in the FIFO and the idle-timeout
  is lax/unset, those bytes are **stuck forever**. (Bug #1.)

### Application → storage media (upload write)
```
UART → PSRAM fill buffer (64 KB) → policy write (onUploadBufferFull)
     → VFS/FATFS cache → SD/flash controller → physical cells
```
- **fill buffer → media** flushes on buffer-full **OR** segment boundary **OR**
  `UPLOAD_END` (final partial + `f_flush` + `f_close`).
- **VFS/FATFS → cells**: a successful `f_write`/`f_close` return only means the
  *driver* accepted it. A counterfeit/failing card can return success and never
  commit. (Bug #2.)

### Outbound application → wire (UART TX / `Print`)
```
app write → driver TX ring → HW FIFO → bridge → host RX
```
- `flush()` must mean "wait until the TX ring AND FIFO are empty on the wire",
  not "I queued it". Check the implementation actually drains (e.g. `uart_wait_tx_done`).

For each boundary, write down: **buffer size**, **flush trigger(s)**, **how you
observe occupancy**, **the overflow behaviour** (block? drop? overwrite?).

## Method: localize the boundary, don't guess

1. **State what you've actually proven.** "MD5 matched" proves *delivered*
   (wire intact) — it does NOT prove *persisted* if the hash is over the
   in-flight stream, not a readback. "`write()` returned N" proves *accepted by
   the next buffer* — not *on the media*. Write the proof next to the claim.
2. **Measure at each boundary, top to bottom.** Add a counter/latency probe at
   every layer (bytes in, bytes out, max occupancy, max flush latency) and read
   them after a failure. The layer where `in != out` (stuck) or `out < in`
   (dropped) is your culprit. In ScaleFX, `FILE_UPLOAD_DIAG` does this for the
   storage path (SD write count / avg / **max latency** / bytes), and the
   per-segment `STORAGE_LOG` does it live on the native console.
3. **Make the invisible visible.** A buffer you can't observe is where the bug
   hides. The UART FIFO bug was invisible because `available()` couldn't see the
   FIFO; the diagnostics that *could* see the SD layer (healthy: 4 ms avg) are
   what redirected attention to the wire. Add the probe to the layer you're
   *assuming* is fine — that's usually where it isn't.
4. **Reproduce and read the offset.** A failure at a **random** byte offset that
   differs run-to-run ⇒ timing/race (a flush trigger that's marginal under load).
   A failure at a **fixed** offset ⇒ a deterministic size/boundary bug. Bug #1
   failed at 1.8 MB then 38 MB (random) ⇒ timing ⇒ a flush trigger, not a logic
   off-by-N.
5. **Change ONE knob, re-measure.** Don't change the threshold and the timeout
   and the buffer size together — you won't know which mattered, and baremetal
   knobs interact (a low timeout is useless if the threshold-full ISR is starved).
6. **Verify the fix end-to-end, including persistence.** For storage, a true
   pass is a **post-write readback hash**, not the streaming hash. Bug #2's card
   passed the streaming MD5 on every run while `df` never grew and the file
   didn't exist — only `stat`/`df` (independent observation of the media) caught it.

## Red flags that you're assuming, not verifying

- "It's probably the SD card / cable / host." — Maybe. *Measure the layer you're
  blaming first.* The slow-card theory survived two reflashes before the diag
  showed 4 ms writes.
- "The MD5 matched, so it's fine." — Fine *on the wire*. Did anything read it
  back off the media? See §3-states.
- "`write()` / `f_close()` returned success." — Returned success from the *next
  buffer down*, which may itself be lying or buffering. Success ≠ committed.
- "It works most of the time." — A buffer flush that's *marginal* (fires on the
  default timeout *usually*) fails intermittently at random offsets. "Usually
  works" is the signature of a flush trigger that needs to be set explicitly, not
  left to a default. **Defaults are an assumption** — pin the value and know why.
- "The small case works, so the path is fine." — Small transfers often hit a
  *different* flush trigger (a long idle that any timeout catches) than a
  sustained one (threshold-bound, idle-starved). Test at the scale that fails.

## Checklist when an I/O path stalls or loses data

- [ ] Draw the full buffer stack producer→consumer; note size + flush trigger(s)
      + overflow behaviour for each boundary.
- [ ] For each boundary, can you *observe* occupancy/throughput? If not, add a
      probe there first — especially the layer you assume is healthy.
- [ ] Classify the symptom: stuck (in==out at a layer, nothing downstream) vs
      dropped (out<in) vs not-persisted (delivered but absent on readback).
- [ ] Random failure offset ⇒ timing/flush-trigger; fixed offset ⇒ size/logic.
- [ ] Are any flush triggers left at SDK/driver defaults? Pin them explicitly
      and justify the value (threshold vs idle-timeout do different jobs).
- [ ] Prove the fix with an *independent* observation of the final layer
      (readback hash, `df`/`stat`, scope on the wire) — not the producer's own
      success return.

## Worked examples

**Bug #1 — UART RX FIFO tail not flushed (looked like a slow SD card).** Large
stream uploads aborted "12 bytes short of a random segment boundary". Assumption:
slow SD card → swapped cards twice. Measurement (`FILE_UPLOAD_DIAG`): both cards
4 ms avg / ~98 ms max writes — healthy. Re-localized to the UART RX boundary:
`available()` saw only the driver ring; the segment tail sat in the HW FIFO with
the client paused for the ACK, and the IDF-default `rx_timeout` was marginal at
6 Mbps under streaming load. Fix: pin `uart_set_rx_full_threshold(64)` +
`uart_set_rx_timeout(10)` so the tail flushes ~17 µs after line idle. See
[27 §7](27-WIRE-ASYNC-AND-UPLOAD.md) +
[native_uart_stream.cpp](../controllers/lib/sfx_platform/platform/native_uart_stream.cpp).

**Bug #2 — card ACKs writes but drops them (passed MD5).** After the FIFO fix the
upload reported "✓ 121 MB, MD5 match" — yet `df` showed 3 MB used (no growth) and
`stat` said the file didn't exist. The upload MD5 is hashed over the *received
wire stream*, not an SD readback, so it proved *delivered*, never *persisted*.
The card was failing/counterfeit. Lesson: a producer-side / wire-side hash is not
a persistence check; verify the final layer independently.

## See also

- [27 — Wire Async & Upload](27-WIRE-ASYNC-AND-UPLOAD.md) — the upload protocol,
  `FILE_UPLOAD_DIAG`, and the FIFO fix in context.
- [24 — Coredump Debugging](24-COREDUMP-DEBUGGING.md) — decode a panic instead of
  guessing (same "measure, don't infer" discipline for crashes).
- [10 — Upload Protocol Refactor](10-UPLOAD-PROTOCOL-REFACTOR.md) — the storage
  pipeline whose buffers §"Every boundary" maps.
