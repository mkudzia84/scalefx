/*
 * audio_asset_cache.cpp — implementation.
 *
 *  Loader algorithm
 *  ────────────────
 *      The loader task blocks on `_loaderSem`.  Every acquire() that
 *      enqueues a new load posts one tick; the loader's main loop wakes,
 *      picks the entry whose proportional load (`loadedBytes/totalBytes`)
 *      is LOWEST among still-loading entries, reads ONE chunk into the
 *      shared internal-SRAM bounce buffer, copies it to the entry's
 *      PSRAM destination, and updates `loadedBytes` with a release
 *      store.  If the entry still has more bytes to read, it self-posts
 *      another tick so it stays in rotation.  Once `loadedBytes ==
 *      totalBytes`, the file handle is closed and counters bumped.
 *
 *      "Pick the one furthest behind" gives natural fairness when
 *      several assets are acquired close together — a brand-new
 *      acquire (loadedBytes=0) jumps to the head until it has some
 *      bytes, then yields to whatever is in second place.
 *
 *  Bounce buffer
 *  ─────────────
 *      Allocated ONCE at begin() from MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
 *      so SDMMC IDMA writes directly into it (no PSRAM-destination
 *      penalty).  The loader memcpy's the chunk into PSRAM after each
 *      fread.  Net SD throughput: ~14 MB/s — same as the legacy
 *      page_cache DMA-cap path.
 */

#include "audio_asset_cache.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#if SFX_PLATFORM_ESP32

#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <serial/diag_log.h>
#include "storage/sd_card.h"   // SdCardModule::lock()/unlock() — serializes
                                // SD operations across all consumers (asset
                                // loader, WavStreamSource fallback, upload
                                // protocol, config save) so they don't race
                                // through VFS-FAT's bare reentrancy.

#define AC_LOG(fmt, ...)   SFX_LOG_INFO ("[AssetCache] " fmt, ##__VA_ARGS__)
#define AC_WARN(fmt, ...)  SFX_LOG_WARN ("[AssetCache] " fmt, ##__VA_ARGS__)
#define AC_ERROR(fmt, ...) SFX_LOG_ERROR("[AssetCache] " fmt, ##__VA_ARGS__)


// ─── AssetHandle impl ───────────────────────────────────────────────

AssetHandle::AssetHandle(AssetHandle&& o) noexcept : _entry(o._entry) {
    o._entry = nullptr;
}

AssetHandle& AssetHandle::operator=(AssetHandle&& o) noexcept {
    if (this != &o) {
        reset();
        _entry   = o._entry;
        o._entry = nullptr;
    }
    return *this;
}

AssetHandle::~AssetHandle() { reset(); }

void AssetHandle::reset() {
    if (!_entry) return;
    // Decrement refcount; entry stays in cache until LRU evicts it.
    _entry->refCount.fetch_sub(1, std::memory_order_acq_rel);
    _entry = nullptr;
}

bool AssetHandle::hasHead(uint32_t headBytes) const {
    if (!_entry) return false;
    const uint32_t loaded = _entry->loadedBytes.load(std::memory_order_acquire);
    return loaded >= headBytes || loaded == _entry->totalBytes;
}

bool AssetHandle::isFullyLoaded() const {
    if (!_entry) return false;
    return _entry->loadedBytes.load(std::memory_order_acquire) == _entry->totalBytes;
}

bool AssetHandle::isFailed() const {
    return _entry && _entry->failed.load(std::memory_order_acquire);
}

AssetFormat AssetHandle::format() const {
    return _entry ? _entry->format : AssetFormat::Unknown;
}

uint32_t AssetHandle::totalBytes() const {
    return _entry ? _entry->totalBytes : 0;
}

uint32_t AssetHandle::loadedBytes() const {
    return _entry ? _entry->loadedBytes.load(std::memory_order_acquire) : 0;
}

const uint8_t* AssetHandle::data() const {
    return _entry ? _entry->data : nullptr;
}

const char* AssetHandle::path() const {
    return _entry ? _entry->path : "";
}


// ─── AudioAssetCache impl ───────────────────────────────────────────

bool AudioAssetCache::begin(int loaderCore, int loaderPriority, int loaderStack) {
    if (_running.load(std::memory_order_acquire)) return true;

    // Slot table in PSRAM (cold metadata, accessed mostly under mutex).
    _entries = (AssetEntry*)heap_caps_calloc(
        kAssetCacheMaxEntries, sizeof(AssetEntry),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_entries) {
        AC_ERROR("PSRAM alloc failed for entry table (%u B)",
                 (unsigned)(kAssetCacheMaxEntries * sizeof(AssetEntry)));
        return false;
    }
    // Re-init via placement-new because heap_caps_calloc returns raw bytes
    // and AssetEntry has std::atomic members that need their constructors.
    for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
        new (&_entries[i]) AssetEntry();
    }

    _muSem     = xSemaphoreCreateMutex();
    _loaderSem = xSemaphoreCreateCounting(kAssetCacheMaxEntries * 256, 0);
    if (!_muSem || !_loaderSem) {
        AC_ERROR("semaphore create failed");
        if (_muSem)     vSemaphoreDelete(_muSem);
        if (_loaderSem) vSemaphoreDelete(_loaderSem);
        heap_caps_free(_entries);   _entries   = nullptr;
        return false;
    }

    _running.store(true, std::memory_order_release);

    BaseType_t ok = xTaskCreatePinnedToCore(
        loaderTaskFunc, "asset_loader", loaderStack, this,
        loaderPriority, &_loaderHandle, loaderCore);
    if (ok != pdPASS) {
        AC_ERROR("loader task create failed");
        _running.store(false, std::memory_order_release);
        vSemaphoreDelete(_muSem);    _muSem    = nullptr;
        vSemaphoreDelete(_loaderSem); _loaderSem = nullptr;
        heap_caps_free(_entries);   _entries   = nullptr;
        return false;
    }

    AC_LOG("begin — budget=%u KB, slots=%d, chunk=%u KB, loader core=%d prio=%d",
           (unsigned)(kAssetCacheBudgetBytes / 1024),
           kAssetCacheMaxEntries,
           (unsigned)(kAssetLoadChunkBytes / 1024),
           loaderCore, loaderPriority);
    return true;
}

void AudioAssetCache::shutdown() {
    if (!_running.load(std::memory_order_acquire)) return;
    _running.store(false, std::memory_order_release);

    // Wake the loader so it can observe _running and exit.  It's
    // counting, so an extra tick is harmless.
    if (_loaderSem) xSemaphoreGive(_loaderSem);

    // Give the loader a beat to exit cleanly.
    if (_loaderHandle) {
        for (int i = 0; i < 100; ++i) {
            if (eTaskGetState(_loaderHandle) == eDeleted) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        _loaderHandle = nullptr;
    }

    // Free every entry's PSRAM data; mark failed so any live handle
    // gracefully aborts.
    if (_entries) {
        lock_();
        for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
            AssetEntry& e = _entries[i];
            if (e.data) {
                heap_caps_free(e.data);
                e.data = nullptr;
            }
            e.failed.store(true, std::memory_order_release);
            e.refCount.store(0, std::memory_order_release);
            e.totalBytes = 0;
            e.loadedBytes.store(0, std::memory_order_release);
            e.path[0] = '\0';
        }
        unlock_();
        heap_caps_free(_entries);
        _entries = nullptr;
    }

    if (_muSem)     { vSemaphoreDelete(_muSem);    _muSem    = nullptr; }
    if (_loaderSem) { vSemaphoreDelete(_loaderSem); _loaderSem = nullptr; }
    _residentBytes.store(0, std::memory_order_release);
}

AssetFormat AudioAssetCache::detectFormat(const char* path) {
    if (!path) return AssetFormat::Unknown;
    const size_t n = strlen(path);
    if (n < 4) return AssetFormat::Unknown;
    const char* ext = path + n - 4;
    auto eq = [&](const char* a, const char* b) {
        for (int i = 0; i < 4; ++i) {
            char ca = a[i]; if (ca >= 'A' && ca <= 'Z') ca += 32;
            char cb = b[i]; if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
        }
        return true;
    };
    if (eq(ext, ".wav")) return AssetFormat::Wav;
    if (eq(ext, ".mp3")) return AssetFormat::Mp3;
    return AssetFormat::Unknown;
}

AssetEntry* AudioAssetCache::findExisting_locked(const char* path) {
    for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
        AssetEntry& e = _entries[i];
        if (e.path[0] && strncmp(e.path, path, kAssetPathMax) == 0) {
            return &e;
        }
    }
    return nullptr;
}

bool AudioAssetCache::evictLRU_locked(uint32_t bytesNeeded) {
    // Score = playCount + recency bonus; lower = colder.  Evict the
    // coldest entry whose refCount==0 AND has no preload owners AND
    // isn't in-flight (data can be safely freed only after the loader
    // is done with it; we conservatively skip in-flight entries).
    while (true) {
        AssetEntry* victim = nullptr;
        uint64_t    victimScore = UINT64_MAX;
        for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
            AssetEntry& e = _entries[i];
            if (!e.path[0])                                     continue;
            if (e.isPinned_locked())                            continue;
            if (e.refCount.load(std::memory_order_acquire) > 0) continue;
            // Skip in-flight (would race with loader writing data[]).
            const uint32_t loaded = e.loadedBytes.load(std::memory_order_acquire);
            if (loaded != e.totalBytes && !e.failed.load(std::memory_order_acquire))
                continue;

            // Score: blend playCount with lastAccessClock so newer +
            // hotter wins.  We're under the table mutex so lastAccess
            // doesn't shift mid-loop.
            const uint64_t pc   = e.playCount.load(std::memory_order_relaxed);
            const uint64_t age  = _lruClock.load(std::memory_order_relaxed) -
                                   e.lastAccessClock;
            const uint64_t score = pc * 1024 + (UINT64_MAX - age) / (1u << 20);
            if (score < victimScore) {
                victimScore = score;
                victim      = &e;
            }
        }
        if (!victim) return false;

        const uint32_t freed = victim->totalBytes;
        AC_LOG("evict %s (%u KB, plays=%u)",
               victim->path,
               (unsigned)(freed / 1024),
               (unsigned)victim->playCount.load(std::memory_order_relaxed));
        if (victim->data) {
            heap_caps_free(victim->data);
            victim->data = nullptr;
        }
        _residentBytes.fetch_sub(freed, std::memory_order_relaxed);
        _evictions.fetch_add(1, std::memory_order_relaxed);

        // Re-init the entry — placement-new the atomics back to zero.
        victim->~AssetEntry();
        new (victim) AssetEntry();

        if (_residentBytes.load(std::memory_order_relaxed) + bytesNeeded
            <= kAssetCacheBudgetBytes)
            return true;
        // else: another round
    }
}

AssetEntry* AudioAssetCache::allocateEntry_locked(const char* path,
                                                  uint32_t totalBytes,
                                                  AssetFormat fmt) {
    // Try to fit; evict LRU until there's room.
    if (totalBytes > kAssetCacheBudgetBytes) {
        AC_WARN("asset too large: %s (%u KB > %u KB budget)",
                path, (unsigned)(totalBytes / 1024),
                (unsigned)(kAssetCacheBudgetBytes / 1024));
        _budgetRejects.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    while (_residentBytes.load(std::memory_order_relaxed) + totalBytes
           > kAssetCacheBudgetBytes) {
        if (!evictLRU_locked(totalBytes)) {
            AC_WARN("budget full, no evictable entries for %s (%u KB)",
                    path, (unsigned)(totalBytes / 1024));
            _budgetRejects.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }

    // Find empty slot.
    AssetEntry* slot = nullptr;
    for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
        if (!_entries[i].path[0]) { slot = &_entries[i]; break; }
    }
    if (!slot) {
        AC_WARN("entry table full (%d slots) — refusing %s",
                kAssetCacheMaxEntries, path);
        _budgetRejects.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    // PSRAM data buffer.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(totalBytes,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        AC_ERROR("PSRAM alloc failed for %s (%u KB)",
                 path, (unsigned)(totalBytes / 1024));
        _budgetRejects.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    strncpy(slot->path, path, kAssetPathMax - 1);
    slot->path[kAssetPathMax - 1] = '\0';
    slot->data       = buf;
    slot->totalBytes = totalBytes;
    slot->loadedBytes.store(0, std::memory_order_release);
    slot->format     = fmt;
    slot->refCount.store(0, std::memory_order_release);
    // owners[] / ownerNames[] left empty by default — setPreloadsForOwner()
    // or the acquire() refcount is the only way to mark an entry pinned.
    slot->failed.store(false, std::memory_order_release);
    slot->playCount.store(0, std::memory_order_release);
    slot->lastAccessClock = _lruClock.fetch_add(1, std::memory_order_relaxed);

    _residentBytes.fetch_add(totalBytes, std::memory_order_relaxed);
    return slot;
}

AssetHandle AudioAssetCache::acquire(const char* path) {
    if (!path || !path[0] || !_running.load(std::memory_order_acquire)) {
        return AssetHandle{};
    }

    lock_();

    // ── Cache hit path ──────────────────────────────────────────────
    AssetEntry* e = findExisting_locked(path);
    if (e) {
        e->refCount.fetch_add(1, std::memory_order_acq_rel);
        e->playCount.fetch_add(1, std::memory_order_relaxed);
        e->lastAccessClock = _lruClock.fetch_add(1, std::memory_order_relaxed);
        _hits.fetch_add(1, std::memory_order_relaxed);
        unlock_();
        return AssetHandle{e};
    }

    // ── Cache miss path — stat file, allocate, enqueue load ─────────
    // Need file size before allocation.  Drop the lock for the stat
    // (filesystem call) then re-take to mutate the table.
    unlock_();

    char fullPath[176];
    snprintf(fullPath, sizeof(fullPath), "/sdcard%s%s",
             (path[0] == '/') ? "" : "/", path);
    struct stat st;
    {
        SdCardModule& sd = SdCardModule::instance();
        sd.lock();
        const int rc = ::stat(fullPath, &st);
        sd.unlock();
        if (rc != 0) {
            AC_WARN("stat failed: %s", path);
            return AssetHandle{};
        }
    }
    const uint32_t totalBytes = (uint32_t)st.st_size;
    if (totalBytes == 0) {
        AC_WARN("empty file: %s", path);
        return AssetHandle{};
    }
    const AssetFormat fmt = detectFormat(path);
    if (fmt == AssetFormat::Unknown) {
        AC_WARN("unknown format (not .wav/.mp3): %s", path);
        return AssetHandle{};
    }

    lock_();
    // Recheck — another caller may have raced us.
    e = findExisting_locked(path);
    if (e) {
        e->refCount.fetch_add(1, std::memory_order_acq_rel);
        e->playCount.fetch_add(1, std::memory_order_relaxed);
        e->lastAccessClock = _lruClock.fetch_add(1, std::memory_order_relaxed);
        _hits.fetch_add(1, std::memory_order_relaxed);
        unlock_();
        return AssetHandle{e};
    }
    e = allocateEntry_locked(path, totalBytes, fmt);
    if (!e) {
        unlock_();
        return AssetHandle{};
    }
    e->refCount.store(1, std::memory_order_release);
    e->playCount.store(1, std::memory_order_relaxed);
    _misses.fetch_add(1, std::memory_order_relaxed);
    unlock_();

    // Wake loader.  Counting semaphore — one give per pending-chunk
    // notion; the loader will self-post additional ticks while the
    // asset has more bytes to read.
    if (_loaderSem) xSemaphoreGive(_loaderSem);

    AC_LOG("acquire %s (%u KB, fmt=%s) — load queued",
           path, (unsigned)(totalBytes / 1024),
           fmt == AssetFormat::Wav ? "wav" : "mp3");
    return AssetHandle{e};
}

// ── AssetEntry owner-list helpers (require cache mutex held) ───────

int AssetEntry::ownerCount_locked() const {
    int n = 0;
    for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
        if (owners[i]) ++n;
    }
    return n;
}

bool AssetEntry::hasOwner_locked(const void* token) const {
    for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
        if (owners[i] == token) return true;
    }
    return false;
}

bool AssetEntry::addOwner_locked(const void* token, const char* name) {
    for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
        if (owners[i] == token) return true;   // already present
    }
    for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
        if (!owners[i]) {
            owners[i]     = token;
            ownerNames[i] = name ? name : "";
            return true;
        }
    }
    return false;   // owner slots full
}

bool AssetEntry::removeOwner_locked(const void* token) {
    for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
        if (owners[i] == token) {
            owners[i]     = nullptr;
            ownerNames[i] = nullptr;
            return true;
        }
    }
    return false;
}


// ── setPreloadsForOwner — the only public preload API ──────────────
//
// One atomic diff across the entire entry table.  For each entry:
//   - if the owner currently has it AND the path is in `paths` → keep
//   - if the owner currently has it AND the path is NOT in `paths` → remove owner
//   - if the owner does NOT have it AND the path IS in `paths` → load+add owner
//   - otherwise → no-op
//
// We do this in two table sweeps (no per-element nested loop):
//   pass 1: walk existing entries; for each one this owner currently
//           holds but isn't in the new list, remove the owner.
//   pass 2: walk the new list; for each path, find-or-allocate the
//           entry and ensure the owner is present.
//
// Both passes happen under `_muSem`, so eviction can't slip between
// the remove + the add.

void AudioAssetCache::setPreloadsForOwner(const void* ownerToken,
                                          const char* ownerName,
                                          const char* const* paths,
                                          int count) {
    if (!ownerToken || !_running.load(std::memory_order_acquire)) return;
    if (count < 0) count = 0;
    if (!ownerName) ownerName = "";

    int added = 0, removed = 0, kept = 0;

    lock_();

    // ── Pass 1: remove this owner from entries no longer in the set ──
    for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
        AssetEntry& e = _entries[i];
        if (!e.path[0])                       continue;
        if (!e.hasOwner_locked(ownerToken))   continue;

        bool stillWanted = false;
        for (int j = 0; j < count; ++j) {
            if (paths[j] && strncmp(paths[j], e.path, kAssetPathMax) == 0) {
                stillWanted = true;
                break;
            }
        }
        if (!stillWanted) {
            e.removeOwner_locked(ownerToken);
            ++removed;
        }
    }

    // ── Pass 2: add this owner to every path in the new set ─────────
    // We have to drop the lock for stat() / allocation since allocateEntry
    // can evict (which scans the same table).  Strategy: build a small
    // "needs new entry" list under the lock, then drop+reacquire to do
    // the SD stat + alloc, then re-take the lock to attach the owner.
    //
    // Most calls will hit the easy path (cache hit, just add owner) and
    // never drop the lock.

    for (int j = 0; j < count; ++j) {
        const char* path = paths[j];
        if (!path || !path[0]) continue;

        AssetEntry* e = findExisting_locked(path);
        if (e) {
            if (e->hasOwner_locked(ownerToken)) {
                ++kept;
            } else {
                if (!e->addOwner_locked(ownerToken, ownerName)) {
                    AC_WARN("preload %s: owner slots full (>%d) — owner '%s' dropped",
                            path, kMaxOwnersPerEntry, ownerName);
                } else {
                    ++added;
                }
            }
            continue;
        }

        // Need to allocate.  Drop the lock to stat the file (filesystem
        // call), then re-take to mutate the table.
        unlock_();

        char fullPath[176];
        snprintf(fullPath, sizeof(fullPath), "/sdcard%s%s",
                 (path[0] == '/') ? "" : "/", path);
        struct stat st;
        bool statOk;
        {
            SdCardModule& sd = SdCardModule::instance();
            sd.lock();
            statOk = (::stat(fullPath, &st) == 0);
            sd.unlock();
        }
        if (!statOk) {
            AC_WARN("preload stat failed: %s", path);
            lock_();
            continue;
        }
        const uint32_t totalBytes = (uint32_t)st.st_size;
        if (totalBytes == 0) {
            AC_WARN("preload empty file: %s", path);
            lock_();
            continue;
        }
        const AssetFormat fmt = detectFormat(path);
        if (fmt == AssetFormat::Unknown) {
            AC_WARN("preload unknown format (not .wav/.mp3): %s", path);
            lock_();
            continue;
        }

        lock_();

        // Recheck — another caller (or a recursive setPreloadsForOwner)
        // may have raced us during the unlocked window.
        e = findExisting_locked(path);
        if (!e) {
            e = allocateEntry_locked(path, totalBytes, fmt);
            if (!e) continue;   // budget reject already logged
            _misses.fetch_add(1, std::memory_order_relaxed);
        } else {
            _hits.fetch_add(1, std::memory_order_relaxed);
        }
        if (!e->hasOwner_locked(ownerToken)) {
            if (!e->addOwner_locked(ownerToken, ownerName)) {
                AC_WARN("preload %s: owner slots full — owner '%s' dropped",
                        path, ownerName);
            } else {
                ++added;
            }
        } else {
            ++kept;
        }
        e->playCount.fetch_add(1, std::memory_order_relaxed);
        e->lastAccessClock = _lruClock.fetch_add(1, std::memory_order_relaxed);

        // Wake loader.  Counting semaphore — one give per pending chunk;
        // the loader self-posts more ticks while there are bytes to read.
        if (_loaderSem) xSemaphoreGive(_loaderSem);
    }

    unlock_();

    // One summary line per call (the per-path lines are emitted by the
    // loader as it completes).  Skip when literally nothing changed.
    if (added || removed) {
        AC_LOG("owner '%s' — +%d -%d =%d  (resident now %u KB)",
               ownerName, added, removed, kept,
               (unsigned)(_residentBytes.load(std::memory_order_relaxed) / 1024));
    } else if (count > 0) {
        AC_LOG("owner '%s' — unchanged (%d paths, all cached)",
               ownerName, kept);
    }
}


// ─── Loader task ────────────────────────────────────────────────────

void AudioAssetCache::loaderTaskFunc(void* arg) {
    auto* self = static_cast<AudioAssetCache*>(arg);
    AC_LOG("loader task started — core=%d prio=%d",
           xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    // Per-task local FILE handle cache (LRU=1 — we only ever load one
    // file at a time, but keep the handle alive across chunks of the
    // same file to skip fopen/fclose per chunk).
    FILE*    cachedFp        = nullptr;
    char     cachedPath[kAssetPathMax] = {};

    while (self->_running.load(std::memory_order_acquire)) {
        // Wait for work.  100 ms timeout so we periodically check
        // _running for graceful shutdown.
        if (xSemaphoreTake(self->_loaderSem, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!self->_running.load(std::memory_order_acquire)) break;

        // Pick the entry that is furthest behind proportionally
        // among still-loading, non-failed entries.
        AssetEntry* target = nullptr;
        uint64_t    worstScore = UINT64_MAX;  // lower = more behind = higher priority
        self->lock_();
        for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
            AssetEntry& e = self->_entries[i];
            if (!e.path[0])                                       continue;
            if (e.failed.load(std::memory_order_acquire))         continue;
            const uint32_t loaded = e.loadedBytes.load(std::memory_order_acquire);
            if (loaded == e.totalBytes)                           continue;

            // Score: ratio of bytes loaded.  Range [0..1<<16].
            const uint64_t score = (uint64_t)loaded * (1u << 16) / e.totalBytes;
            if (score < worstScore) {
                worstScore = score;
                target     = &e;
            }
        }
        self->unlock_();

        if (!target) continue;   // nothing to do (spurious wake)

        // ── Load one chunk ──────────────────────────────────────────
        const uint32_t loaded = target->loadedBytes.load(std::memory_order_acquire);
        const uint32_t remain = target->totalBytes - loaded;
        const uint32_t take   = (remain < kAssetLoadChunkBytes)
                                ? remain : kAssetLoadChunkBytes;

        // Serialize against every other SD consumer (WavStreamSource
        // fallback, upload protocol, config save, …) via the same
        // SdCardModule mutex they all use.  Critical section is small
        // — one fopen-if-needed + one fseek + one fread per chunk.
        SdCardModule& sd = SdCardModule::instance();
        sd.lock();

        // Re-open if file changed (or not yet opened).
        if (!cachedFp || strncmp(cachedPath, target->path, kAssetPathMax) != 0) {
            if (cachedFp) { fclose(cachedFp); cachedFp = nullptr; }
            char fullPath[176];
            snprintf(fullPath, sizeof(fullPath), "/sdcard%s%s",
                     (target->path[0] == '/') ? "" : "/", target->path);
            cachedFp = fopen(fullPath, "rb");
            if (!cachedFp) {
                sd.unlock();
                AC_ERROR("fopen failed: %s", target->path);
                target->failed.store(true, std::memory_order_release);
                self->_loadFailures.fetch_add(1, std::memory_order_relaxed);
                cachedPath[0] = '\0';
                continue;
            }
            strncpy(cachedPath, target->path, kAssetPathMax - 1);
            cachedPath[kAssetPathMax - 1] = '\0';
        }

        if (fseek(cachedFp, (long)loaded, SEEK_SET) != 0) {
            sd.unlock();
            AC_ERROR("fseek failed @ %u: %s", (unsigned)loaded, target->path);
            target->failed.store(true, std::memory_order_release);
            self->_loadFailures.fetch_add(1, std::memory_order_relaxed);
            fclose(cachedFp); cachedFp = nullptr; cachedPath[0] = '\0';
            continue;
        }

        const uint32_t t0 = SFX_MILLIS();
        // fread directly into PSRAM destination.  VFS-FAT bounces
        // through its own DMA-cap SRAM internally; we accept the
        // ~1 MB/s throughput penalty in exchange for not snatching
        // DMA-cap pool from I²S DMA descriptors at boot time.
        const size_t got = fread(target->data + loaded, 1, take, cachedFp);
        sd.unlock();
        if (got == 0) {
            AC_ERROR("fread returned 0 @ %u (want %u): %s",
                     (unsigned)loaded, (unsigned)take, target->path);
            target->failed.store(true, std::memory_order_release);
            self->_loadFailures.fetch_add(1, std::memory_order_relaxed);
            sd.lock(); fclose(cachedFp); sd.unlock();
            cachedFp = nullptr; cachedPath[0] = '\0';
            continue;
        }

        // Publish atomically so sources see the new bytes.
        target->loadedBytes.fetch_add((uint32_t)got, std::memory_order_release);

        const uint32_t dt = SFX_MILLIS() - t0;
        self->_totalLoadMs.fetch_add(dt, std::memory_order_relaxed);
        uint32_t prevMax = self->_maxLoadMs.load(std::memory_order_relaxed);
        while (dt > prevMax &&
               !self->_maxLoadMs.compare_exchange_weak(prevMax, dt,
                                                       std::memory_order_relaxed)) {}

        const uint32_t nowLoaded = loaded + (uint32_t)got;
        const bool     done      = nowLoaded == target->totalBytes;

        if (self->_verbose.load(std::memory_order_relaxed)) {
            AC_LOG("chunk %s @%u +%u%s in %ums",
                   target->path,
                   (unsigned)loaded, (unsigned)got,
                   done ? " DONE" : "", (unsigned)dt);
        }

        if (done) {
            sd.lock(); fclose(cachedFp); sd.unlock();
            cachedFp = nullptr; cachedPath[0] = '\0';
            self->_loadsCompleted.fetch_add(1, std::memory_order_relaxed);
            AC_LOG("loaded %s (%u KB)",
                   target->path, (unsigned)(target->totalBytes / 1024));
        } else {
            // Still more to read — self-post so we keep working on
            // this (or another) in-flight entry on the next iteration.
            xSemaphoreGive(self->_loaderSem);
        }
    }

    if (cachedFp) {
        SdCardModule& sd = SdCardModule::instance();
        sd.lock(); fclose(cachedFp); sd.unlock();
    }
    AC_LOG("loader task exiting");
    vTaskDelete(nullptr);
}


// ─── Stats / diag ───────────────────────────────────────────────────

AudioAssetCache::Stats AudioAssetCache::stats() const {
    Stats s{};
    s.hits          = _hits.load(std::memory_order_relaxed);
    s.misses        = _misses.load(std::memory_order_relaxed);
    s.evictions     = _evictions.load(std::memory_order_relaxed);
    s.budgetRejects = _budgetRejects.load(std::memory_order_relaxed);
    s.loadFailures  = _loadFailures.load(std::memory_order_relaxed);
    s.loadsCompleted = _loadsCompleted.load(std::memory_order_relaxed);
    s.maxLoadMs     = _maxLoadMs.load(std::memory_order_relaxed);
    const uint32_t lc = s.loadsCompleted;
    s.avgLoadMs     = lc ? (uint32_t)(_totalLoadMs.load(std::memory_order_relaxed) / lc) : 0;

    if (!_entries) return s;
    for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
        const AssetEntry& e = _entries[i];
        if (!e.path[0]) continue;
        ++s.entries;
        s.residentBytes += e.loadedBytes.load(std::memory_order_relaxed);
        if (e.isPinned_locked())                          ++s.pinnedEntries;
        if (e.failed.load(std::memory_order_relaxed))     ++s.failedEntries;
        if (e.loadedBytes.load(std::memory_order_relaxed) != e.totalBytes &&
            !e.failed.load(std::memory_order_relaxed)) {
            ++s.inFlightEntries;
        }
    }
    return s;
}

void AudioAssetCache::resetStats() {
    _hits.store(0, std::memory_order_relaxed);
    _misses.store(0, std::memory_order_relaxed);
    _evictions.store(0, std::memory_order_relaxed);
    _budgetRejects.store(0, std::memory_order_relaxed);
    _loadFailures.store(0, std::memory_order_relaxed);
    _loadsCompleted.store(0, std::memory_order_relaxed);
    _totalLoadMs.store(0, std::memory_order_relaxed);
    _maxLoadMs.store(0, std::memory_order_relaxed);
}

void AudioAssetCache::logSnapshot() const {
    const Stats s = stats();
    AC_LOG("entries=%u/%d  resident=%u/%u KB  inflight=%u  pinned=%u  failed=%u  hits=%u  misses=%u  evict=%u",
           s.entries, kAssetCacheMaxEntries,
           (unsigned)(s.residentBytes / 1024),
           (unsigned)(kAssetCacheBudgetBytes / 1024),
           s.inFlightEntries, s.pinnedEntries, s.failedEntries,
           s.hits, s.misses, s.evictions);
}


// ─── Preload diagnostics ────────────────────────────────────────────

static AudioAssetCache::PreloadStatus
classifyEntry(const AssetEntry& e) {
    if (e.failed.load(std::memory_order_acquire)) return AudioAssetCache::PreloadStatus::Failed;
    if (e.loadedBytes.load(std::memory_order_acquire) == e.totalBytes)
        return AudioAssetCache::PreloadStatus::Ready;
    return AudioAssetCache::PreloadStatus::Loading;
}

static void fillInfo(const AssetEntry& e, AudioAssetCache::PreloadInfo& out) {
    strncpy(out.path, e.path, kAssetPathMax);
    out.path[kAssetPathMax - 1] = '\0';
    out.totalBytes  = e.totalBytes;
    out.loadedBytes = e.loadedBytes.load(std::memory_order_acquire);
    out.status      = classifyEntry(e);
    out.format      = e.format;
    out.ownerCount  = e.ownerCount_locked();
    for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
        out.ownerNames[i] = e.ownerNames[i];   // may be null
    }
}

bool AudioAssetCache::getPreloadInfo(const char* path, PreloadInfo& out) const {
    if (!path || !path[0] || !_entries) return false;
    // const_cast to take the non-const mutex.  The lookup is read-only.
    auto* self = const_cast<AudioAssetCache*>(this);
    self->lock_();
    bool found = false;
    for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
        const AssetEntry& e = _entries[i];
        if (!e.path[0]) continue;
        if (strncmp(e.path, path, kAssetPathMax) == 0) {
            fillInfo(e, out);
            found = true;
            break;
        }
    }
    self->unlock_();
    return found;
}

void AudioAssetCache::enumeratePreloads(PreloadVisitor cb, void* userData) const {
    if (!cb || !_entries) return;
    auto* self = const_cast<AudioAssetCache*>(this);
    self->lock_();
    for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
        const AssetEntry& e = _entries[i];
        if (!e.path[0]) continue;
        PreloadInfo info;
        fillInfo(e, info);
        if (!cb(info, userData)) break;
    }
    self->unlock_();
}

namespace {

const char* statusGlyph(AudioAssetCache::PreloadStatus s) {
    using S = AudioAssetCache::PreloadStatus;
    switch (s) {
        case S::Loading: return "loading";
        case S::Ready:   return "ready  ";
        case S::Failed:  return "FAILED ";
        default:         return "?      ";
    }
}

const char* formatGlyph(AssetFormat f) {
    switch (f) {
        case AssetFormat::Wav: return "wav";
        case AssetFormat::Mp3: return "mp3";
        default:               return "?  ";
    }
}

}   // anon

void AudioAssetCache::logPreloadStatus() const {
    const Stats s = stats();
    if (s.entries == 0) return;   // no noise on empty boards

    uint32_t ready = 0, loading = 0, failed = 0;
    uint32_t readyBytes = 0, loadingBytes = 0;
    if (_entries) {
        auto* self = const_cast<AudioAssetCache*>(this);
        self->lock_();
        for (int i = 0; i < kAssetCacheMaxEntries; ++i) {
            const AssetEntry& e = _entries[i];
            if (!e.path[0]) continue;
            switch (classifyEntry(e)) {
                case PreloadStatus::Ready:
                    ++ready;
                    readyBytes += e.totalBytes;
                    break;
                case PreloadStatus::Loading:
                    ++loading;
                    loadingBytes += e.totalBytes;
                    break;
                case PreloadStatus::Failed:
                    ++failed;
                    break;
                default: break;
            }
        }
        self->unlock_();
    }

    AC_LOG("preload status — %u paths, %u ready (%u KB), %u loading (%u KB), %u failed",
           (unsigned)s.entries,
           (unsigned)ready, (unsigned)(readyBytes / 1024),
           (unsigned)loading, (unsigned)(loadingBytes / 1024),
           (unsigned)failed);

    // Per-path detail.
    enumeratePreloads([](const PreloadInfo& info, void*) -> bool {
        // Compose owner list, e.g. "[alerts]" or "[alerts,enginefx]".
        char owners[64];
        int  off = 0;
        owners[0] = '[';
        ++off;
        bool first = true;
        for (int i = 0; i < kMaxOwnersPerEntry; ++i) {
            if (!info.ownerNames[i]) continue;
            int n = snprintf(owners + off, sizeof(owners) - off,
                             "%s%s", first ? "" : ",", info.ownerNames[i]);
            if (n < 0 || off + n + 1 >= (int)sizeof(owners)) break;
            off += n;
            first = false;
        }
        if (first) {   // no owners (handle was released, kept by LRU)
            const char* lru = "lru";
            int n = snprintf(owners + off, sizeof(owners) - off, "%s", lru);
            if (n > 0) off += n;
        }
        if (off + 2 <= (int)sizeof(owners)) {
            owners[off++] = ']';
            owners[off  ] = '\0';
        } else {
            owners[sizeof(owners) - 1] = '\0';
        }

        const uint32_t pct = info.totalBytes
            ? (info.loadedBytes * 100u / info.totalBytes) : 0;
        AC_LOG("  %s %s  %s  %u/%u KB (%u%%)  %s",
               statusGlyph(info.status),
               formatGlyph(info.format),
               info.path,
               (unsigned)(info.loadedBytes / 1024),
               (unsigned)(info.totalBytes / 1024),
               (unsigned)pct,
               owners);
        return true;
    }, nullptr);
}

#endif  // SFX_PLATFORM_ESP32
