/*
 * LED status structs — protocol-native, used by LedRuntime and CoreServer.
 *
 * Replaces the LightFX-specific LightFxSeqStatus / LightFxChannelStatus
 * structs that the legacy LedManager filled.  Lives in the slave-
 * protocol namespace because that's where the wire format lives.
 *
 * All channel indices in these structs are 0-based to match the wire
 * format (master always passes 0-based indices via the address byte).
 */

#ifndef SFX_COMPONENT_LED_STATUS_H
#define SFX_COMPONENT_LED_STATUS_H

#include <cstdint>

namespace ComponentPacket {

/// Wire-format LED event (8 bytes — matches LED_QUEUE_LOAD payload format).
/// Meaning of p1..p5 depends on `type` (LedEventType::*) — see slave.h.
struct LedEvent {
    uint8_t  type;     ///< ComponentPacket::LedEventType::*
    uint16_t p1;
    uint16_t p2;
    uint8_t  p3;
    uint8_t  p4;
    uint8_t  p5;
};

/// Compact channel status — fills LED_QUERY_RESP payload.
struct LedChannelStatus {
    uint8_t addr         = 0;   ///< raw address byte (bit 7 = PWM-borrowed)
    uint8_t brightness   = 0;   ///< 0..255 last-emitted brightness
    uint8_t queueState   = 0;   ///< 0 = idle, 1 = playing, 2 = paused
    uint8_t currentEvent = 0;   ///< 0-based index within the loaded queue
};

/// Detailed runtime status — fills LED_QUEUE_STATUS_RESP payload.
struct LedQueueStatus {
    uint8_t  addr          = 0;
    uint8_t  brightness    = 0;   ///< current emitted brightness 0..255
    uint8_t  queueState    = 0;   ///< 0 = idle, 1 = playing, 2 = paused
    uint8_t  eventCount    = 0;   ///< total events loaded in the queue
    uint8_t  currentEvent  = 0;   ///< 0-based index within the queue
    uint8_t  currentType   = 0;   ///< LedEventType::* of the active event
    uint16_t timeInEvent_ms = 0;  ///< how long the active event has been running
    uint16_t timeRemaining_ms = 0;///< 0xFFFF if indefinite (duration_ms=0)
    uint8_t  flags         = 0;   ///< bit 0 = REPEAT, bit 1 = ENABLED, bit 2 = COMPLETE_LATCHED
};

namespace LedQueueState {
    constexpr uint8_t IDLE    = 0;
    constexpr uint8_t PLAYING = 1;
    constexpr uint8_t PAUSED  = 2;
}

namespace LedStatusFlags {
    constexpr uint8_t REPEAT             = 0x01;  ///< loaded queue's REPEAT bit
    constexpr uint8_t ENABLED            = 0x02;  ///< channel enabled (LED_ENABLE_CHANNEL)
    constexpr uint8_t COMPLETE_LATCHED   = 0x04;  ///< one-shot finished, awaiting clear
}

}  // namespace ComponentPacket

#endif  // SFX_COMPONENT_LED_STATUS_H
