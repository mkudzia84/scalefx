/*
 * input_dispatcher.h — `InputDispatcherServicePolicyT<TTopology>`.
 *
 *   Master-side service that owns:
 *     - subscription to topology role-events (RCIN / SBUS / JETIEX
 *       broadcasts from any board, local or remote);
 *     - the registration table of `TriggerInput`s that effects own,
 *       each bound to a (source PortRef, channel) tuple;
 *     - the per-protocol frame decode (extract channel µs out of the
 *       payload, including SBUS's `flags` byte for ch17 / ch18).
 *
 *   Effects don't parse RC wire formats themselves.  They configure
 *   one or more `TriggerInput` instances, register each with the
 *   dispatcher pointing at a source port + channel index, and receive
 *   typed-value callbacks on edge changes only.
 *
 *   Channel index convention — **0-based across the system**:
 *     - RC PWM: always channel 0
 *     - SBUS:   0..15 proportional, 16 = ch17 (digital), 17 = ch18 (digital)
 *     - Jeti:   0..23 proportional
 *
 *   `TriggerInput::channel` is `u8` so 0..255 is supported; in
 *   practice all current protocols fit in 0..23.
 *
 *   No new capability bit — effects opt-in to using the dispatcher;
 *   nothing on the wire changes.  Slotted into `BoardOf<...>` AFTER
 *   the topology service.
 */

#ifndef HUBFX_INPUT_DISPATCHER_H
#define HUBFX_INPUT_DISPATCHER_H

#include <concepts>
#include <cstdint>
#include <cstring>

#include <serial/core/core.h>
#include <serial/diag_log.h>
#include <serial/roles.h>
#include <server/board_server.h>

#include "../effect_id.h"
#include "../../topology/topology_service.h"      // TopologyService concept
#include "trigger_input.h"
#include "input_protocol.h"                        // InputRoutingPacket + TelemetryPacket
#include <jeti_ex/jeti_telemetry_hub.h>            // the telemetry collection (item 4)

namespace hubfx::effects::input {

// ────────────────────────────────────────────────────────────────────
//  InputDispatcher concept
// ────────────────────────────────────────────────────────────────────
//
// Contract every type passed as `TInputDispatcher` to consumers
// (EngineFx, GunFx) must satisfy.  `InputDispatcherServicePolicyT<TTopology>`
// is the canonical model.
//
// Effects register a `TriggerInput` instance, a source `PortRef`, and
// a 0-based channel index.  The dispatcher takes care of decoding
// SBUS / Jeti / RC PWM frames and feeding the right channel µs to
// each registered TriggerInput on every relevant role event.
//
template <typename T>
concept InputDispatcher = requires(T& d, TriggerInput* in,
                                   const PortRef& source, uint8_t channel) {
    { d.subscribe(in, source, channel) } -> std::convertible_to<bool>;
    { d.unsubscribe(in) };
};

template <hubfx::topology::TopologyService TTopology>
class InputDispatcherServicePolicyT {
public:
    /// No capability bit — purely master-internal plumbing.
    static constexpr uint32_t kCapabilityBits = 0u;

    /// Max simultaneous (TriggerInput, source, channel) registrations.
    /// Sized for the realistic HubFX rev: 1 engine toggle + 1 throttle
    /// + 1 gun trigger + 1 gun smoke arm + 1 light-program selector +
    /// 1 master-brightness pot + headroom = 16 is plenty.
    static constexpr uint8_t kMaxBindings = 16;

    InputDispatcherServicePolicyT() = default;

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == InputRoutingPacket::INPUT_ROUTING_SET_ENABLED
            || type == InputRoutingPacket::INPUT_ROUTING_GET_REQ
            || type == TelemetryPacket::TELEMETRY_GET_REQ
            || type == ConnectionPacket::CONNECTION_GET_REQ
            || type == ConnectionPacket::CONNECTION_SET_CFG;
    }
    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);
    /// Per-pass: drive the generic connection-loss state machine (item 5).
    void update();

    // ── Connection-loss (item 5) ─────────────────────────────────────
    //
    // In-firmware subscription for effects (e.g. GearControl) that want to
    // react to an input link dropping.  `state`: 0 = up/recovered, 1 = lost
    // (signal gone, holding + brownout), 2 = down (loss confirmed past the
    // configurable interval — the actionable signal).  Single subscriber is
    // enough for now (gear); add a small list if more effects need it.
    using ConnectionCallback = void (*)(void* ctx, const char* guid,
                                        uint8_t portKind, uint8_t portIdx,
                                        uint8_t state);
    void onConnectionLoss(ConnectionCallback cb, void* ctx) {
        _connCb = cb; _connCtx = ctx;
    }
    /// Global link-loss interval (ms): silence beyond this → DOWN signal.
    /// 0 disables loss signalling entirely.  Default 1000.
    void setLinkLossMs(uint16_t ms) { _linkLossMs = ms; }
    uint16_t linkLossMs() const     { return _linkLossMs; }

    // ── Global RC-routing gate ───────────────────────────────────────
    //
    // When false, onRoleEvent stops feeding subscribed TriggerInputs, so
    // RC inputs no longer drive effects (effects hold their last state;
    // the operator drives them from Studio).  The wire broadcast that
    // monitors read is a separate fan-out and keeps flowing either way.
    // Protocol-agnostic — the gate is after the per-protocol extractor,
    // so it covers PPM / SBUS / Jeti EX uniformly.  Default on.
    void setRoutingEnabled(bool en) { _routingEnabled = en; }
    bool routingEnabled() const     { return _routingEnabled; }

    // ── Effects-facing API ───────────────────────────────────────────

    /// Bind `input` to a specific (source port, channel index).
    /// The TriggerInput must already be `configure()`'d with its
    /// mapping + callback.  Returns false if the binding table is
    /// full or `input == nullptr`.
    bool subscribe(TriggerInput* input,
                   const PortRef& source, uint8_t channel);

    /// Drop a registration.  Safe to call from inside the callback.
    void unsubscribe(TriggerInput* input);

    /// Inspection.
    uint8_t numBindings() const { return _numBindings; }

    /// Iterate every subscribed binding.  Used by the telemetry emitter
    /// to walk live RC channel values without exposing the binding
    /// table.  Callback signature: `fn(TriggerInput*, const PortRef&,
    /// uint8_t channel)`.  Pure inspection — must not mutate the
    /// dispatcher's state.
    template <typename Fn>
    void forEachBinding(Fn&& fn) const {
        for (uint8_t i = 0; i < kMaxBindings; ++i) {
            if (_bindings[i].occupied && _bindings[i].input) {
                fn(_bindings[i].input, _bindings[i].source, _bindings[i].channel);
            }
        }
    }

private:
    void onRoleEvent(const char* guid, uint8_t innerType,
                     const uint8_t* p, size_t len);
    static void roleEventTrampoline(void* ctx, const char* guid,
                                    uint8_t innerType,
                                    const uint8_t* p, size_t len);

    // Per-protocol channel extractors — return true + write `outUs` /
    // `outValid` for the requested channel; false if channel out of range.
    static bool extractRcPwm  (const uint8_t* p, size_t len,
                               uint8_t channel,
                               uint16_t& outUs, bool& outValid);
    static bool extractPpm    (const uint8_t* p, size_t len,
                               uint8_t channel,
                               uint16_t& outUs, bool& outValid);
    static bool extractSbus   (const uint8_t* p, size_t len,
                               uint8_t channel,
                               uint16_t& outUs, bool& outValid);
    static bool extractJetiEx (const uint8_t* p, size_t len,
                               uint8_t channel,
                               uint16_t& outUs, bool& outValid);

    // Match a binding's source against an incoming event.  Empty
    // hub-guid in either side matches the hub-local case.
    static bool sourceMatches(const PortRef& bindSource,
                              uint8_t evtPortKind, uint8_t evtPortIdx,
                              const char* evtGuid);

    // Serialize the telemetry collection (JetiTelemetryHub) into `buf` for
    // TELEMETRY_RESP — bounded to `cap`, truncating devices/sensors that don't
    // fit.  Returns bytes written.  (item 4)
    size_t buildTelemetrySnapshot(uint8_t* buf, size_t cap) const;

    // Target publish interval (ms) for the collection — mirrors
    // JetiExpander::replyIntervalMs() (each metric ~10 Hz, clamped) so the read
    // path stays decoupled from the Core-0 expander header.
    static uint16_t targetPubIntervalMs(uint8_t activeSensors) {
        const uint32_t n  = activeSensors ? activeSensors : 1u;
        uint32_t iv = 1000u / (n * 10u);           // kPerValueHz = 10
        if (iv < 25u)  iv = 25u;                    // kMinReplyIntervalMs
        if (iv > 200u) iv = 200u;                   // kMaxReplyIntervalMs
        return (uint16_t)iv;
    }

    struct Binding {
        TriggerInput* input   = nullptr;
        PortRef       source;
        uint8_t       channel = 0;
        bool          occupied = false;
    };

    Binding _bindings[kMaxBindings] = {};
    uint8_t _numBindings            = 0;
    bool    _routingEnabled         = true;   ///< RC → effect feed gate

    // ── Connection-loss tracking (item 5) ─────────────────────────────
    // One slot per input source we've seen frames from.  ESP32-S3 hosts ≤2
    // input ports; remote-board inputs are rare → 4 slots is ample.
    static constexpr uint8_t kMaxLinks    = 4;
    static constexpr uint32_t kDetectMs   = 300;  ///< silence → LOST (hold + brownout + reset)
    struct Link {
        char     guid[9]   = {};      ///< "" = hub-local
        uint8_t  portKind  = 0;
        uint8_t  portIdx   = 0;
        uint32_t lastMs    = 0;       ///< last frame (SFX_MILLIS)
        uint16_t brownouts = 0;
        uint8_t  state     = 0;       ///< 0 up, 1 lost, 2 down
        bool     occupied  = false;
    };
    Link     _links[kMaxLinks] = {};
    uint16_t _linkLossMs       = 1000;     ///< DOWN threshold (configurable)
    ConnectionCallback _connCb  = nullptr;
    void*              _connCtx = nullptr;

    /// Find-or-add a link slot for (guid, portKind, portIdx); stamps lastMs and
    /// fires a recovery event if it was lost/down.  Called on every frame.
    void touchLink(const char* guid, uint8_t portKind, uint8_t portIdx, uint32_t nowMs);
    /// Fire the in-firmware callback + async CONNECTION_EVENT for one link.
    void fireConnection(const Link& l);

    sfx_core::BoardServerBase* _ctx  = nullptr;
    TTopology*                 _topo = nullptr;
};

}  // namespace hubfx::effects::input

#include "input_dispatcher.ipp"

#endif  // HUBFX_INPUT_DISPATCHER_H
