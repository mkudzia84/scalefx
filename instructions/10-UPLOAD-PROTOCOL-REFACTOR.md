# Upload Protocol Refactor — Windowed Flow Control

> **Status:** WINDOWED mode is the shipping default (v0.27.0 build 135+). Batch mode (formerly STREAM, mode=3) also ships, now on a **single-core inline-write** pipeline (HubFX v0.34.x).
> **Original problem:** STREAM mode (mode=3) failed reliably for files >1 MB.
> **Windowed solution:** New WINDOWED mode (mode=2) with server-controlled flow, per-window diagnostics, and hard recovery timeouts.
> **Batch mode today:** 64 KB PSRAM fill buffer drained synchronously to SD from the main loop. Uploads are **exclusive** — audio/engine/USB/diagnostics all skip while `isUploadActive()` is true. The dual-core `SpscRingBuffer` + writer-task pipeline below (builds 136-143) was retired after it correlated with end-of-upload drain hangs; that section is preserved as history.

## Stream Mode Recovery (builds 136-143)

After implementing WINDOWED mode, STREAM mode was also made reliable through a series of infrastructure improvements:

### Fixes Applied (chronological)

1. **PSRAM ring buffer pipeline (build 137):** Replaced flat 256 KB buffer with a 2 MB `SpscRingBuffer` (PSRAM) + 64 KB fill buffer + 256 KB staging buffer. Core 0 writes to ring via `processStream()`, Core 1 writer task drains to SD in 256 KB batches. Segmented protocol — 512 KB segments with per-segment ACK.

2. **Diagnostics and segment ACK (build 139):** Added per-segment instrumentation (`_streamMaxGap_ms`, `_streamIterCount`, periodic 2s progress logs). Segment ACK payload includes `ring_fill_pct` byte for client visibility.

3. **Writer task lifecycle refactor (build 141):** Replaced persistent writer task with on-demand per-upload lifecycle. `WriterStats` struct exposes performance counters as atomics. `allocateWriterBuffers()` called once at boot, task created/destroyed per upload.

4. **Audio suspend during uploads (build 142):** `onStreamStart()`/`onStreamEnd()` lifecycle callbacks suspend both audio tasks on Core 1 (consumer + producer) during stream uploads. Fixes priority starvation where audio tasks prevented the writer from running at equal priority.

5. **SD throughput + client flow control (build 143):** Changed SDMMC bus clock from 20 MHz → 40 MHz (`SDMMC_FREQ_HIGHSPEED`). The Go CLI throttles sending when `ring_fill_pct > 50%` (formula: `delay_ms = (pct - 50) * 60`).

### Current Stream Mode Architecture

```
Core 0 (protocol):    Serial RX → fill buf (64KB) → SPSC ring (2MB PSRAM)
Core 1 (writer):      Ring → staging (256KB) → SD card (SDIO 1-bit 40MHz)
Core 1 (audio):       SUSPENDED during upload (consumer + producer)
Client:               512KB segments, throttle when ring >50%
```

**Stream mode is now viable for files of any size** with the combined fixes. WINDOWED mode remains available as an alternative with per-chunk CRC-16 integrity and server-controlled window sizing.

---

## Problem Analysis

### Current Upload Modes

| Mode | Value | Framing | Per-Chunk ACK | Speed | Reliability |
|------|-------|---------|---------------|-------|-------------|
| **SYNC** | 0 | COBS | Yes | ~180 KB/s | ★★★★★ |
| **BURST** | 1 | COBS | No (fire-and-forget) | ~350 KB/s | ★★★☆☆ |
| **WINDOWED** | 2 | COBS | Per-window | ~470 KB/s | ★★★★★ |
| **STREAM** | 3 | Raw bytes (no COBS) | Per-segment | ~490 KB/s | ★★★★☆ (with fixes) |

### Why STREAM Mode Fails

STREAM mode bypasses COBS framing entirely — the client writes raw bytes after UPLOAD_BEGIN ACK, and the firmware counts down `_streamBytesRemaining` until all bytes arrive. Then it switches back to COBS for the UPLOAD_END packet.

**The fundamental fragility is in the stream-to-COBS transition:**

1. Client sends `file_size` raw bytes immediately after receiving UPLOAD_BEGIN ACK
2. Firmware counts `_streamBytesRemaining` down to 0 in `processStreamData()`
3. When counter reaches 0, firmware sets `streamReceiving = false` and resumes COBS parsing
4. Client sends UPLOAD_END as a COBS-framed packet

**If even 1 byte is lost in transit** (USB-UART bridge error, UART FIFO overflow, electrical glitch):
- `_streamBytesRemaining` never reaches 0
- Firmware stays in raw stream mode indefinitely
- The UPLOAD_END COBS packet gets consumed as raw file data (corrupting the file)
- The firmware is now stuck — the client is waiting for a response that will never come

The 5-second stall timeout (`STREAM_DATA_TIMEOUT_MS`) eventually fires and cleans up, but by then the client has already sent UPLOAD_END (which was consumed as raw data) and is waiting for a response. Both sides are now in incompatible states.

**Additional problems with STREAM mode:**
- **No per-chunk integrity** — single byte corruption silently corrupts the file (MD5 only detected at the end)
- **No flow control** — client blasts data; firmware can only spin-wait or drop data when ring buffer fills
- **No diagnostics** — client has no visibility into server buffer fill, write rate, or error state during transfer
- **Complex dual-core pipeline** — requires dedicated ring buffer (1MB PSRAM), separate writer task, atomic flags, semaphores — all for a mode that doesn't reliably work
- **No recovery** — once desynchronized, both sides must timeout independently with no coordination

### Root Cause of >1MB Failure

For files ≤1MB, the entire file fits in the 1MB ring buffer. The writer task drains it to SD after all bytes arrive. No bytes need to be in-flight when the ring buffer is full, so no data loss occurs.

For files >1MB, the ring buffer fills during transfer. Even with the writer task draining to SD, the UART RX path must compete with SD writes for throughput. At 6Mbps (~528 KB/s effective) with SD writes at ~400-2400 KB/s, the system is balanced but fragile. Any transient delay (SD write latency spike, USB-UART bridge buffering, RTOS scheduling jitter) causes byte loss — which is **irrecoverable** in STREAM mode.

---

## Proposed Solution: WINDOWED Mode (mode=2)

### Design Goals

1. **Flow control** — server tells client exactly how many chunks to send before waiting
2. **Per-window diagnostics** — after each window, server reports buffer fill, SD write rate, bytes written, CRC errors
3. **Recovery** — hard timeouts at every stage; both sides can abort cleanly. No "stuck forever" states.
4. **Near-STREAM throughput** — by ACKing every 32 chunks (~64KB) instead of every chunk, throughput approaches raw stream speed
5. **COBS-framed throughout** — no raw byte mode, no transition fragility

### Protocol Flow

```
CLIENT                                    SERVER
  |                                         |
  |--- UPLOAD_BEGIN (mode=2) ------------->|
  |<-- ACK [window:u16LE] ----------------|   (initial window size, e.g. 32)
  |                                         |
  |--- UPLOAD_DATA (seq=0, crc16) ------->|  ┐
  |--- UPLOAD_DATA (seq=1, crc16) ------->|  │ Window 0
  |--- ...                                 |  │ (32 COBS-framed chunks)
  |--- UPLOAD_DATA (seq=31, crc16) ------>|  ┘
  |                                         |  server flushes buffers...
  |<-- UPLOAD_PROGRESS [diagnostics] ------|   (next window size may differ)
  |                                         |
  |--- UPLOAD_DATA (seq=32, crc16) ------>|  ┐
  |--- ...                                 |  │ Window 1
  |--- UPLOAD_DATA (seq=63, crc16) ------>|  ┘
  |                                         |
  |<-- UPLOAD_PROGRESS [diagnostics] ------|
  |                                         |
  |   ...repeat until all data sent...      |
  |                                         |
  |--- UPLOAD_END ------------------------>|
  |<-- ACK [md5:16B][crc_errors:u16LE] ---|
```

### Wire Format

#### UPLOAD_BEGIN ACK (mode=2)

When upload mode is WINDOWED, the ACK payload includes the initial window size:

```
ACK payload: [window_size:u16LE]
```

- `window_size`: Number of UPLOAD_DATA chunks the client should send before waiting for UPLOAD_PROGRESS.
- Default: 32 (each chunk ~2KB = ~64KB per window)

#### FILE_UPLOAD_PROGRESS (new packet type: 0xB0)

Sent by the server after each complete window of chunks:

```
[acked_seq:u16LE]         — Last sequence number successfully received
[bytes_written_sd:u32LE]  — Total bytes written to storage so far
[buf_fill_pct:u8]         — Write buffer fill percentage (0-100)
[sd_write_rate:u16LE]     — Current SD write rate in KB/s (0 = not measured)
[crc_errors:u16LE]        — Total CRC errors accumulated so far
[next_window:u16LE]       — Window size for the NEXT batch of chunks
```

Total payload: **13 bytes**

The `next_window` field provides **server-controlled flow control**. The server adjusts the window size based on buffer fill:

| Buffer Fill | Action | Rationale |
|-------------|--------|-----------|
| < 25% | Double window (max 128) | Plenty of space, maximize throughput |
| 25–75% | Keep same | Balanced |
| > 75% | Halve window (min 4) | Buffer pressure, slow down client |
| CRC error rate > 10% | Halve window (min 4) | Signal quality issue |

#### Partial Final Window

The last window may contain fewer chunks than `window_size` (when remaining data doesn't fill a complete window). The server detects this by tracking `_uploadBytesWritten` against `_uploadExpectedSize`:

- If after processing a chunk, `_uploadBytesWritten == _uploadExpectedSize`, send UPLOAD_PROGRESS immediately (don't wait for full window).
- Client then sends UPLOAD_END as normal.

### Timeout Rules

| Location | Timeout | Trigger | Action |
|----------|---------|---------|--------|
| **Server** | 5s per chunk gap | No UPLOAD_DATA received for 5s during active window | Cancel upload, delete partial, log |
| **Server** | 30s inactivity | No activity at all for 30s (existing `UPLOAD_TIMEOUT_MS`) | Cancel upload, delete partial |
| **Client** | 15s per window | No UPLOAD_PROGRESS received within 15s after sending window | Send UPLOAD_CANCEL, report error |
| **Client** | User interrupt | Ctrl+C | Send UPLOAD_CANCEL |

### Recovery Guarantees

1. **No stuck states** — every transfer stage has a hard timeout on both sides
2. **Clean abort** — `UPLOAD_CANCEL` always works because protocol is COBS-framed (no raw byte mode confusion)
3. **Partial file cleanup** — server deletes partial file on any abort/timeout
4. **Storage lock release** — `cleanupUpload()` always releases SD/Flash lock
5. **CRC error visibility** — client can decide to abort early if CRC error rate is too high
6. **Server reports errors** — if SD write fails mid-transfer, the next UPLOAD_PROGRESS is replaced with a NACK

### Performance Analysis

```
Mode     │ ACKs per 1MB │ RTT overhead │ Estimated KB/s │ Reliability
─────────┼──────────────┼──────────────┼────────────────┼──────────────
SYNC     │ 512          │ 512ms        │ ~180           │ ★★★★★
WINDOWED │ 16 (W=32)    │ 16ms         │ ~470           │ ★★★★★
BURST    │ 1            │ 1ms          │ ~485           │ ★★★☆☆
STREAM   │ 1            │ 1ms          │ ~490           │ ★☆☆☆☆
```

With window size 32 (~64KB per window), WINDOWED mode adds only **16 round-trips per MB** vs 512 for SYNC. At ~1ms RTT over USB serial, that's ~16ms overhead per MB — negligible. Throughput is within ~5% of the raw STREAM mode, with full reliability.

### Implementation Reuse

**WINDOWED mode reuses the existing SYNC/BURST infrastructure entirely:**

| Component | STREAM mode (current) | WINDOWED mode (proposed) |
|-----------|----------------------|------------------------|
| Buffer type | 1MB PSRAM ring buffer | 512KB x 2 double-buffer (existing) |
| Writer task | Per-upload stream writer (Core 0) | Persistent chunked writer (Core 1, existing) |
| Synchronization | Ring buffer + atomics + semaphore | Simple semaphore swap (existing) |
| Serial parsing | Raw bytes in `processStreamData()` | COBS via `CommandRouter` (standard path) |
| Main loop | Special `isStreamReceiving()` branch | Normal `server.loop()` |
| CRC integrity | None per-chunk (MD5 at end only) | CRC-16 per chunk (existing) |

**Result:** Less code overall — the ring buffer, stream writer task, and raw byte processing path can be removed once STREAM mode is deprecated.

---

## Implementation Plan

### Phase 1: Firmware — Add WINDOWED Mode

**Files to modify:**

| File | Changes |
|------|---------|
| `hubfx/hubfx.h` | Add `FILE_UPLOAD_PROGRESS = 0xB0`, add `UPLOAD_WINDOW = 2` to enum, update mode validation |
| `storage_server.h` | Add window tracking state: `_windowSize`, `_windowChunkCount`, `_windowBytesInWindow` |
| `storage_server.ipp` | `handleUploadBegin()`: accept mode=2, send window_size in ACK payload. `handleUploadData()`: count chunks, send UPLOAD_PROGRESS when window complete. Add `sendUploadProgress()` and `computeNextWindowSize()` |
| `esp32_storage_policy.h/.cpp` | No changes needed — reuses existing chunked writer |
| `pico_storage_policy.h` | No changes needed — blocking writes already work for chunked mode |

**New server state:**
```cpp
uint16_t _windowSize = 0;           // Current window size (0 = not windowed)
uint16_t _windowChunkCount = 0;     // Chunks received in current window
```

**New method:**
```cpp
void sendUploadProgress();
uint16_t computeNextWindowSize() const;
```

### Phase 2: Protocol Sync — All Clients

**Files to modify:**

| Platform | Files | Changes |
|----------|-------|---------|
| **C++** | `hubfx/hubfx.h` | Constants (Phase 1) |
| **Go** | `protocol/hubfx/hubfx.go` | Add constants |
| **Go CLI** | `api_files.go` | New `uploadWindowed()` method; parse UPLOAD_PROGRESS; progress callback with diagnostics |

### Phase 3: Deprecate STREAM Mode

After WINDOWED mode is proven:
1. Change `--burst` flag to use WINDOWED instead of STREAM
2. Remove ring buffer allocation/free for stream mode
3. Remove `processStreamData()` and `isStreamReceiving()` from server
4. Remove stream writer task from ESP32 policy
5. Remove `streamReceiving` flag and raw byte processing from main loop
6. Simplify main loop back to just `server.loop()`

### Phase 4: Testing

1. **Small file** (<1MB): Verify WINDOWED works, compare speed to SYNC
2. **Large file** (5MB+): Verify no failures at any size
3. **Slow SD card**: Verify flow control reduces window size dynamically
4. **CRC injection**: Verify CRC errors are counted and reported
5. **Client timeout**: Kill client mid-transfer → verify server auto-cancels and cleans up
6. **Server abort**: Cause SD write failure → verify NACK and cleanup
7. **Interrupt**: Ctrl+C during transfer → verify clean cancel on both sides

---

## FAQ

**Q: Why not just fix STREAM mode?**  
A: The fundamental design — counting raw bytes and expecting the counter to match — is inherently fragile on a USB-UART bridge at 6Mbps. Any byte loss (electrical, FIFO overflow, USB microframe timing) causes irrecoverable desync. COBS framing eliminates this class of bug entirely.

**Q: Will WINDOWED mode be slower than STREAM?**  
A: By ~5% in theory (~470 vs ~490 KB/s). In practice, STREAM mode frequently fails for large files, making its effective speed 0 KB/s for those transfers. WINDOWED is faster for any file that STREAM can't reliably transfer.

**Q: Can we keep STREAM mode as a fallback?**  
A: Yes, during Phase 3 transition. But once WINDOWED is proven, there's no reason to maintain the extra complexity (ring buffer, stream writer task, raw byte processing, dual main-loop branches).

**Q: What about BURST mode (mode=1)?**  
A: BURST mode stays as-is — it works for clients that don't want flow control. WINDOWED is strictly better (same throughput, plus diagnostics and flow control), so BURST may also be deprecated long-term.

**Q: Why window size 32?**  
A: Each UPLOAD_DATA chunk is ~2KB (2044 bytes data + 4 bytes header). 32 chunks = ~64KB per window. This gives 16 round-trips per MB — negligible overhead at ~1ms RTT. The server can adjust dynamically via `next_window` in UPLOAD_PROGRESS.

**Q: What if the server needs the window ACK to also flush its write buffers?**  
A: The double-buffer approach handles this naturally. If the writer task hasn't finished the previous buffer when the window completes, the server blocks on `submitWriteBuffer()` before sending UPLOAD_PROGRESS. This provides implicit backpressure — the client can't send the next window until it receives the progress response.
