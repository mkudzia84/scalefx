/*
 * page_cache.h — cross-core byte-stream page cache (test firmware version).
 *
 * Maps the same shape that will land in `sfx_storage`/`sfx_audio` once
 * validated here: ad-hoc PSRAM pages keyed by (path, byteOffset),
 * 256 KB max page size, 4 MB total budget, refcounted PageRef + LRU
 * eviction, async reader task on Core 1 lower priority than the
 * producer.
 *
 * Threading
 * ─────────
 *   Producer  : Core 1, prio MAX-2 — calls `acquire(path, byteOffset, len)`
 *               and drains the returned PageRef.  Blocks ONLY on
 *               `xSemaphoreGive(_readerSem)`, never on SD I/O.
 *   Reader    : Core 1, prio MAX-3 — created by `begin()`.  Pops slot
 *               pointers from a queue and does the fopen/fseek/fread/
 *               fclose dance on the SD via VFS-FAT POSIX.
 *
 *   The slot state-word is `std::atomic<uint8_t>` so the producer can
 *   poll readiness without taking the mutex.  Slot data buffers are
 *   PSRAM-allocated; the lookup table is itself in PSRAM (32 slots ×
 *   ~96 bytes = 3 KB).
 *
 * Serial telemetry
 * ────────────────
 *   Per-call hot path: nothing (would dominate stdout).
 *   On a load (start + done) the reader task emits one line each:
 *     [PageCache] LOAD start  slot=12 path=/sounds/foo.wav off=524288 size=262144
 *     [PageCache] LOAD done   slot=12 (89 ms)
 *   On eviction:
 *     [PageCache] EVICT slot=12 path=/sounds/bar.wav off=0 (LRU)
 *   Stats are queryable via `PageCache::instance().stats()` for the
 *   periodic telemetry sweep in main.ino.
 */

#ifndef PAGE_CACHE_TEST_H
#define PAGE_CACHE_TEST_H

#include <Arduino.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ── Configuration ─────────────────────────────────────────────────────

constexpr uint32_t kMaxPageBytes     = 256u * 1024u;      // 256 KB
constexpr uint32_t kPageBudgetBytes  = 4u   * 1024u * 1024u;  // 4 MB
constexpr int      kMaxPageSlots     = 32;
constexpr int      kPagePathMax      = 96;
constexpr int      kReaderQueueDepth = 16;

// ── Slot state machine ────────────────────────────────────────────────

enum class PageState : uint8_t {
    Free    = 0,
    Loading = 1,
    Ready   = 2,
    Idle    = 3,      // Ready but refcount == 0 → eviction candidate
    Failed  = 4,
};

struct PageSlot;    // opaque — definition in .cpp

// ── PageRef — RAII handle ─────────────────────────────────────────────

class PageRef {
public:
    PageRef() = default;
    PageRef(const PageRef&)            = delete;
    PageRef& operator=(const PageRef&) = delete;
    PageRef(PageRef&& o) noexcept;
    PageRef& operator=(PageRef&& o) noexcept;
    ~PageRef();

    bool           isValid()    const { return _slot != nullptr; }
    bool           isReady()    const;
    bool           isFailed()   const;
    const uint8_t* data()       const;
    uint32_t       size()       const;
    uint32_t       fileOffset() const;
    void           reset();

private:
    friend class PageCache;
    explicit PageRef(PageSlot* s) : _slot(s) {}
    PageSlot* _slot = nullptr;
};

// ── PageCache — singleton ─────────────────────────────────────────────

class PageCache {
public:
    static PageCache& instance() {
        static PageCache inst;
        return inst;
    }

    PageCache(const PageCache&)            = delete;
    PageCache& operator=(const PageCache&) = delete;

    /// Allocate slot table + reader task.  `readerCore=1` and
    /// `readerPriority=configMAX_PRIORITIES-3` are the production-target
    /// values; the producer is expected to run at MAX-2.
    bool begin(int readerCore = 1,
               int readerPriority = configMAX_PRIORITIES - 3,
               int readerStack    = 8192);

    void shutdown();

    /// Acquire a page covering `byteOffset` in `path`.  Caps page size
    /// to min(`kMaxPageBytes`, `bytesAvailable`).  Returns:
    ///   - hit  → ref with isReady()=true (cache lookup, no I/O)
    ///   - miss → ref with isReady()=false initially; flips after the
    ///            reader task fills it
    ///   - hard failure (all slots referenced) → invalid ref
    PageRef acquire(const char* path, uint32_t byteOffset, uint32_t bytesAvailable);

    // ── Stats (read from any task / core) ─────────────────────────────
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

    /// Verbose mode: print "LOAD start / done / EVICT" lines from the
    /// reader task as work happens.  Off by default to keep the output
    /// readable when the cache is hot.
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

    PageSlot*         _slots             = nullptr;
    SemaphoreHandle_t _muSem             = nullptr;   // table mutex
    SemaphoreHandle_t _readerSem         = nullptr;   // wake reader
    TaskHandle_t      _readerHandle      = nullptr;

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

#endif  // PAGE_CACHE_TEST_H
