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
- **Keepalive is gated off** while `Connection.streamActive` is set
  ([connection.go](../app/go/protocol/connection.go)). Safe: the wire is busy
  with segments (not idle), and HubFX runs with the inactivity watchdog
  disabled, so no keepalive is needed for the upload's duration.
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

## Checklist for any new long/raw wire operation

- Does it put the firmware in a non-COBS / exclusive mode? → hold the wire
  (gate keepalive + pollers via a `streamActive`-style flag).
- Does it rely on an async packet for flow control? → give it a registered
  filter, never the lossy broadcast queue.
- Does the client burst > the firmware RX ring? → size the ring to the burst.
- Add a collision/drop counter and surface it in Studio.
