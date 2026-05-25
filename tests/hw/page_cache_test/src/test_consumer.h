/*
 * test_consumer.h — paced cache consumer that simulates the audio mixer.
 *
 * One instance = one virtual playback channel.  Holds a path + size
 * cursor; on each tick (every `kTickMs` ms) it drains as many bytes as
 * the per-tick budget allows from the page cache, advancing the cursor.
 *
 * Per-tick budget
 * ───────────────
 *   = `kBytesPerSec` * `kTickMs` / 1000
 *   At 192 KB/s (48 kHz × 2 ch × 2 B) and a 10 ms tick that's 1920 B.
 *
 *   At each tick we issue at most ONE page-cache read.  If the current
 *   page exhausts mid-tick we stop early; the next tick swaps to the
 *   next page.  Underruns (next page not yet Ready) are counted but
 *   we don't busy-wait — we report and move on.
 *
 * Prefetch
 * ────────
 *   Once the current page's drain crosses 50 %, we acquire the NEXT
 *   page so the reader task starts loading it in the background.
 *
 * Looping
 * ───────
 *   When `loop=true` and the cursor reaches end-of-file, we wrap back
 *   to byte 0 and bump `loopCount`.  When `loop=false` and EOF is
 *   reached, the consumer marks itself done and idles.
 */

#ifndef TEST_CONSUMER_H
#define TEST_CONSUMER_H

#include "page_cache.h"

#include <Arduino.h>
#include <atomic>
#include <cstdint>

class TestConsumer {
public:
    /// `bytesPerSec` defaults to 192 000 (48 kHz stereo16) — the rate
    /// the real audio mixer reads at.  Tick is fixed at 10 ms.
    bool start(int channelId,
               const char* path,
               uint32_t fileSize,
               bool loop,
               uint32_t bytesPerSec = 192000,
               int core             = 1,
               int priority         = configMAX_PRIORITIES - 2,
               int stack            = 8192);

    void stop();

    // ── Stats (read from any task) ───────────────────────────────────
    struct Stats {
        uint32_t bytesConsumed;
        uint32_t loopCount;
        uint32_t underruns;
        uint32_t cursor;
        uint32_t lastTickMs;     ///< wall time of last tick
        uint32_t windowBytes;    ///< bytes consumed since last snapshot
        uint32_t maxStallMs;     ///< worst single-tick stall (page wait)
        bool     done;
    };
    Stats snapshot();   ///< thread-safe — atomically reads + resets `windowBytes`

    int  channelId()  const { return _channelId; }
    const char* path() const { return _path; }

private:
    static void taskFunc(void* arg);
    void        run();
    bool        advanceToPageFor(uint32_t cursor);

    // Configuration (immutable after start())
    int       _channelId = -1;
    char      _path[96]  = {};
    uint32_t  _fileSize  = 0;
    bool      _loop      = false;
    uint32_t  _bytesPerSec = 0;

    // Page state
    PageRef   _curPage;
    PageRef   _nextPage;
    uint32_t  _curPageBase = 0;       ///< absolute byte offset of _curPage's first byte
    uint32_t  _nextPageBase = 0;

    // Stats (cross-core)
    std::atomic<uint32_t> _cursor       {0};
    std::atomic<uint32_t> _bytesConsumed{0};
    std::atomic<uint32_t> _loopCount    {0};
    std::atomic<uint32_t> _underruns    {0};
    std::atomic<uint32_t> _lastTickMs   {0};
    std::atomic<uint32_t> _windowBytes  {0};
    std::atomic<uint32_t> _maxStallMs   {0};
    std::atomic<bool>     _running      {false};
    std::atomic<bool>     _done         {false};

    TaskHandle_t _taskHandle = nullptr;

    static constexpr int kTickMs = 10;
};

#endif  // TEST_CONSUMER_H
