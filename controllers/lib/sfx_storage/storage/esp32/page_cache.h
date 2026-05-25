/*
 * page_cache.h — ad-hoc PSRAM page cache for SD-backed byte streams.
 *
 * Validated in the page_cache_test bench harness (tests/hw/
 * page_cache_test/) before promotion to this library.  Production
 * uses it for paged WAV playback: the audio producer task drains
 * pages while a separate reader task fills them in the background,
 * giving zero-underrun audio playback at SD read latencies of
 * 100–200 ms.
 *
 * Keying
 * ──────
 *   Pages are keyed by (path, byteOffset).  The path is the
 *   user-visible relative path (e.g. "/sounds/foo.wav"); the reader
 *   task prefixes the SD VFS mount point ("/sdcard") before fopen.
 *   `byteOffset` is the absolute byte position into the file (NOT a
 *   page index).
 *
 * Sizing
 * ──────
 *   Page size: ad-hoc per acquire(), `min(kMaxPageBytes, bytesAvailable)`.
 *   Total resident: bounded by `kPageBudgetBytes` (defaults to 4 MB on
 *   ESP32-S3); LRU eviction of unreferenced (Idle) slots reclaims under
 *   pressure.  Pages live in PSRAM.
 *
 * Threading
 * ─────────
 *   Producer  (typical: Core 1 prio MAX-2) calls `acquire()` and drains
 *               the returned PageRef.  Blocks only on the queue semaphore
 *               (never on SD I/O).
 *   Reader    (Core 1 prio MAX-3) — created by `begin()`.  Pops slot
 *               pointers from a queue and does fopen/fseek/fread/fclose
 *               via POSIX VFS-FAT.
 *
 *   The slot state-word is atomic so producer can check readiness
 *   without taking the table mutex.  FF_FS_REENTRANT=1 in the bundled
 *   FATFS makes the concurrent fopen/fread/fwrite from Core 0 and
 *   Core 1 task-safe at the VFS layer.
 *
 * Reader-side file-handle cache
 * ─────────────────────────────
 *   The reader keeps up to 4 FILE* handles alive across page loads
 *   (LRU) so repeat reads of the same file skip the fopen/fclose
 *   overhead — typically >95 % hit rate during steady-state streaming.
 *
 * Lifecycle
 * ─────────
 *   PageCache::instance().begin(core, priority, stack)
 *      → allocates slot table (PSRAM), creates reader task
 *   PageCache::instance().shutdown()
 *      → reader stop, all pages freed, file-handle cache flushed
 */

#ifndef SFX_STORAGE_PAGE_CACHE_H
#define SFX_STORAGE_PAGE_CACHE_H

#include <cstdint>
#include <cstddef>
#include <atomic>
#include "platform/sfx_platform.h"

#if SFX_PLATFORM_ESP32

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ── Configuration ────────────────────────────────────────────────────

/// Maximum bytes per page (file tails may be shorter).  256 KB ≈ 1.3 s
/// of 16-bit stereo at 48 kHz.
constexpr uint32_t kMaxPageBytes     = 256u * 1024u;

/// Total PSRAM budget for resident pages.  ~16 slots at full size.
constexpr uint32_t kPageBudgetBytes  = 4u * 1024u * 1024u;

/// Maximum tracked slots.  Slot metadata ~96 bytes each (in PSRAM).
constexpr int kMaxPageSlots          = 32;

/// Path buffer per slot — caps the user-relative path length.
constexpr int kPagePathMax           = 96;

/// Async reader task pending-queue depth.
constexpr int kReaderQueueDepth      = 16;

// ── Slot state machine ──────────────────────────────────────────────

enum class PageState : uint8_t {
    Free    = 0,   ///< Empty slot, no PSRAM held.
    Loading = 1,   ///< PSRAM allocated, reader task in flight.
    Ready   = 2,   ///< PSRAM populated, data() readable.
    Idle    = 3,   ///< Ready but refcount=0 — LRU eviction candidate.
    Failed  = 4,   ///< Load failed; data invalid.
};

struct PageSlot;   // opaque (defined in .cpp)

// ── PageRef — RAII handle ───────────────────────────────────────────
//
// Move-only.  Holding a PageRef pins the slot against eviction.  On
// destruction the refcount decrements; if it drops to 0, the slot
// transitions to Idle and becomes an LRU eviction candidate.

class PageRef {
public:
    PageRef() = default;
    PageRef(const PageRef&)            = delete;
    PageRef& operator=(const PageRef&) = delete;
    PageRef(PageRef&&) noexcept;
    PageRef& operator=(PageRef&&) noexcept;
    ~PageRef();

    /// True iff this ref points at a real slot (Loading / Ready / Idle).
    /// Default-constructed and load-failure refs return false.
    bool           isValid()    const { return _slot != nullptr; }

    /// True iff the slot's load has completed and bytes are readable.
    /// Producer polls this; if false, treat as underrun and stall briefly.
    bool           isReady()    const;

    /// True iff the load attempt completed in failure (SD error / OOM).
    /// Caller should release this ref and abort the channel.
    bool           isFailed()   const;

    /// Raw page bytes (RAW PCM, not float).  Nullptr if not Ready.
    const uint8_t* data()       const;

    /// Number of valid bytes in `data()`.  May be less than kMaxPageBytes
    /// for the last page of a file.
    uint32_t       size()       const;

    /// Byte offset within the source file (NOT into the page).
    uint32_t       fileOffset() const;

    /// Drop the ref (== `*this = {};`).
    void           reset();

private:
    friend class PageCache;
    explicit PageRef(PageSlot* s) : _slot(s) {}
    PageSlot* _slot = nullptr;
};


// ── PageCache singleton ─────────────────────────────────────────────

class PageCache {
public:
    static PageCache& instance() {
        static PageCache inst;
        return inst;
    }

    PageCache(const PageCache&)            = delete;
    PageCache& operator=(const PageCache&) = delete;

    /// Allocate slot table (PSRAM) + create reader task.  Producer
    /// runs at `priority + 1` typically.  Idempotent.  Returns false
    /// on PSRAM alloc / task create failure.
    bool begin(int readerCore     = 1,
               int readerPriority = configMAX_PRIORITIES - 3,
               int readerStack    = 8192);

    /// Stop reader, free all pages, drop file-handle cache.  Idempotent.
    void shutdown();

    /// Acquire a page covering `byteOffset` in `path`.  `bytesAvailable`
    /// caps the page size if the file ends mid-page.  Returns:
    ///   - hit  → ref with `isReady()==true`, no SD I/O
    ///   - miss → ref with `isReady()==false` initially; flips to true
    ///            once reader fills the slot
    ///   - hard failure (all slots referenced, no eviction possible)
    ///        → invalid ref (`isValid()==false`)
    PageRef acquire(const char* path,
                    uint32_t    byteOffset,
                    uint32_t    bytesAvailable);

    // ── Stats (read from any task / core) ───────────────────────────
    struct Stats {
        uint32_t hits;
        uint32_t misses;
        uint32_t evictions;
        uint32_t budgetReject;
        uint32_t loadFailures;
        uint32_t loadsCompleted;
        uint32_t avgLoadMs;
        uint32_t maxLoadMs;
        uint32_t residentPages;
        uint32_t residentBytes;
        int      queueDepth;
    };
    Stats stats() const;
    void  resetStats();

    /// Verbose mode prints LOAD start / done / EVICT lines from the
    /// reader as work happens.  Off by default to keep the log readable.
    void setVerbose(bool v) { _verbose.store(v, std::memory_order_release); }

private:
    PageCache() = default;
    ~PageCache() { shutdown(); }

    friend class PageRef;
    static void readerTaskFunc(void* arg);

    PageSlot* findExisting_locked(const char* path, uint32_t off);
    PageSlot* allocateSlot_locked(const char* path, uint32_t off, uint32_t bytes);
    void      releaseSlot(PageSlot* s);
    void      drainOneLoad();

    PageSlot*         _slots         = nullptr;
    SemaphoreHandle_t _muSem         = nullptr;
    SemaphoreHandle_t _readerSem     = nullptr;
    TaskHandle_t      _readerHandle  = nullptr;

    std::atomic<bool>     _running       {false};
    std::atomic<bool>     _verbose       {false};
    std::atomic<uint32_t> _residentBytes {0};
    std::atomic<uint64_t> _lruClock      {0};

    std::atomic<uint32_t> _hits          {0};
    std::atomic<uint32_t> _misses        {0};
    std::atomic<uint32_t> _evictions     {0};
    std::atomic<uint32_t> _budgetReject  {0};
    std::atomic<uint32_t> _loadFailures  {0};
    std::atomic<uint32_t> _loadsCompleted{0};
    std::atomic<uint64_t> _totalLoadMs   {0};
    std::atomic<uint32_t> _maxLoadMs     {0};

    void lock_()   { xSemaphoreTake(_muSem, portMAX_DELAY); }
    void unlock_() { xSemaphoreGive(_muSem); }
};


#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_STORAGE_PAGE_CACHE_H
