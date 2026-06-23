/*
 * UploadEngine<TPolicy> — the exclusive file-upload state machine.
 *
 * Owns EVERYTHING about an in-flight upload: the sync (windowed) + batch
 * (raw-stream) protocols, the per-segment flow control, MD5 hashing, the
 * inactivity / stale-reset self-heal, the post-mortem diagnostics snapshot,
 * and the exclusive-upload lifecycle callbacks.  This is the wire-critical
 * core (Rules 53/54/56/57) — split out of StorageServicePolicy so the
 * upload state machine is one cohesive, readable unit separate from the
 * filesystem-query handlers.
 *
 * It reaches the SHARED storage dependencies (the platform policy, the shared
 * buffers, storage lock/ready, transfer-exclusivity notification, the wire)
 * through a back-reference to the owning StorageServicePolicy — the same
 * pattern AudioMixer's MixKernel uses to reach the mixer it serves.  Those
 * dependencies are genuinely shared with the file-ops half (one storage lock,
 * one transfer-active flag, one platform policy), so they stay on the policy;
 * only the upload-specific state lives here.
 *
 * #included by storage_service.h (decls), with the method bodies in
 * storage_upload_engine.ipp #included AFTER StorageServicePolicy is complete.
 */

#ifndef SFX_STORAGE_UPLOAD_ENGINE_H
#define SFX_STORAGE_UPLOAD_ENGINE_H

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>

#include <serial/storage/storage_protocol.h>   // StorageWire
#include "sfx_md5.h"                            // sfx_storage::SfxMd5

namespace sfx { class Stream; }

// StorageServicePolicy owns this engine and provides its shared dependencies.
template <typename TPolicy> class StorageServicePolicy;

template <typename TPolicy>
class UploadEngine {
public:
    explicit UploadEngine(StorageServicePolicy<TPolicy>& svc) : _svc(svc) {}

    // ── Queried by the file-ops half + the hub main loop ─────────────────
    // _uploadActive / _streamActive are read cross-task (the Jeti telemetry
    // task gates on isActive() — Rule 15), so they are atomic with acquire
    // loads here paired with release stores in the handlers.
    bool isActive() const       { return _uploadActive.load(std::memory_order_acquire); }
    bool isStreamActive() const { return _streamActive.load(std::memory_order_acquire); }
    StorageWire::StorageTarget target() const { return _uploadTarget; }

    // ── Exclusive-upload lifecycle callbacks (resource suspend/resume) ───
    void onUploadStart(std::function<void()> cb) { _onUploadStart = std::move(cb); }
    void onUploadEnd  (std::function<void()> cb) { _onUploadEnd   = std::move(cb); }

    // ── Packet handlers (0xA0..0xA3 + diag 0xA4) ─────────────────────────
    void handleUploadBegin(const uint8_t* payload, size_t len);
    void handleUploadData (const uint8_t* payload, size_t len);
    void handleUploadEnd();
    void handleUploadCancel();
    void handleUploadDiagReq();

    // ── Raw-stream pump + timeout (driven from the main loop) ────────────
    void processStream();
    void checkUploadTimeout();

    /// Force-cleanup any active upload (called on SHUTDOWN).
    void cancelActiveUpload();

private:
    void captureUploadDiag(uint8_t reason);
    void cleanupUpload(bool deletePartial);
    void sendStreamSegmentAck();

    // Thin wrappers onto the owning policy's shared surface — keep the moved
    // handler bodies textually unchanged.  Defined in the .ipp (where the
    // policy is a complete type).
    int     sendAck();
    int     sendNack(uint8_t errorCode, const char* reason = nullptr);
    int     sendRawPacket(uint8_t t, uint8_t tag, const uint8_t* p = nullptr, size_t l = 0);
    uint8_t currentTag() const;
    sfx::Stream* serial() const;
    bool    checkStorageReady(StorageWire::StorageTarget target);
    void    lockStorage(StorageWire::StorageTarget target);
    void    unlockStorage(StorageWire::StorageTarget target);
    void    notifyTransferStart();
    void    notifyTransferEnd();

    StorageServicePolicy<TPolicy>& _svc;

    // --- Upload state (protocol-only) ---
    std::atomic<bool> _uploadActive{false};  // read cross-task (Rule 15)
    StorageWire::StorageTarget _uploadTarget = StorageWire::TARGET_SD;
    StorageWire::UploadMode    _uploadMode   = StorageWire::UPLOAD_SYNC;
    char     _uploadPath[128]    = {};
    uint32_t _uploadExpectedSize = 0;
    uint32_t _uploadBytesWritten = 0;
    uint16_t _uploadExpectedSeq  = 0;
    sfx_storage::SfxMd5 _uploadMd5;

    // --- Stream upload state ---
    std::atomic<bool> _streamActive{false};     // True while raw binary streaming (read cross-task, Rule 15)
    uint32_t _streamSegmentSize     = 0;        // Bytes per segment (set in handleUploadBegin)
    uint16_t _streamSegmentIndex    = 0;        // Current segment number (0-based)
    uint32_t _streamSegBytesRemaining = 0;      // Bytes left in current segment
    uint16_t _streamSegmentCount    = 0;        // Total segment count (for logging)

    // --- Stream diagnostics ---
    uint32_t _streamStartTime_ms    = 0;        // Stream mode activation time
    uint32_t _streamSegStartTime_ms = 0;        // Current segment start time
    uint32_t _streamEndTime_ms      = 0;        // Stream mode deactivation time
    uint32_t _streamLastLogTime_ms  = 0;        // Last periodic progress log
    uint32_t _streamLastLogBytes    = 0;        // Bytes at last periodic log
    uint32_t _streamLastCallTime_ms = 0;        // Last processStream() call time
    uint32_t _streamMaxGap_ms       = 0;        // Worst-case gap between calls
    uint32_t _streamIterCount       = 0;        // processStream() call count per segment

    // Segment sizes — these are the per-ACK windows on the wire.  The
    // client blasts one segment then blocks on FILE_UPLOAD_PROGRESS
    // before sending the next, so the segment size IS the back-pressure
    // window.  Too big = SD spikes (cluster allocation, GC, FAT update)
    // overflow the UART RX buffer; too small = ACK round-trips dominate.
    //
    // 2026-05-28 (build 495 diag): 64 KB segments still lost data to an
    // occasional 100 ms SD spike — because the buffer is 32 KB, the
    // 64 KB segment requires ONE buffer-full flush mid-segment, and
    // THAT'S where the spike landed (client still blasting, UART RX
    // overflowed).  Shrunk SD to 16 KB so segment-size ≤ buffer-size:
    // every flush now happens at the segment boundary while the client
    // is blocked on FILE_UPLOAD_PROGRESS, so a 100+ ms SD spike never
    // coincides with active wire traffic.  Cost: 90 ACKs per 1.4 MB
    // upload × ~5 ms = ~450 ms overhead, ~10 % throughput hit at
    // 500 KB/s, but reliable to arbitrary file size.
    static constexpr uint32_t STREAM_SEGMENT_SIZE        = 16384;   // SD: 16 KB per segment
    static constexpr uint32_t STREAM_SEGMENT_SIZE_FLASH  = 16384;   // flash: 16 KB per segment
    static constexpr uint32_t STREAM_INACTIVITY_MS       = 5000;    // 5s timeout per segment

    // --- Upload lifecycle callbacks (exclusive-upload resource mgmt) ---
    std::function<void()> _onUploadStart;
    std::function<void()> _onUploadEnd;
    bool _uploadSuspended = false;  // Guard: ensures exactly one onUploadEnd per onUploadStart

    // Sync-upload inactivity timeout (hardening, 2026-05-31): lowered 30000 → 8000.
    // A sync chunk's round-trip is dominated by the SD/flash write (a few ms) plus
    // wire latency, so 8 s is a generous ceiling for one missing chunk while being
    // far quicker to self-heal an abandoned transfer that never reconnects.  The
    // primary recovery path is stale-upload reset on the next UPLOAD_BEGIN
    // (handleUploadBegin) — this is the no-reconnect fallback so the device can't
    // sit upload-exclusive for half a minute.
    static constexpr uint32_t UPLOAD_TIMEOUT_MS = 8000;
    uint32_t _uploadLastActivity_ms = 0;

    static constexpr uint32_t MAX_UPLOAD_SIZE_FLASH = 2  * 1024 * 1024;
    static constexpr uint32_t MAX_UPLOAD_SIZE_SD    = 256 * 1024 * 1024;

    // ── Upload diagnostics snapshot (FILE_UPLOAD_DIAG) ────────────────
    // Frozen at every terminal point (complete / abort / cancel / stale-reset)
    // BEFORE cleanupUpload() resets the policy's writer stats, so a client that
    // saw a segment-ACK timeout can pull the real SD write latencies the stream
    // phase could never report over the wire.  Survives until the next BEGIN.
    struct UploadDiag {
        uint32_t bytesRecv      = 0;
        uint32_t expectedSize   = 0;
        uint16_t segIndex       = 0;
        uint16_t segCount       = 0;
        uint8_t  fillPct        = 0;
        uint32_t sdWriteCount   = 0;
        uint32_t sdBytesWritten = 0;
        uint32_t sdMaxLat_ms    = 0;   // worst single SD write — THE smoking gun
        uint32_t sdTotalStall_ms= 0;
        uint32_t maxLoopGap_ms  = 0;
        uint8_t  flags          = 0;   // bit0=uploadActive bit1=streamActive
        uint8_t  reason         = 0;   // StorageWire::UploadEndReason
    };
    UploadDiag _diag;
};

#endif  // SFX_STORAGE_UPLOAD_ENGINE_H
