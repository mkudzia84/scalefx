/*
 * Result Queue - Tag-Correlated Command/Response for Serial Bus Clients
 *
 * Provides tag auto-assignment, blocking wait-for-tag, and async one-shot
 * callbacks for client controllers that send commands and expect ACK/NACK
 * responses with matching correlation tags.
 *
 * Design:
 *   - Each command gets a unique tag (1-255, wrapping)
 *   - Blocking wait spins calling a caller-supplied process function until
 *     the matching tag arrives or timeout expires
 *   - While waiting, responses with other tags are stashed for later
 *   - Async mode: register a one-shot callback for a tag; it fires when
 *     that tag's response arrives during any process() call
 *
 * Integration:
 *   - Embed a ResultQueue in each BusClient-derived client
 *   - In handlePacket() ACK/NACK cases, call queue.resolve(tag, result)
 *   - Replace sendPacketBlocking()/waitForAckNack() with sendCommand()
 *
 * Usage:
 *   ResultQueue _resultQueue;
 *
 *   // Blocking:
 *   CommandResult sendCommand(uint8_t type, const uint8_t* payload, size_t len) {
 *       uint8_t tag = _resultQueue.nextTag();
 *       sendPacket(type, payload, len, tag);
 *       return _resultQueue.waitForTag(tag, [this]{ SerialBus::process(); });
 *   }
 *
 *   // Async (one-shot):
 *   void sendCommandAsync(uint8_t type, const uint8_t* payload, size_t len,
 *                         ResultQueue::TagCallback cb) {
 *       uint8_t tag = _resultQueue.nextTag();
 *       sendPacket(type, payload, len, tag);
 *       _resultQueue.onTagResponse(tag, cb);
 *   }
 *
 *   // In handlePacket():
 *   case CorePacket::ACK:
 *       _resultQueue.resolve(tag, CommandResult::Ack());
 *       break;
 *   case CorePacket::NACK:
 *       _resultQueue.resolve(tag, CommandResult::Nack(errorCode, reason));
 *       break;
 */

#ifndef SERIAL_RESULT_QUEUE_H
#define SERIAL_RESULT_QUEUE_H

#include <Arduino.h>
#include <functional>
#include "serial_core.h"

// ============================================================================
// ResultQueue
// ============================================================================

/**
 * @brief Tag-correlated command/response queue for serial bus clients
 *
 * Composable component: embed in any BusClient-derived client to get
 * tag-matched blocking waits and async one-shot callbacks.
 */
class ResultQueue {
public:
    /// Maximum number of concurrent async callbacks / stashed responses
    static constexpr size_t MAX_PENDING = 64;

    /// Callback type for async one-shot tag responses
    using TagCallback = std::function<void(const CommandResult&)>;

    /// Process function type (caller provides to pump serial during blocking wait)
    using ProcessFunc = std::function<void()>;

    ResultQueue() = default;

    // ========================================================================
    // Tag Management
    // ========================================================================

    /**
     * @brief Get next unique correlation tag (1-255, wrapping)
     * @return Tag value to use for next command
     */
    uint8_t nextTag();

    // ========================================================================
    // Configuration
    // ========================================================================

    void setCommandTimeout_ms(unsigned long ms) { _commandTimeout_ms = ms; }  // ms
    unsigned long commandTimeout_ms() const { return _commandTimeout_ms; }     // ms

    // ========================================================================
    // Response Resolution (call from handlePacket)
    // ========================================================================

    /**
     * @brief Resolve a tag with a command result
     *
     * Called from the client's handlePacket() when an ACK, NACK, STATUS,
     * or other response-type packet arrives.
     *
     * Resolution priority:
     *   1. If a blocking wait is pending for this tag → unblock it
     *   2. If an async callback is registered for this tag → fire and clear
     *   3. Otherwise → stash for later retrieval
     *
     * @param tag   Response tag (0 = async, ignored)
     * @param result  The command result (Ack, Nack, etc.)
     * @return true if the tag was consumed (matched a wait or callback)
     */
    bool resolve(uint8_t tag, const CommandResult& result);

    // ========================================================================
    // Blocking Wait
    // ========================================================================

    /**
     * @brief Block until a response with the given tag arrives or timeout
     *
     * While waiting, calls processFunc repeatedly to pump serial data.
     * If a response with a different tag arrives, it is stashed or dispatched
     * to async callbacks via resolve().
     *
     * @param tag         Tag to wait for (1-255)
     * @param processFunc Function to call to pump serial (e.g., SerialBus::process)
     * @return CommandResult for the matching response, or Timeout
     */
    CommandResult waitForTag(uint8_t tag, ProcessFunc processFunc);

    // ========================================================================
    // Async One-Shot Callbacks
    // ========================================================================

    /**
     * @brief Register a one-shot callback for a tag
     *
     * When a response with this tag arrives (via resolve()), the callback
     * fires once and is automatically cleared.
     *
     * @param tag  Tag to listen for
     * @param cb   Callback (receives CommandResult)
     * @return true if registered, false if no slots available
     */
    bool onTagResponse(uint8_t tag, TagCallback cb);

    /**
     * @brief Cancel an async callback for a tag
     * @param tag Tag to cancel
     */
    void cancelTagResponse(uint8_t tag);

    // ========================================================================
    // State
    // ========================================================================

    /**
     * @brief Check if currently blocking on a tag
     */
    bool isWaiting() const { return _waitingTag != 0; }

    /**
     * @brief Get the tag currently being waited on (0 if none)
     */
    uint8_t waitingTag() const { return _waitingTag; }

    /**
     * @brief Clear all state (stash, callbacks, wait)
     */
    void reset();

private:
    // Tag counter (1-255 wrapping, 0 reserved for async)
    uint8_t _nextTag = 1;
    unsigned long _commandTimeout_ms = 1000;  // ms

    // Blocking wait state
    volatile uint8_t _waitingTag = 0;
    volatile bool _waitResolved = false;
    CommandResult _waitResult;

    // Async one-shot callbacks
    struct AsyncEntry {
        uint8_t tag = 0;
        TagCallback callback;
        bool active = false;
    };
    AsyncEntry _asyncCallbacks[MAX_PENDING];

    // Out-of-order response stash
    struct StashEntry {
        uint8_t tag = 0;
        CommandResult result;
        bool valid = false;
    };
    StashEntry _stash[MAX_PENDING];

    void stash(uint8_t tag, const CommandResult& result);
    bool unstash(uint8_t tag, CommandResult& out);
};

#endif // SERIAL_RESULT_QUEUE_H
