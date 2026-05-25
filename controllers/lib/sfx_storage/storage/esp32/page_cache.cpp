/*
 * page_cache.cpp — implementation.
 *
 * Reader task algorithm
 * ─────────────────────
 *   1. Block on `_readerSem` (counting semaphore — one tick per
 *      enqueued load).
 *   2. Pop one slot pointer from the queue (under table mutex).
 *   3. Open / fseek / fread / leave the file handle in the LRU
 *      file-cache for the next call.
 *   4. Transition slot state Loading → Ready or Loading → Failed
 *      via release-store so the producer sees it without taking
 *      the table mutex.
 *
 * Path discipline
 * ───────────────
 *   Caller passes user-relative paths ("/sounds/foo.wav"); the reader
 *   prefixes the SD VFS mount ("/sdcard") before fopen.  The SD card
 *   module must already be mounted by the time `begin()` is called.
 */

#include "page_cache.h"

#if SFX_PLATFORM_ESP32

#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <serial/diag_log.h>

#define PAGE_LOG(fmt, ...)   SFX_LOG_INFO ("[PageCache] " fmt, ##__VA_ARGS__)
#define PAGE_WARN(fmt, ...)  SFX_LOG_WARN ("[PageCache] " fmt, ##__VA_ARGS__)
#define PAGE_ERROR(fmt, ...) SFX_LOG_ERROR("[PageCache] " fmt, ##__VA_ARGS__)


// ──────────────────────────────────────────────────────────────────────
//  Reader-task file-handle cache
// ──────────────────────────────────────────────────────────────────────
//
// Per-page fopen/fclose dominates SD load latency once the actual data
// transfer is fast.  This tiny LRU keeps FILE* handles alive across
// page loads of the same file — >95 % hit rate during streaming,
// drops per-page open overhead from ~50 ms to ~1 ms (fseek only).
//
// Lives in the reader task ONLY — no other task touches it, no locking.
// Flushed on shutdown via `closeAllFiles()`.

namespace {
constexpr int kFileCacheSlots = 4;

struct FileCacheEntry {
    char     path[kPagePathMax] = {};
    FILE*    fp                 = nullptr;
    uint32_t lastUsedMs         = 0;
};
FileCacheEntry s_fileCache[kFileCacheSlots];

FILE* openOrReuse(const char* fullPath, const char* userPath) {
    const uint32_t now = millis();
    // Direct hit
    for (auto& e : s_fileCache) {
        if (e.fp && strncmp(e.path, userPath, kPagePathMax) == 0) {
            e.lastUsedMs = now;
            return e.fp;
        }
    }
    // Empty slot, else LRU-evict the stalest.
    FileCacheEntry* victim = nullptr;
    uint32_t        lruAge = 0;
    for (auto& e : s_fileCache) {
        if (!e.fp) { victim = &e; break; }
        const uint32_t age = now - e.lastUsedMs;
        if (!victim || age > lruAge) { victim = &e; lruAge = age; }
    }
    if (victim->fp) {
        fclose(victim->fp);
        victim->fp      = nullptr;
        victim->path[0] = '\0';
    }
    victim->fp = fopen(fullPath, "rb");
    if (!victim->fp) return nullptr;
    strncpy(victim->path, userPath, kPagePathMax - 1);
    victim->path[kPagePathMax - 1] = '\0';
    victim->lastUsedMs = now;
    return victim->fp;
}

void closeAllFiles() {
    for (auto& e : s_fileCache) {
        if (e.fp) { fclose(e.fp); e.fp = nullptr; }
        e.path[0] = '\0';
    }
}
}  // namespace


// ──────────────────────────────────────────────────────────────────────
//  PageSlot (full definition — opaque to header)
// ──────────────────────────────────────────────────────────────────────

struct PageSlot {
    std::atomic<PageState> state{PageState::Free};

    char       path[kPagePathMax] = {};
    uint32_t   fileOffset         = 0;
    uint32_t   bytesRequested     = 0;
    uint32_t   bytesValid         = 0;
    uint32_t   capacity           = 0;
    uint8_t*   bytes              = nullptr;

    std::atomic<int>      refcount{0};
    std::atomic<uint64_t> lruTick {0};
    uint32_t              loadStartMs = 0;
};

static PageSlot* s_readerQueue[kReaderQueueDepth] = {};
static int       s_qHead = 0;
static int       s_qTail = 0;


// ──────────────────────────────────────────────────────────────────────
//  PageRef
// ──────────────────────────────────────────────────────────────────────

PageRef::PageRef(PageRef&& o) noexcept : _slot(o._slot) { o._slot = nullptr; }
PageRef& PageRef::operator=(PageRef&& o) noexcept {
    if (this != &o) {
        reset();
        _slot   = o._slot;
        o._slot = nullptr;
    }
    return *this;
}
PageRef::~PageRef() { reset(); }

void PageRef::reset() {
    if (_slot) {
        PageCache::instance().releaseSlot(_slot);
        _slot = nullptr;
    }
}
bool PageRef::isReady() const {
    if (!_slot) return false;
    auto s = _slot->state.load(std::memory_order_acquire);
    return s == PageState::Ready || s == PageState::Idle;
}
bool PageRef::isFailed() const {
    if (!_slot) return false;
    return _slot->state.load(std::memory_order_acquire) == PageState::Failed;
}
const uint8_t* PageRef::data() const {
    if (!_slot) return nullptr;
    return isReady() ? _slot->bytes : nullptr;
}
uint32_t PageRef::size()       const { return _slot && isReady() ? _slot->bytesValid : 0; }
uint32_t PageRef::fileOffset() const { return _slot ? _slot->fileOffset : 0; }


// ──────────────────────────────────────────────────────────────────────
//  Lifecycle
// ──────────────────────────────────────────────────────────────────────

bool PageCache::begin(int readerCore, int readerPriority, int readerStack) {
    if (_running.load(std::memory_order_acquire)) return true;

    _slots = (PageSlot*)heap_caps_calloc(kMaxPageSlots, sizeof(PageSlot),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_slots) {
        PAGE_ERROR("slot-table alloc failed (%u B)",
                   (unsigned)(kMaxPageSlots * sizeof(PageSlot)));
        return false;
    }
    for (int i = 0; i < kMaxPageSlots; ++i) new (&_slots[i]) PageSlot();

    _muSem     = xSemaphoreCreateMutex();
    _readerSem = xSemaphoreCreateCounting(kReaderQueueDepth, 0);
    if (!_muSem || !_readerSem) {
        PAGE_ERROR("semaphore create failed");
        if (_muSem)     { vSemaphoreDelete(_muSem);     _muSem     = nullptr; }
        if (_readerSem) { vSemaphoreDelete(_readerSem); _readerSem = nullptr; }
        heap_caps_free(_slots);
        _slots = nullptr;
        return false;
    }

    s_qHead = s_qTail = 0;
    for (int i = 0; i < kReaderQueueDepth; ++i) s_readerQueue[i] = nullptr;

    _residentBytes.store(0, std::memory_order_relaxed);
    _lruClock.store(0, std::memory_order_relaxed);
    resetStats();

    _running.store(true, std::memory_order_release);

    BaseType_t r = xTaskCreatePinnedToCore(&PageCache::readerTaskFunc,
                                            "PageReader",
                                            readerStack, this,
                                            readerPriority, &_readerHandle,
                                            readerCore);
    if (r != pdPASS) {
        PAGE_ERROR("reader task create failed");
        _running.store(false, std::memory_order_release);
        return false;
    }

    PAGE_LOG("ready: slots=%d budget=%u KB page=%u KB reader core=%d prio=%d",
             kMaxPageSlots,
             (unsigned)(kPageBudgetBytes / 1024),
             (unsigned)(kMaxPageBytes / 1024),
             readerCore, readerPriority);
    return true;
}

void PageCache::shutdown() {
    if (!_running.exchange(false, std::memory_order_acq_rel)) return;
    if (_readerSem) xSemaphoreGive(_readerSem);
    for (int i = 0; i < 20 && _readerHandle; ++i) vTaskDelay(pdMS_TO_TICKS(5));
    if (_readerHandle) { vTaskDelete(_readerHandle); _readerHandle = nullptr; }
    closeAllFiles();
    if (_readerSem) { vSemaphoreDelete(_readerSem); _readerSem = nullptr; }
    if (_muSem)     { vSemaphoreDelete(_muSem);     _muSem     = nullptr; }
    if (_slots) {
        for (int i = 0; i < kMaxPageSlots; ++i) {
            if (_slots[i].bytes) heap_caps_free(_slots[i].bytes);
            _slots[i].~PageSlot();
        }
        heap_caps_free(_slots);
        _slots = nullptr;
    }
}


// ──────────────────────────────────────────────────────────────────────
//  Acquire / release
// ──────────────────────────────────────────────────────────────────────

PageSlot* PageCache::findExisting_locked(const char* path, uint32_t off) {
    for (int i = 0; i < kMaxPageSlots; ++i) {
        PageSlot& s = _slots[i];
        if (s.state.load(std::memory_order_acquire) == PageState::Free) continue;
        if (s.fileOffset != off) continue;
        if (strncmp(s.path, path, kPagePathMax) != 0) continue;
        return &s;
    }
    return nullptr;
}

PageSlot* PageCache::allocateSlot_locked(const char* path, uint32_t off,
                                         uint32_t bytes) {
    // 1. Free slot first.
    PageSlot* victim = nullptr;
    for (int i = 0; i < kMaxPageSlots; ++i) {
        if (_slots[i].state.load(std::memory_order_acquire) == PageState::Free) {
            victim = &_slots[i];
            break;
        }
    }
    // 2. None free → LRU-evict an Idle/Failed unreferenced slot.
    if (!victim) {
        uint64_t  lruMin = UINT64_MAX;
        PageSlot* cand   = nullptr;
        for (int i = 0; i < kMaxPageSlots; ++i) {
            PageSlot& s = _slots[i];
            auto st = s.state.load(std::memory_order_acquire);
            if (st != PageState::Idle && st != PageState::Failed) continue;
            if (s.refcount.load(std::memory_order_acquire) != 0) continue;
            uint64_t t = s.lruTick.load(std::memory_order_acquire);
            if (t < lruMin) { lruMin = t; cand = &s; }
        }
        victim = cand;
        if (victim) {
            if (_verbose.load(std::memory_order_acquire)) {
                PAGE_LOG("EVICT path=%s off=%u (LRU)",
                         victim->path, (unsigned)victim->fileOffset);
            }
            if (victim->bytes) {
                _residentBytes.fetch_sub(victim->capacity, std::memory_order_acq_rel);
                heap_caps_free(victim->bytes);
                victim->bytes = nullptr;
            }
            victim->capacity   = 0;
            victim->bytesValid = 0;
            victim->state.store(PageState::Free, std::memory_order_release);
            _evictions.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!victim) {
        _budgetReject.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // 3. Honour the global byte budget — evict additional Idle slots.
    while (_residentBytes.load(std::memory_order_acquire) + bytes > kPageBudgetBytes) {
        uint64_t  lruMin = UINT64_MAX;
        PageSlot* extra  = nullptr;
        for (int i = 0; i < kMaxPageSlots; ++i) {
            PageSlot& s = _slots[i];
            if (&s == victim) continue;
            auto st = s.state.load(std::memory_order_acquire);
            if (st != PageState::Idle && st != PageState::Failed) continue;
            if (s.refcount.load(std::memory_order_acquire) != 0) continue;
            uint64_t t = s.lruTick.load(std::memory_order_acquire);
            if (t < lruMin) { lruMin = t; extra = &s; }
        }
        if (!extra) {
            victim->state.store(PageState::Free, std::memory_order_release);
            _budgetReject.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (extra->bytes) {
            _residentBytes.fetch_sub(extra->capacity, std::memory_order_acq_rel);
            heap_caps_free(extra->bytes);
            extra->bytes = nullptr;
        }
        extra->capacity   = 0;
        extra->bytesValid = 0;
        extra->state.store(PageState::Free, std::memory_order_release);
        _evictions.fetch_add(1, std::memory_order_relaxed);
    }
    // 4. Allocate page bytes.
    victim->bytes = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!victim->bytes) {
        _budgetReject.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    _residentBytes.fetch_add(bytes, std::memory_order_acq_rel);

    strncpy(victim->path, path, kPagePathMax - 1);
    victim->path[kPagePathMax - 1] = '\0';
    victim->fileOffset     = off;
    victim->bytesRequested = bytes;
    victim->bytesValid     = 0;
    victim->capacity       = bytes;
    victim->refcount.store(0, std::memory_order_release);
    victim->lruTick.store(_lruClock.fetch_add(1, std::memory_order_acq_rel) + 1,
                          std::memory_order_release);
    victim->state.store(PageState::Loading, std::memory_order_release);
    return victim;
}

PageRef PageCache::acquire(const char* path, uint32_t off, uint32_t avail) {
    if (!_running.load(std::memory_order_acquire) || !path || !_slots) return {};
    const uint32_t need = (avail < kMaxPageBytes) ? avail : kMaxPageBytes;
    if (!need) return {};

    lock_();

    PageSlot* slot = findExisting_locked(path, off);
    if (slot) {
        slot->refcount.fetch_add(1, std::memory_order_acq_rel);
        slot->lruTick.store(_lruClock.fetch_add(1, std::memory_order_acq_rel) + 1,
                            std::memory_order_release);
        auto st = slot->state.load(std::memory_order_acquire);
        if (st == PageState::Idle) {
            slot->state.store(PageState::Ready, std::memory_order_release);
        }
        unlock_();
        _hits.fetch_add(1, std::memory_order_relaxed);
        return PageRef{slot};
    }

    slot = allocateSlot_locked(path, off, need);
    if (!slot) { unlock_(); return {}; }
    slot->refcount.fetch_add(1, std::memory_order_acq_rel);

    int next = (s_qHead + 1) % kReaderQueueDepth;
    if (next == s_qTail) {
        // Queue full — refuse the load, mark Failed so the caller
        // gets a "load failed" signal rather than waiting forever.
        slot->state.store(PageState::Failed, std::memory_order_release);
        _loadFailures.fetch_add(1, std::memory_order_relaxed);
        unlock_();
        PAGE_WARN("reader queue full — load REJECTED");
        return PageRef{slot};
    }
    s_readerQueue[s_qHead] = slot;
    s_qHead = next;
    slot->loadStartMs = millis();

    unlock_();
    xSemaphoreGive(_readerSem);
    _misses.fetch_add(1, std::memory_order_relaxed);
    if (_verbose.load(std::memory_order_acquire)) {
        PAGE_LOG("LOAD start  path=%s off=%u size=%u",
                 slot->path, (unsigned)slot->fileOffset, (unsigned)slot->capacity);
    }
    return PageRef{slot};
}

void PageCache::releaseSlot(PageSlot* s) {
    if (!s) return;
    int r = s->refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r < 0) { s->refcount.store(0, std::memory_order_release); return; }
    if (r == 0 && s->state.load(std::memory_order_acquire) == PageState::Ready) {
        s->state.store(PageState::Idle, std::memory_order_release);
    }
}


// ──────────────────────────────────────────────────────────────────────
//  Reader task
// ──────────────────────────────────────────────────────────────────────

void PageCache::readerTaskFunc(void* arg) {
    auto* self = (PageCache*)arg;
    PAGE_LOG("reader running on core %d prio %d",
             xPortGetCoreID(), uxTaskPriorityGet(nullptr));
    while (self->_running.load(std::memory_order_acquire)) {
        if (xSemaphoreTake(self->_readerSem, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!self->_running.load(std::memory_order_acquire)) break;
        self->drainOneLoad();
    }
    PAGE_LOG("reader exiting");
    vTaskDelete(nullptr);
}

void PageCache::drainOneLoad() {
    lock_();
    PageSlot* slot = nullptr;
    if (s_qTail != s_qHead) {
        slot = s_readerQueue[s_qTail];
        s_readerQueue[s_qTail] = nullptr;
        s_qTail = (s_qTail + 1) % kReaderQueueDepth;
    }
    unlock_();
    if (!slot) return;
    if (slot->state.load(std::memory_order_acquire) != PageState::Loading) return;

    // SD I/O.  Mount path is "/sdcard"; the slot's path is user-visible.
    char full[160];
    const bool needSlash = (slot->path[0] != '/');
    snprintf(full, sizeof(full), "/sdcard%s%s",
             needSlash ? "/" : "", slot->path);

    FILE* fp = openOrReuse(full, slot->path);
    if (!fp) {
        PAGE_ERROR("LOAD FAIL fopen(%s) errno=%d", full, errno);
        slot->state.store(PageState::Failed, std::memory_order_release);
        _loadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (fseek(fp, (long)slot->fileOffset, SEEK_SET) != 0) {
        PAGE_ERROR("LOAD FAIL fseek path=%s off=%u errno=%d",
                   slot->path, (unsigned)slot->fileOffset, errno);
        slot->state.store(PageState::Failed, std::memory_order_release);
        _loadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const size_t n = fread(slot->bytes, 1, slot->capacity, fp);
    if (n == 0) {
        PAGE_ERROR("LOAD FAIL fread path=%s off=%u",
                   slot->path, (unsigned)slot->fileOffset);
        slot->state.store(PageState::Failed, std::memory_order_release);
        _loadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    slot->bytesValid = (uint32_t)n;
    slot->state.store(PageState::Ready, std::memory_order_release);

    const uint32_t latency = millis() - slot->loadStartMs;
    _loadsCompleted.fetch_add(1, std::memory_order_relaxed);
    _totalLoadMs.fetch_add(latency, std::memory_order_relaxed);
    uint32_t prev = _maxLoadMs.load(std::memory_order_relaxed);
    while (latency > prev &&
           !_maxLoadMs.compare_exchange_weak(prev, latency,
                                              std::memory_order_relaxed)) {}

    if (_verbose.load(std::memory_order_acquire)) {
        PAGE_LOG("LOAD done   path=%s off=%u read=%u/%u (%u ms)",
                 slot->path, (unsigned)slot->fileOffset,
                 (unsigned)n, (unsigned)slot->capacity, (unsigned)latency);
    }
}


// ──────────────────────────────────────────────────────────────────────
//  Stats
// ──────────────────────────────────────────────────────────────────────

PageCache::Stats PageCache::stats() const {
    Stats s{};
    s.hits           = _hits.load(std::memory_order_acquire);
    s.misses         = _misses.load(std::memory_order_acquire);
    s.evictions      = _evictions.load(std::memory_order_acquire);
    s.budgetReject   = _budgetReject.load(std::memory_order_acquire);
    s.loadFailures   = _loadFailures.load(std::memory_order_acquire);
    s.loadsCompleted = _loadsCompleted.load(std::memory_order_acquire);
    uint64_t total   = _totalLoadMs.load(std::memory_order_acquire);
    s.avgLoadMs      = s.loadsCompleted ? (uint32_t)(total / s.loadsCompleted) : 0;
    s.maxLoadMs      = _maxLoadMs.load(std::memory_order_acquire);
    s.residentBytes  = _residentBytes.load(std::memory_order_acquire);

    uint32_t resident = 0;
    if (_slots) {
        for (int i = 0; i < kMaxPageSlots; ++i) {
            if (_slots[i].state.load(std::memory_order_acquire) != PageState::Free)
                resident++;
        }
    }
    s.residentPages = resident;
    int qd = s_qHead - s_qTail;
    if (qd < 0) qd += kReaderQueueDepth;
    s.queueDepth = qd;
    return s;
}

void PageCache::resetStats() {
    _hits.store(0, std::memory_order_relaxed);
    _misses.store(0, std::memory_order_relaxed);
    _evictions.store(0, std::memory_order_relaxed);
    _budgetReject.store(0, std::memory_order_relaxed);
    _loadFailures.store(0, std::memory_order_relaxed);
    _loadsCompleted.store(0, std::memory_order_relaxed);
    _totalLoadMs.store(0, std::memory_order_relaxed);
    _maxLoadMs.store(0, std::memory_order_relaxed);
}

#endif  // SFX_PLATFORM_ESP32
