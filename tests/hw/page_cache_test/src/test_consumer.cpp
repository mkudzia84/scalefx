/*
 * test_consumer.cpp — implementation.
 *
 * Drain algorithm per tick:
 *   1. Compute `want` bytes for this tick.
 *   2. Ensure _curPage covers the current cursor:
 *      - If _curPage exists and cursor is inside it → use it.
 *      - Else slide _nextPage into _curPage if it matches.
 *      - Else fresh acquire().
 *   3. If _curPage is not Ready yet → it's an underrun for this tick.
 *      Record + return.
 *   4. memcpy-style "consume" from the page (we don't actually copy
 *      out — for the test we just advance the cursor and validate the
 *      first / last byte of the page-data view to ensure we're seeing
 *      real bytes from the file).
 *   5. If we crossed 50 % of the page, prefetch the next page (if
 *      there is one).
 *   6. Repeat until `want` is drained OR we hit a page boundary
 *      (then yield to next tick — keep the per-tick I/O bounded).
 */

#include "test_consumer.h"

bool TestConsumer::start(int channelId, const char* path, uint32_t fileSize,
                          bool loop, uint32_t bytesPerSec,
                          int core, int priority, int stack) {
    if (_running.load(std::memory_order_acquire)) return false;
    if (!path || !fileSize) return false;
    if (!PageCache::instance().begin()) {
        // begin() is idempotent; this just guarantees it's up.
    }
    _channelId   = channelId;
    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _fileSize    = fileSize;
    _loop        = loop;
    _bytesPerSec = bytesPerSec;

    _cursor.store(0, std::memory_order_relaxed);
    _bytesConsumed.store(0, std::memory_order_relaxed);
    _loopCount.store(0, std::memory_order_relaxed);
    _underruns.store(0, std::memory_order_relaxed);
    _windowBytes.store(0, std::memory_order_relaxed);
    _maxStallMs.store(0, std::memory_order_relaxed);
    _done.store(false, std::memory_order_relaxed);
    _running.store(true, std::memory_order_release);

    BaseType_t r = xTaskCreatePinnedToCore(&TestConsumer::taskFunc,
                                            "Consumer", stack, this,
                                            priority, &_taskHandle, core);
    if (r != pdPASS) {
        _running.store(false, std::memory_order_release);
        Serial.printf("[ch%d] FATAL: task create failed\n", channelId);
        return false;
    }
    Serial.printf("[ch%d] consumer started: path=%s size=%u %s @ %u KB/s\n",
                  channelId, path, (unsigned)fileSize,
                  loop ? "(LOOP)" : "(once)",
                  (unsigned)(bytesPerSec / 1024));
    return true;
}

void TestConsumer::stop() {
    if (!_running.exchange(false, std::memory_order_acq_rel)) return;
    for (int i = 0; i < 50 && _taskHandle; ++i) vTaskDelay(pdMS_TO_TICKS(5));
    if (_taskHandle) { vTaskDelete(_taskHandle); _taskHandle = nullptr; }
    _curPage.reset();
    _nextPage.reset();
}

TestConsumer::Stats TestConsumer::snapshot() {
    Stats s{};
    s.bytesConsumed = _bytesConsumed.load(std::memory_order_acquire);
    s.loopCount     = _loopCount.load(std::memory_order_acquire);
    s.underruns     = _underruns.load(std::memory_order_acquire);
    s.cursor        = _cursor.load(std::memory_order_acquire);
    s.lastTickMs    = _lastTickMs.load(std::memory_order_acquire);
    s.maxStallMs    = _maxStallMs.load(std::memory_order_acquire);
    s.done          = _done.load(std::memory_order_acquire);
    s.windowBytes   = _windowBytes.exchange(0, std::memory_order_acq_rel);
    return s;
}


// ── Task ──────────────────────────────────────────────────────────────

void TestConsumer::taskFunc(void* arg) {
    static_cast<TestConsumer*>(arg)->run();
    vTaskDelete(nullptr);
}

void TestConsumer::run() {
    const TickType_t period = pdMS_TO_TICKS(kTickMs);
    TickType_t       lastWake = xTaskGetTickCount();
    const uint32_t   bytesPerTick = (_bytesPerSec * kTickMs) / 1000;

    // Pre-acquire the first page so the reader's initial latency is
    // absorbed before the first tick.
    _curPageBase = 0;
    _curPage     = PageCache::instance().acquire(_path, 0, _fileSize);

    while (_running.load(std::memory_order_acquire) &&
           !_done.load(std::memory_order_acquire)) {
        vTaskDelayUntil(&lastWake, period);
        const uint32_t tickStart = millis();
        _lastTickMs.store(tickStart, std::memory_order_release);

        uint32_t want = bytesPerTick;
        while (want > 0) {
            uint32_t cur = _cursor.load(std::memory_order_acquire);

            // Loop wrap / EOF
            if (cur >= _fileSize) {
                if (_loop) {
                    cur = 0;
                    _cursor.store(0, std::memory_order_release);
                    _curPage.reset();
                    _nextPage.reset();
                    _curPageBase = 0;
                    _loopCount.fetch_add(1, std::memory_order_acq_rel);
                } else {
                    _done.store(true, std::memory_order_release);
                    break;
                }
            }

            // Ensure _curPage covers `cur`
            if (!_curPage.isValid() ||
                cur < _curPageBase ||
                cur >= _curPageBase + _curPage.size()) {
                // Try sliding nextPage in
                if (_nextPage.isValid() && _nextPageBase == cur) {
                    _curPage     = std::move(_nextPage);
                    _curPageBase = _nextPageBase;
                    _nextPage    = PageRef{};
                } else {
                    _curPage.reset();
                    _curPageBase = (cur / kMaxPageBytes) * kMaxPageBytes;
                    const uint32_t avail = _fileSize - _curPageBase;
                    _curPage = PageCache::instance().acquire(_path,
                                                              _curPageBase,
                                                              avail);
                }
            }

            if (!_curPage.isValid() || !_curPage.isReady()) {
                // Underrun: page not ready (still Loading, or hard failure).
                if (_curPage.isFailed()) {
                    Serial.printf("[ch%d] FATAL page load failed at off=%u — aborting\n",
                                  _channelId, (unsigned)_curPageBase);
                    _done.store(true, std::memory_order_release);
                    break;
                }
                _underruns.fetch_add(1, std::memory_order_acq_rel);
                const uint32_t stall = millis() - tickStart;
                uint32_t prev = _maxStallMs.load(std::memory_order_relaxed);
                while (stall > prev &&
                       !_maxStallMs.compare_exchange_weak(prev, stall,
                                                          std::memory_order_relaxed)) {}
                // Don't busy-wait — abandon this tick.
                break;
            }

            // Consume from the page.
            const uint32_t byteInPage = cur - _curPageBase;
            const uint32_t leftInPage = _curPage.size() - byteInPage;
            uint32_t       take       = (leftInPage < want) ? leftInPage : want;
            if (take > _fileSize - cur) take = _fileSize - cur;

            // Light-touch validation: read the first + last byte of the
            // slice so the compiler doesn't elide the page access.  In a
            // real consumer this would be a memcpy into the float decode
            // buffer.
            volatile uint8_t first = _curPage.data()[byteInPage];
            volatile uint8_t last  = _curPage.data()[byteInPage + take - 1];
            (void)first; (void)last;

            _cursor.store(cur + take, std::memory_order_release);
            _bytesConsumed.fetch_add(take, std::memory_order_acq_rel);
            _windowBytes.fetch_add(take, std::memory_order_acq_rel);
            want -= take;

            // Prefetch when we've drained > 50 % of the current page.
            const uint32_t consumedInPage = (cur + take) - _curPageBase;
            if (consumedInPage > (_curPage.size() / 2)) {
                const uint32_t nextBase = _curPageBase + _curPage.size();
                if (nextBase < _fileSize &&
                    (!_nextPage.isValid() || _nextPageBase != nextBase)) {
                    _nextPageBase = nextBase;
                    _nextPage = PageCache::instance().acquire(_path, nextBase,
                                                              _fileSize - nextBase);
                }
            }
        }
    }

    Serial.printf("[ch%d] consumer exit: consumed=%u/%u loops=%u underruns=%u\n",
                  _channelId,
                  (unsigned)_bytesConsumed.load(std::memory_order_acquire),
                  (unsigned)_fileSize,
                  (unsigned)_loopCount.load(std::memory_order_acquire),
                  (unsigned)_underruns.load(std::memory_order_acquire));
    _curPage.reset();
    _nextPage.reset();
    _taskHandle = nullptr;
}
