/*
 * audio_asset_cache.h — PSRAM-resident asset cache.
 *
 *  Replaces the SD-streaming PageCache + AudioPageScheduler stack.
 *  Built on the conclusion (Test 3, May 2026) that two concurrent
 *  SDMMC-streaming sources cannot both stay fed under I²S↔SDMMC bus
 *  contention on ESP32-S3.  New model: every playable asset lives in
 *  PSRAM as a contiguous byte buffer.  Sources read directly from
 *  PSRAM during playback; SD is touched only during the initial
 *  background load.
 *
 *  Asset = the on-disk file's bytes verbatim.
 *      - For .wav: raw PCM (header included; source skips it on open)
 *      - For .mp3: compressed bitstream (decoded at runtime by source)
 *
 *  Streaming-load model
 *  ────────────────────
 *      `acquire(path)` returns immediately with a handle.  If the asset
 *      is new, a background loader task starts reading the file in
 *      256 KB chunks and updating `loadedBytes` atomically as each
 *      chunk lands.  The source can begin playback as soon as enough
 *      head bytes are resident to make forward progress (~32 KB for
 *      PCM, ~4 KB for MP3).  Subsequent acquire() of the same path
 *      returns instantly — the asset stays resident until evicted.
 *
 *  Retention (LRU + heatmap)
 *  ─────────────────────────
 *      Assets stay loaded after their refcount drops to 0 until
 *      another acquire() needs space.  Eviction picks the entry with
 *      the lowest `playCount × recency` score among unreferenced,
 *      unowned entries (owners[] all null AND refCount == 0).  Pinned
 *      (owned) or actively-referenced entries are immortal — pressure
 *      beyond their bytes returns an invalid handle (caller must fail
 *      closed).
 *
 *  Preload sets
 *  ────────────
 *      Each config consumer ("alerts", "enginefx", "gunfx", …) calls
 *      `setPreloadsForOwner(this, name, paths, count)` once per
 *      config-apply.  The cache diffs vs the owner's previous set
 *      and adjusts ownership atomically — new paths get queued, gone
 *      paths get released, unchanged ones are no-ops.  Owners are
 *      stored as opaque `void*` tokens (the caller's `this`); names
 *      are short literals used purely for diagnostics.  Per-entry
 *      owner capacity is `kMaxOwnersPerEntry` (4 — covers shared
 *      paths if multiple consumers reference the same MP3).
 *
 *  Threading
 *  ─────────
 *      - acquire/release/pin: any task (table mutex inside).
 *      - loadedBytes / refCount: lock-free atomics; sources poll
 *        without taking the mutex.
 *      - Loader task: single, Core 1 prio MAX-3.  Round-robins through
 *        partially-loaded entries one chunk at a time so concurrent
 *        new acquires all reach "playable head" quickly.
 */

#ifndef SFX_AUDIO_ASSET_CACHE_H
#define SFX_AUDIO_ASSET_CACHE_H

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "platform/sfx_platform.h"

#if SFX_PLATFORM_ESP32

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ── Configuration ────────────────────────────────────────────────────

/// Total PSRAM byte budget for resident asset data.  Aligned to fit
/// inside HubFX's 8 MB PSRAM with ~2 MB headroom for mixer buffers +
/// decoder scratch + housekeeping.
constexpr uint32_t kAssetCacheBudgetBytes = 6u * 1024u * 1024u;

/// Maximum number of cached asset entries.  Metadata-only structs
/// live in PSRAM next to the asset data.  Each entry is ~200 B; 64
/// slots = ~13 KB metadata.
constexpr int kAssetCacheMaxEntries = 64;

/// Path buffer per entry — must cover the longest realistic SD path.
constexpr int kAssetPathMax         = 160;

/// Chunk size for the background loader's per-iteration fread.
/// We fread directly into the entry's PSRAM destination; VFS-FAT
/// internally bounces through its own DMA-cap SRAM block (~1 MB/s).
/// Slower than an explicit internal-SRAM staging buffer, but avoids
/// snatching DMA-cap pool from I²S DMA descriptors at boot time.
constexpr uint32_t kAssetLoadChunkBytes = 32u * 1024u;

// ── Asset format tag ─────────────────────────────────────────────────

enum class AssetFormat : uint8_t {
    Unknown = 0,
    Wav     = 1,   ///< RIFF/WAVE container; source parses header at open
    Mp3     = 2,   ///< MPEG-1/2 Layer III; source feeds bytes to helix
};

// ── Preload owner book-keeping ───────────────────────────────────────
//
// Each entry tracks up to `kMaxOwners` "preload owners" — opaque
// pointers (typically `this` from a config-apply path).  An entry
// with any owner is implicitly pinned against LRU eviction.  When
// the last owner releases, the entry becomes evictable; if the same
// owner re-acquires it later, it's a cheap cache hit (assuming it
// hasn't been evicted in the meantime).  See setPreloadsForOwner().
//
// `ownerNames` is a parallel array of short literal labels ("alerts",
// "enginefx", …) used only for diagnostic logging.  Each slot's
// owner+name move in lock-step.
constexpr int kMaxOwnersPerEntry = 4;

// ── Asset entry (one per cached file) ────────────────────────────────

struct AssetEntry {
    char            path[kAssetPathMax] = {};

    // Storage
    uint8_t*        data       = nullptr;   ///< PSRAM buffer
    uint32_t        totalBytes = 0;         ///< file size in bytes
    std::atomic<uint32_t> loadedBytes{0};   ///< loader writes, sources read
    AssetFormat     format     = AssetFormat::Unknown;

    // Lifecycle
    std::atomic<int> refCount{0};           ///< number of live AssetHandles
    std::atomic<bool> failed{false};        ///< terminal load error

    // Preload owners — `pinned` is derived from owners[] being non-empty.
    const void*     owners    [kMaxOwnersPerEntry] = {};   // nullptr = empty
    const char*     ownerNames[kMaxOwnersPerEntry] = {};   // parallel, diag-only

    // Heatmap (LRU score)
    std::atomic<uint32_t> playCount{0};     ///< incremented on every acquire()
    uint64_t              lastAccessClock = 0;  ///< monotonic touch tick

    // Helpers (require holding the cache mutex).
    int  ownerCount_locked() const;
    bool isPinned_locked()   const { return ownerCount_locked() > 0; }
    bool hasOwner_locked(const void* token) const;
    bool addOwner_locked(const void* token, const char* name);   // false if full
    bool removeOwner_locked(const void* token);                  // false if absent
};

// ── AssetHandle — RAII reference ─────────────────────────────────────

class AssetHandle {
public:
    AssetHandle() = default;
    AssetHandle(const AssetHandle&)            = delete;
    AssetHandle& operator=(const AssetHandle&) = delete;
    AssetHandle(AssetHandle&&) noexcept;
    AssetHandle& operator=(AssetHandle&&) noexcept;
    ~AssetHandle();

    /// True iff this handle points at an entry that was successfully
    /// allocated (does not require load completion).
    bool isValid() const { return _entry != nullptr; }

    /// True iff at least `headBytes` are resident.  Sources call this
    /// before reading; if false, they should return 0 frames (caller
    /// covers the gap with silence) and try again next tick.
    bool hasHead(uint32_t headBytes) const;

    /// True iff the whole file is now resident.  After this returns
    /// true, all read positions up to `totalBytes()` are guaranteed
    /// valid PSRAM bytes.
    bool isFullyLoaded() const;

    /// True iff the loader hit a permanent error (SD read fail, etc.).
    /// Caller must release and abort the channel.
    bool isFailed() const;

    /// File format detected from the path extension at acquire() time.
    AssetFormat format() const;

    /// File size in bytes (immutable after acquire()).
    uint32_t totalBytes() const;

    /// Currently-resident byte count.  Grows monotonically during
    /// background load; equals totalBytes() once fully loaded.
    uint32_t loadedBytes() const;

    /// PSRAM buffer base.  Nullptr if !isValid().  The buffer length
    /// is `totalBytes()`; only bytes `[0, loadedBytes())` are
    /// guaranteed valid yet.
    const uint8_t* data() const;

    /// Path the handle was acquired with (for diagnostics).
    const char* path() const;

    /// Drop the ref (equivalent to `*this = {};`).
    void reset();

private:
    friend class AudioAssetCache;
    explicit AssetHandle(AssetEntry* e) : _entry(e) {}
    AssetEntry* _entry = nullptr;
};

// ── AudioAssetCache singleton ────────────────────────────────────────

class AudioAssetCache {
public:
    static AudioAssetCache& instance() {
        static AudioAssetCache inst;
        return inst;
    }

    AudioAssetCache(const AudioAssetCache&)            = delete;
    AudioAssetCache& operator=(const AudioAssetCache&) = delete;

    /// Start the loader task + allocate the entry table.  Idempotent.
    /// Loader defaults to Core 0 so it doesn't compete with the audio
    /// producer (Core 1 prio MAX-2) which never sleeps while there's
    /// any drain-buffer to feed.  Core 0 hosts loopTask + USB CDC +
    /// SDMMC ISR — the loader fits naturally between them.  Returns
    /// false on PSRAM alloc / task create failure.
    // Loader priority: the preload is a BACKGROUND prefetch with no
    // real-time deadline — it must sit BELOW every latency-critical Core-0
    // task.  The original configMAX-3 (22) outranked the jeti_in1 task (6)
    // and starved RC decode + telemetry replies + the ESC-telemetry drain
    // for the whole SD read burst whenever playback touched an uncached
    // sound (bench 2026-07-15: radio blackout + ESC telemetry loss while
    // audio played).  3 = above loopTask (1), below AudioDecoder (5) and
    // jeti_in1 (6) — preloads just take a little longer under load.
    bool begin(int loaderCore     = 0,
               int loaderPriority = 3,
               int loaderStack    = 8192);

    /// Stop loader, free every asset (active handles get failed=true),
    /// drop bounce buf.  Idempotent.
    void shutdown();

    /// Acquire a handle to the asset at `path`.  Behavior:
    ///   - already cached  → handle with isValid()=true, hasHead() may
    ///                       be true immediately (cache hit)
    ///   - new path        → allocates entry, enqueues loader; handle
    ///                       starts with loadedBytes()=0
    ///   - over budget but evictable → evicts LRU unowned, retries
    ///   - hard reject     → invalid handle
    ///
    /// `playCount` is incremented on every call (for LRU heatmap).
    AssetHandle acquire(const char* path);

    // ── Preload set (owner-token API) ───────────────────────────────
    //
    // Declare the set of paths an owner wants resident.  Diffs vs the
    // owner's previous set in one atomic operation:
    //   - paths in new-set, not previously       → preload queued, owner added
    //   - paths previously-in-set, not in new    → owner removed; if no
    //                                              owners remain, entry
    //                                              becomes evictable
    //   - unchanged                              → no-op (owner stays)
    //
    // `ownerToken` is any unique pointer — typically `this` from the
    // calling service.  Must be stable for the lifetime the owner
    // intends to hold the preloads.  `ownerName` is a short literal
    // ("alerts", "enginefx", …) logged in the diff line for
    // human-readable diagnostics; pointer is stored as-is, not copied,
    // so caller must use a string literal.  count=0 releases every
    // path this owner held (equivalent to releaseAllForOwner).
    //
    // Idempotent.  Safe to call repeatedly with the same list.
    void setPreloadsForOwner(const void* ownerToken,
                              const char* ownerName,
                              const char* const* paths,
                              int count);

    /// Convenience wrapper for `setPreloadsForOwner(token, "", nullptr, 0)`.
    /// Useful at service shutdown if we ever destroy services.
    void releaseAllForOwner(const void* ownerToken) {
        setPreloadsForOwner(ownerToken, "", nullptr, 0);
    }

    // ── Stats ────────────────────────────────────────────────────────
    struct Stats {
        uint32_t entries;            ///< populated slots
        uint32_t residentBytes;      ///< sum of loadedBytes across entries
        uint32_t pinnedEntries;      ///< entries with ≥ 1 owner
        uint32_t inFlightEntries;    ///< currently loading
        uint32_t failedEntries;      ///< terminal load failures
        uint32_t hits;
        uint32_t misses;
        uint32_t evictions;
        uint32_t budgetRejects;
        uint32_t loadFailures;
        uint32_t loadsCompleted;
        uint32_t avgLoadMs;
        uint32_t maxLoadMs;
    };
    Stats stats() const;
    void  resetStats();

    /// Emit one-line cache snapshot via SFX_LOG_INFO (safe from any task).
    void logSnapshot() const;

    /// Toggle verbose loader logging (LOAD start / chunk / done).
    void setVerbose(bool v) { _verbose.store(v, std::memory_order_release); }

    // ── Preload diagnostics ─────────────────────────────────────────
    enum class PreloadStatus : uint8_t {
        NotPresent = 0,   ///< no entry for this path
        Loading    = 1,   ///< allocated, loadedBytes < totalBytes
        Ready      = 2,   ///< fully resident
        Failed     = 3,   ///< loader hit a permanent error
    };
    struct PreloadInfo {
        char          path[kAssetPathMax];
        uint32_t      totalBytes;
        uint32_t      loadedBytes;
        PreloadStatus status;
        AssetFormat   format;
        int           ownerCount;
        const char*   ownerNames[kMaxOwnersPerEntry];   // up to 4
    };

    /// Snapshot one path.  Returns false if no entry exists.
    bool getPreloadInfo(const char* path, PreloadInfo& out) const;

    /// Walk every populated entry.  `cb` returns false to stop early.
    using PreloadVisitor = bool (*)(const PreloadInfo&, void* userData);
    void enumeratePreloads(PreloadVisitor cb, void* userData) const;

    /// One-line summary + per-path detail via SFX_LOG_INFO.  Cheap;
    /// safe to call from any task.  Skips emission entirely when no
    /// entries are populated (no noise on empty boards).
    void logPreloadStatus() const;

private:
    AudioAssetCache() = default;
    ~AudioAssetCache() { shutdown(); }

    friend class AssetHandle;
    static void loaderTaskFunc(void* arg);

    // table mgmt (all _locked variants require caller to hold _muSem)
    AssetEntry* findExisting_locked(const char* path);
    AssetEntry* allocateEntry_locked(const char* path, uint32_t totalBytes,
                                     AssetFormat fmt);
    bool        evictLRU_locked(uint32_t bytesNeeded);
    void        releaseEntry(AssetEntry* e);
    void        loadOneChunk(AssetEntry* e);   // called by loader task

    AssetEntry*       _entries     = nullptr;       ///< PSRAM table (slot array)
    SemaphoreHandle_t _muSem       = nullptr;
    SemaphoreHandle_t _loaderSem   = nullptr;       ///< counting; one tick per chunk
    TaskHandle_t      _loaderHandle = nullptr;

    std::atomic<bool>     _running       {false};
    std::atomic<bool>     _verbose       {false};
    std::atomic<uint32_t> _residentBytes {0};
    std::atomic<uint64_t> _lruClock      {0};

    std::atomic<uint32_t> _hits          {0};
    std::atomic<uint32_t> _misses        {0};
    std::atomic<uint32_t> _evictions     {0};
    std::atomic<uint32_t> _budgetRejects {0};
    std::atomic<uint32_t> _loadFailures  {0};
    std::atomic<uint32_t> _loadsCompleted{0};
    std::atomic<uint64_t> _totalLoadMs   {0};
    std::atomic<uint32_t> _maxLoadMs     {0};

    void lock_()   { xSemaphoreTake(_muSem, portMAX_DELAY); }
    void unlock_() { xSemaphoreGive(_muSem); }

    /// Extension → format detection ("/foo/bar.WAV" → Wav).
    static AssetFormat detectFormat(const char* path);
};

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_AUDIO_ASSET_CACHE_H
