# 27 — Wire Async Discipline & Stream-Upload Exclusivity

Hard-won rules from the 2026-05-31 Studio stream-upload investigation. Two
stacked client-side bugs made Studio uploads of files > 64 KB hang while the CLI
worked — both rooted in how the shared serial wire is multiplexed. Codified as
Rules 53–54 (and the native-layer Rule 55) in
[copilot-instructions.md](../.github/copilot-instructions.md).

## The wire is a single shared half-duplex resource

One UART (HubFX UART0 @ 6 Mbps, USB-CDC bridge) carries EVERYTHING: command
requests, ACK/NACK responses, async broadcasts (live-view input channels at
~50 Hz), keepalives, log packets, AND raw stream-upload data. Three failure
modes follow.

### 1. Raw stream mode has no framing — nothing else may transmit

A `UploadStream` upload (SD files > 64 KB in Studio; the CLI default) switches
the firmware into **raw byte-stream mode**: `loop()` calls only
`storage.processStream()` and `return`s — the COBS framer is bypassed entirely.
Any COBS packet the *client* writes during this window is read as **file data**,
corrupting the upload and throwing off the byte count so the segment never
completes → 15 s segment-ACK timeout → "stuck".

The client MUST hold the wire exclusively for the raw phase:
- **Keepalive is gated off** during ANY upload — `Connection.streamActive` (the
  raw-byte burst) OR the broader `Connection.uploadActive`
  ([connection.go](../app/go/protocol/connection.go)). `FileUpload` sets
  `SetUploadPhase(true)` for the whole transfer (sync AND stream) and clears it on
  return; the keepalive loop skips while either flag is set. Safe: the wire is
  busy (segments or a chunk/ACK loop), and HubFX runs with the inactivity
  watchdog disabled, so no keepalive is needed for the upload's duration. Sync
  uploads were previously NOT gated — a 3 s keepalive could land between chunks
  and contend on the half-duplex wire (hardening 2026-05-31). Background pollers
  should likewise check `conn.UploadActive()` and skip while a transfer runs.
- **No concurrent commands.** A per-tab status poller (`setInterval`), a
  telemetry request, an `ls` refresh — anything that calls `Send()` mid-upload
  corrupts it. `uploadStream` marks the phase via `conn.SetStreamPhase(true/false)`.
- **Instrumentation is permanent.** `Send()` logs any COBS write during the
  stream phase as a `COLLIDE` trace + bumps a `collisions` counter; Studio
  surfaces it as a loud `WIRE-COLLISION` warning with the offending packet type,
  and reports a per-upload collision count. A clean upload = 0 collisions. This
  is how the `KEEPALIVE` culprit was pinned instead of guessed.

### 2. Flow-control async ≠ lossy telemetry async

The client async dispatcher
([`dispatchResponse`](../app/go/protocol/connection.go)) deliberately **drops**
async (`TAG_ASYNC`) packets on a full 256-deep `asyncQueue` — the reader
goroutine must NEVER block on a slow consumer (Studio's Wails emit at 50 Hz), or
the serial buffer backs up and *command responses* time out. Live-view channel
broadcasts are lossy by design.

BUT `FILE_UPLOAD_PROGRESS` (the per-segment flow-control ACK that gates the next
16 KB segment) is ALSO `TAG_ASYNC`. Routing it through that shared lossy queue
meant a 50 Hz live-view flood could drop the critical ACK → upload stall with
**no collision** (the second, subtler bug).

**Rule:** async packets with a **registered filter** (`RegisterAsyncFilter`,
e.g. upload progress) are explicit flow-control consumers, NOT lossy telemetry.
`dispatchResponse` delivers them straight to their own buffered channel,
bypassing the shared broadcast queue — still NON-BLOCKING (filter buffers are
sized for their own cadence, not contended by broadcasts). Only the *general
broadcast callback* path stays lossy. Never put an ACK / flow-control / response
packet on the lossy path.

### 3. RX ring must hold a full burst

The firmware UART RX ring must be ≥ one stream-upload segment
(`STREAM_SEGMENT_SIZE` = 16 KB), because the client blasts a whole segment before
waiting for its ACK (no intra-segment backpressure). HubFX uses **32 KB** (2×).
An 8 KB ring overflowed mid-segment and stalled whenever `processStream` lagged
the 6 Mbps fill — masked for months because the upload read accidentally used
Arduino's slow per-byte `Stream::readBytes` polyfill (the `sfx::Stream` seam
exposed the bulk-drain path that the 8 KB ring assumed). See Rule on RX sizing
in the `.ino` (`wireUart.begin`).

## Why the CLI never saw any of this

The CLI is single-threaded: one command at a time, no background pollers, no
live-view subscription. Its uploads finish before the 3 s idle keepalive fires
and nothing floods the async queue. Studio is the stress case (concurrent
pollers + 50 Hz live-view + keepalive on the same wire) — always test wire
changes against Studio, not just the CLI.

### 4. Concurrent senders must not race on shared `Connection` state (Rule 56)

Studio drives the wire from several goroutines at once — the config-apply upload,
status/telemetry pollers, the keepalive loop, per-RPC Wails handlers. Every
mutable `Connection` field needs a lock/atomic: `nextTag` (`tagMu`),
`tagWaiters`/`streamWaiters`/`asyncFilters` (`waiterMu`), writes (`writeMu`),
`streamActive`/`collisions` (atomics).

The 2026-05-31 bug: `NextTag()` was an **unlocked** read-modify-write, so two
concurrent commands got the SAME correlation tag → one command's ACK landed in
the other's waiter and the loser timed out (the periodic `upload chunk @0
(seq=0): timeout` on Studio config-apply). The CLI never reproduced it
(single-threaded). Fix: a `tagMu` mutex. Same reason the bugs above hid from the
CLI: it sends one command at a time.

### 5. Firmware must self-heal an abandoned upload (Rule 57)

The wire is single-master and a sync upload is **exclusive** on HubFX — `loop()`
drains only `pumpBus()` (or `processStream()`) while `isUploadActive()`, skipping
`board.process()`. So a half-finished transfer the client walked away from (a
timed-out chunk, a dropped ACK, a Studio crash) leaves the device upload-exclusive
until something clears `_uploadActive`. Two recovery paths, both in
[storage_service.ipp](../controllers/lib/sfx_storage/server/storage_service.ipp):

- **Stale-upload reset on a fresh `UPLOAD_BEGIN` (primary).** A new `UPLOAD_BEGIN`
  while one is already active means the previous client abandoned and reconnected
  to retry. The handler used to NACK `UPLOAD_IN_PROGRESS` — which wedged the retry
  until the inactivity timeout fired. It now `cleanupUpload(true)`s the stale
  transfer (closes the file, frees buffers, unlocks storage, fires `onUploadEnd`)
  and honours the new BEGIN immediately. The reconnecting client recovers on its
  first retry, not after a timeout.
- **Inactivity timeout (fallback, no reconnect).** `checkUploadTimeout()` runs
  every upload-exclusive pass; `UPLOAD_TIMEOUT_MS` was lowered 30 s → **8 s** for
  sync (stream stays 5 s) so a client that never comes back self-heals in seconds
  rather than half a minute. 8 s is a generous ceiling for one missing chunk
  (round-trip is dominated by the few-ms SD/flash write).

Note `pumpBus()` still dispatches the FULL policy chain, so `IDENTIFY` / `connect`
and any **different-target** command stay answered during a sync upload — the
device is not actually dead, it just can't accept a **same-target** storage op
(those NACK `UPLOAD_IN_PROGRESS` before touching the lock, so no deadlock). The
30 s wedge was the perception bug the recovery paths above fix.

### 6. Stream-upload diagnostics are post-mortem, not live (Rule 57)

During a raw-stream upload the firmware is in byte-stream mode and **cannot emit
COBS log packets over the wire** — the rich per-segment `STORAGE_LOG` stats
(`sd_rate`, `sd_maxlat`, loop gap, fill %) only reach the **native USB-Serial-JTAG
console**, never the CH343/Studio wire. So when a large upload stalls, the
operator on the wire sees only a bare "stream timeout" with no cause.

`FILE_UPLOAD_DIAG_REQ/RESP` (`0xA4/0xA5`) closes that gap. The firmware freezes a
diagnostics snapshot — bytes received, segment progress, ring fill, and the SD
**writer stats (write count, bytes, avg/MAX latency, total I/O)** plus an
`UploadEndReason` — at every terminal point (`captureUploadDiag()` in
`handleUploadEnd` / `checkUploadTimeout` / cancel / flush-fail / health / stale-
reset), BEFORE `cleanupUpload()` resets the policy's stats. The snapshot survives
until the next `FILE_UPLOAD_BEGIN`, so the client queries it **after** the wire is
back in COBS mode. `uploadStream`'s timeout path auto-fetches it and embeds a
summary in the returned error (echoed by both the CLI and the Studio console);
Studio also exposes `FsUploadDiag()` for on-demand queries.

The smoking gun is `sdMaxLat_ms`: a single 16 KB SD write taking multiple seconds
(GC / wear-levelling / a flaky card) is what blows the client's 30 s segment-ACK
deadline — no firmware change makes that write acceptable, but the diag tells you
it's the card, not the protocol. (The firmware no longer *self*-aborts on a slow-
but-healthy write — `processStream` re-stamps `_uploadLastActivity_ms` AFTER each
flush so the inactivity timer measures client silence, not our own write latency.)

### 7. UART RX FIFO must flush the segment tail (the real large-file bug)

The stream-upload failures that *looked* like a slow SD card were actually a
**UART RX FIFO flush** bug, proven by the diagnostics (§6): both cards wrote at
avg 4 ms / max ~98 ms — healthy — yet large uploads aborted with the firmware
**exactly 12 bytes short** of a random segment boundary.

`NativeUartStream::available()` reads `uart_get_buffered_data_len()`, which counts
ONLY the driver ring buffer. Bytes still in the 128-byte **hardware FIFO** are
invisible until the driver ISR moves them across, and that ISR fires on either a
`rx_full_threshold` crossing OR an `rx_timeout` (line-idle) event. The client
blasts a 16 KB segment then **pauses for the segment ACK** — so the segment's
tail bytes sit in the FIFO *below* the threshold with no further bytes to cross
it; the only thing that can flush them is the idle-timeout. Relying on the IDF
*default* timeout was marginal at 6 Mbps under streaming load: it flushed the
tail *most* of the time (so it failed intermittently, at random offsets, not
every segment). Small commands (IDENTIFY/status) always worked because they're
followed by a long idle the default timeout always caught.

Fix ([native_uart_stream.cpp](../controllers/lib/sfx_platform/platform/native_uart_stream.cpp)
`beginConfig`, after `uart_param_config`):

```cpp
uart_set_rx_full_threshold(_port, 64);  // flush eagerly + stay clear of overrun
uart_set_rx_timeout(_port, 10);         // flush the tail ~17 µs after line idle
```

`rx_timeout` handles the **not-full** case (the segment tail / any short packet);
`rx_full_threshold` handles the bulk-streaming case and keeps the FIFO clear of
its 128-byte overrun ceiling while the loop blocks in a synchronous SD write.
Applies to every `NativeUartStream` — the wire UART and the RC SBUS/Jeti UARTs.

**Aside — MD5 match ≠ persisted.** The upload MD5 is computed over the bytes
*received off the wire*, NOT an SD readback. A failing/counterfeit card that ACKs
writes but drops them (observed: `df` used-space never grew, the file didn't
exist) still returns "✓ MD5 match". A true end-to-end verify needs a post-upload
readback hash — not implemented today; treat MD5-match as "wire intact", not
"on disk".

## Checklist for any new long/raw wire operation

- Does it put the firmware in a non-COBS / exclusive mode? → hold the wire
  (gate keepalive + pollers via a `streamActive`-style flag).
- Does it rely on an async packet for flow control? → give it a registered
  filter, never the lossy broadcast queue.
- Does the client burst > the firmware RX ring? → size the ring to the burst.
- Does more than one goroutine touch the `Connection`? → every shared field gets
  a lock/atomic; `go test -race`; reproduce against Studio, not the CLI.
- Add a collision/drop counter and surface it in Studio.
