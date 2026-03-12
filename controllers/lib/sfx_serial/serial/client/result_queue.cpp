/*
 * Result Queue - Implementation
 *
 * Tag-correlated command/response queue for serial bus clients.
 * See serial_result_queue.h for design and usage documentation.
 */

#include "result_queue.h"
#include "platform/sfx_platform.h"

// ============================================================================
// Tag Management
// ============================================================================

uint8_t ResultQueue::nextTag() {
    uint8_t tag = _nextTag;
    _nextTag = (_nextTag % 255) + 1;
    return tag;
}

// ============================================================================
// Response Resolution
// ============================================================================

bool ResultQueue::resolve(uint8_t tag, const CommandResult& result) {
    if (tag == 0) return false;  // Async packets don't resolve tags

    // 1. Blocking wait match
    if (_waitingTag.load(std::memory_order_relaxed) == tag) {
        _waitResult = result;
        _waitResolved.store(true, std::memory_order_release);
        return true;
    }

    // 2. Async callback match
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (_asyncCallbacks[i].active && _asyncCallbacks[i].tag == tag) {
            auto cb = _asyncCallbacks[i].callback;  // Copy before clearing
            _asyncCallbacks[i].active = false;
            if (cb) cb(result);
            return true;
        }
    }

    // 3. Stash for later
    stash(tag, result);
    return false;
}

// ============================================================================
// Blocking Wait
// ============================================================================

CommandResult ResultQueue::waitForTag(uint8_t tag, ProcessFunc processFunc) {
    // Check stash first
    CommandResult stashed;
    if (unstash(tag, stashed)) {
        return stashed;
    }

    _waitingTag.store(tag, std::memory_order_relaxed);
    _waitResolved.store(false, std::memory_order_relaxed);

    unsigned long startMs = millis();

    while (!_waitResolved.load(std::memory_order_acquire)) {
        if (processFunc) processFunc();

        if (millis() - startMs > _commandTimeout_ms) {
            _waitingTag.store(0, std::memory_order_relaxed);
            return CommandResult::Timeout();
        }

        SFX_DELAY_MS(1);
    }

    _waitingTag.store(0, std::memory_order_relaxed);
    return _waitResult;
}

// ============================================================================
// Async One-Shot Callbacks
// ============================================================================

bool ResultQueue::onTagResponse(uint8_t tag, TagCallback cb) {
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (!_asyncCallbacks[i].active) {
            _asyncCallbacks[i].tag = tag;
            _asyncCallbacks[i].callback = cb;
            _asyncCallbacks[i].active = true;
            return true;
        }
    }
    return false;  // No slots available
}

void ResultQueue::cancelTagResponse(uint8_t tag) {
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (_asyncCallbacks[i].active && _asyncCallbacks[i].tag == tag) {
            _asyncCallbacks[i].active = false;
        }
    }
}

// ============================================================================
// State
// ============================================================================

void ResultQueue::reset() {
    _waitingTag.store(0, std::memory_order_relaxed);
    _waitResolved.store(false, std::memory_order_relaxed);
    for (size_t i = 0; i < MAX_PENDING; i++) {
        _asyncCallbacks[i].active = false;
        _stash[i].valid = false;
    }
}

// ============================================================================
// Private Helpers
// ============================================================================

void ResultQueue::stash(uint8_t tag, const CommandResult& result) {
    // Overwrite existing entry for same tag, or use first free slot
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (_stash[i].valid && _stash[i].tag == tag) {
            _stash[i].result = result;
            return;
        }
    }
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (!_stash[i].valid) {
            _stash[i].tag = tag;
            _stash[i].result = result;
            _stash[i].valid = true;
            return;
        }
    }
    // Stash full — drop oldest (index 0), shift down
    for (size_t i = 0; i < MAX_PENDING - 1; i++) {
        _stash[i] = _stash[i + 1];
    }
    _stash[MAX_PENDING - 1].tag = tag;
    _stash[MAX_PENDING - 1].result = result;
    _stash[MAX_PENDING - 1].valid = true;
}

bool ResultQueue::unstash(uint8_t tag, CommandResult& out) {
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (_stash[i].valid && _stash[i].tag == tag) {
            out = _stash[i].result;
            _stash[i].valid = false;
            return true;
        }
    }
    return false;
}
