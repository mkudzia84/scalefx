/*
 * Serial LightFX Protocol - Binary Protocol Types and Constants
 *
 * Binary COBS protocol definitions for LightFX controller.
 * This header contains shared protocol types used by both client and server.
 *
 * Client/Server classes are in sfx_boards library:
 *   - LightFxClient: lightfx/client/lightfx_client.h
 *   - LightFxServer: lightfx/server/lightfx_server.h
 *
 * Packet Types (0x40-0x5F range):
 *   LED_SET (0x40)         - [ch:u8][brightness:u8]
 *   LED_OFF (0x41)         - [ch:u8] (0=all)
 *   LED_SEQ_CLEAR (0x42)   - [ch:u8]
 *   LED_SEQ_ADD (0x43)     - [ch:u8][eventType:u8][params...]
 *   LED_SEQ_START (0x44)   - [ch:u8]
 *   LED_SEQ_STOP (0x45)    - [ch:u8]
 *   LED_SEQ_RESTART (0x46) - [ch:u8]
 *   LED_SEQ_STATUS (0x47)  - [ch:u8]
 *   LED_STATUS (0x48)      - Query all channels
 *   LED_SEQ_QUEUE (0x49)   - [ch:u8] -> LED_SEQ_QUEUE_RESP
 *   LED_MASTER_BRIGHTNESS (0x4A) - [pct:u8] (0-100)
 *   SERVO_SET (0x50)       - [id:u8][pulse:i16]
 *   SERVO_SETTINGS (0x51)  - [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16][rev:u8(opt)]
 *   LANDING_LIGHT_BIND (0x52)    - [slot:u8][servo:u8][channelMask:u8][brightness:u8]
 *                                  servo=0 means no servo (LED-only group)
 *                                  channelMask: bitmask, bit0=ch1 .. bit7=ch8
 *   LANDING_LIGHT_UNBIND (0x53)  - [slot:u8] (0=all)
 *   LANDING_LIGHT_DEPLOY (0x54)  - [slot:u8] (0=all)
 *   LANDING_LIGHT_RETRACT (0x55) - [slot:u8] (0=all)
 *   LIGHT_PROGRAM_SELECT (0x4D)  - [index:u8]   Activate program by 0-based index
 *   LIGHT_PROGRAM_RESET  (0x4E)  - (no payload) Stop sequences, retract groups, no active program
 */

#ifndef SERIAL_LIGHTFX_H
#define SERIAL_LIGHTFX_H

#include <Arduino.h>
#include <functional>
#include "serial/core/core.h"

// ============================================================================
// LightFX Binary Packet Types (0x40-0x5F range)
// ============================================================================

namespace LightFxPacket {
    // LED direct control
    constexpr uint8_t LED_SET           = 0x40;  // [ch:u8][brightness:u8]
    constexpr uint8_t LED_OFF           = 0x41;  // [ch:u8] (0=all)
    
    // LED sequence control
    constexpr uint8_t LED_SEQ_CLEAR     = 0x42;  // [ch:u8]
    constexpr uint8_t LED_SEQ_ADD       = 0x43;  // [ch:u8][event_type:u8][params...]
    constexpr uint8_t LED_SEQ_START     = 0x44;  // [ch:u8]
    constexpr uint8_t LED_SEQ_STOP      = 0x45;  // [ch:u8]
    constexpr uint8_t LED_SEQ_RESTART   = 0x46;  // [ch:u8]
    constexpr uint8_t LED_SEQ_STATUS    = 0x47;  // [ch:u8] -> response packet
    constexpr uint8_t LED_STATUS        = 0x48;  // -> response packet
    constexpr uint8_t LED_SEQ_QUEUE     = 0x49;  // [ch:u8] -> LED_SEQ_QUEUE_RESP
    
    // LED master brightness
    constexpr uint8_t LED_MASTER_BRIGHTNESS = 0x4A;  // [pct:u8] (0-100)
    
    // LED reset and enable/disable
    constexpr uint8_t LED_RESET             = 0x4B;  // [ch:u8] (0=all) Reset channel to defaults, re-enable
    constexpr uint8_t LED_ENABLE            = 0x4C;  // [ch:u8][enabled:u8] Enable/disable LED channel

    // Light program runtime control (resolved via LightProgramManager loaded
    // from /lightfx.yaml). HubFX uses these to drive both its local channels
    // and the LightFX slave from the same program index, so on/off/select stay
    // in lock-step across both boards.
    constexpr uint8_t LIGHT_PROGRAM_SELECT  = 0x4D;  // [index:u8]  Activate program by 0-based index
    constexpr uint8_t LIGHT_PROGRAM_RESET   = 0x4E;  // (no payload) Stop sequences, retract all groups, no active program

    // Servo control
    constexpr uint8_t SERVO_SET         = 0x50;  // [id:u8][pulse:i16]
    constexpr uint8_t SERVO_SETTINGS    = 0x51;  // [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
    
    // Landing light control
    constexpr uint8_t LANDING_LIGHT_BIND    = 0x52;  // [slot:u8][servo_id:u8][channelMask:u8][brightness:u8]  (servo_id=0: no servo, channelMask: bit0=ch1..bit7=ch8)
    constexpr uint8_t LANDING_LIGHT_UNBIND  = 0x53;  // [slot:u8] (0=all)
    constexpr uint8_t LANDING_LIGHT_DEPLOY  = 0x54;  // [slot:u8] (0=all)
    constexpr uint8_t LANDING_LIGHT_RETRACT = 0x55;  // [slot:u8] (0=all)
    constexpr uint8_t LANDING_LIGHT_STATUS  = 0x56;  // [slot:u8][phase:u8][finished:u8] async progress

    // LightFX-specific battery safety: soft-disable LED channels on low voltage.
    // Sensor configuration (chemistry, cell count) is handled by the generic
    // BatteryServerT through CorePacket::BATTERY_CONFIG (0xEE).
    constexpr uint8_t BATTERY_AUTO_CUTOFF   = 0x5E;  // [enabled:u8]

    // Response packets (server -> client)
    constexpr uint8_t LED_SEQ_STATUS_RESP  = 0x5A;  // [ch:u8][playing:u8][events:u8][index:u8][loops:u32][brightness:u8]
    constexpr uint8_t LED_STATUS_RESP      = 0x5B;  // [ch:u8][brightness:u8][seq_playing:u8][events:u8] x8
    constexpr uint8_t LED_SEQ_QUEUE_RESP   = 0x5D;  // [ch:u8][count:u8][index:u8][playing:u8][brightness:u8][events: type:u8,duration:u16,param:u8 x count]
}

// LED event types for binary protocol
// NOTE: For ON and OFF, duration=0 means infinite — the event never completes,
//       which prevents the sequence from advancing or looping.  Use as a
//       terminal event to hold a steady state (e.g., FADE_IN → ON∞).
namespace LightFxEventType {
    constexpr uint8_t ON        = 0x00;  // [duration:u16][pwmDuty:u16][brightness:u8]  (duration 0=infinite/no-loop, pwmDuty 0=off, 1-100=power save)
    constexpr uint8_t OFF       = 0x01;  // [duration:u16]  (duration 0=infinite/no-loop)
    constexpr uint8_t FLASH     = 0x02;  // [interval:u16][duration:u16][brightness:u8][duty:u8]
    constexpr uint8_t FADE_IN   = 0x03;  // [duration:u16][unused:u16][brightness:u8]
    constexpr uint8_t FADE_OUT  = 0x04;  // [duration:u16][unused:u16][brightness:u8]
    constexpr uint8_t FADING    = 0x05;  // [cycle:u16][duration:u16][min:u8][max:u8]
    constexpr uint8_t BEACON    = 0x06;  // [cycle:u16][duration:u16][flashPct:u8][max:u8][min:u8]
    constexpr uint8_t MAX_TYPE  = 0x06;  // Maximum valid event type
}

// Landing light sequence phase constants (wire format)
namespace LandingLightPhase {
    constexpr uint8_t RETRACTED  = 0;
    constexpr uint8_t DEPLOYING  = 1;
    constexpr uint8_t DEPLOYED   = 2;
    constexpr uint8_t RETRACTING = 3;
}

// ============================================================================
// LightFX Hardware Specification Constants
// ============================================================================

namespace LightFxSpec {
    // LED channel limits
    constexpr uint8_t LED_CHANNEL_MIN = 1;
    constexpr uint8_t LED_CHANNEL_MAX = 8;
    constexpr uint8_t LED_BRIGHTNESS_MAX = 100;
    constexpr uint8_t MASTER_BRIGHTNESS_MAX = 100;  // percent
    
    // Servo limits (standard PWM range)
    constexpr uint8_t SERVO_ID_MIN = 1;
    constexpr uint8_t SERVO_ID_MAX = 3;
    constexpr uint16_t SERVO_PULSE_MIN = 500;    // µs
    constexpr uint16_t SERVO_PULSE_MAX = 2500;   // µs
    
    // Sequence limits
    constexpr uint8_t SEQ_MAX_EVENTS = 24;
    
    // Validation helpers
    inline bool isValidLedChannel(uint8_t ch) {
        return ch >= LED_CHANNEL_MIN && ch <= LED_CHANNEL_MAX;
    }
    
    inline bool isValidLedChannelOrAll(uint8_t ch) {
        return ch == 0 || (ch >= LED_CHANNEL_MIN && ch <= LED_CHANNEL_MAX);
    }
    
    inline bool isValidServoId(uint8_t id) {
        return id >= SERVO_ID_MIN && id <= SERVO_ID_MAX;
    }
    
    inline bool isValidServoPulse(int16_t pulse) {
        return pulse == -1 || (pulse >= SERVO_PULSE_MIN && pulse <= SERVO_PULSE_MAX);
    }
    
    inline bool isValidEventType(uint8_t type) {
        return type <= LightFxEventType::MAX_TYPE;
    }
    
    inline bool isValidMasterBrightness(uint8_t pct) {
        return pct <= MASTER_BRIGHTNESS_MAX;
    }
    
    // Landing light slot limits
    constexpr uint8_t LANDING_LIGHT_SLOT_MIN = 1;
    constexpr uint8_t LANDING_LIGHT_SLOT_MAX = 3;
    
    inline bool isValidLandingLightSlot(uint8_t slot) {
        return slot >= LANDING_LIGHT_SLOT_MIN && slot <= LANDING_LIGHT_SLOT_MAX;
    }
    
    inline bool isValidLandingLightSlotOrAll(uint8_t slot) {
        return slot == 0 || isValidLandingLightSlot(slot);
    }
    
    /** @brief Validate channel mask (at least one bit set, only bits 0-7) */
    inline bool isValidChannelMask(uint8_t mask) {
        return mask != 0;  // All 8 bits valid (ch1-ch8), just need at least one
    }
    
    /** @brief Validate servo ID for landing light (0=no servo, 1-3=servo) */
    inline bool isValidLandingServoId(uint8_t id) {
        return id == 0 || isValidServoId(id);
    }
}

// ============================================================================
// LightFX Error Codes
// ============================================================================

namespace LightFxError {
    constexpr uint8_t OK                = 0x00;
    constexpr uint8_t INVALID_CHANNEL   = 0x50;
    constexpr uint8_t SEQ_FULL          = 0x51;
    constexpr uint8_t INVALID_EVENT     = 0x52;
    constexpr uint8_t INVALID_PARAM     = 0x53;
    constexpr uint8_t INVALID_SERVO     = 0x54;
    constexpr uint8_t INVALID_SLOT      = 0x55;
    constexpr uint8_t CHANNEL_DISABLED  = 0x56;
    constexpr uint8_t INVALID_PROGRAM   = 0x57;  // LIGHT_PROGRAM_SELECT index out of range / no config

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case OK:              return "OK";
            case INVALID_CHANNEL: return "Invalid LED channel";
            case SEQ_FULL:        return "Sequence buffer full";
            case INVALID_EVENT:   return "Invalid event type";
            case INVALID_PARAM:   return "Invalid parameter";
            case INVALID_SERVO:   return "Invalid servo ID";
            case INVALID_SLOT:    return "Invalid landing light slot";
            case CHANNEL_DISABLED: return "LED channel disabled";
            case INVALID_PROGRAM: return "Invalid light program index";
            default:              return SerialError::getMessage(code);
        }
    }
}

// ============================================================================
// LightFX Data Types
// ============================================================================

/**
 * @brief LED sequence status information
 */
struct LightFxSeqStatus {
    uint8_t channel = 0;
    bool playing = false;
    uint8_t eventCount = 0;
    uint8_t currentIndex = 0;
    uint32_t loopCount = 0;
    uint8_t brightness = 0;  // Current calculated brightness 0-100
};

/**
 * @brief LED channel status information
 */
struct LightFxChannelStatus {
    uint8_t channel = 0;
    uint8_t brightness = 0;
    bool seqPlaying = false;
    uint8_t seqEventCount = 0;
};

/**
 * @brief Single event info for queue query
 */
struct LightFxEventInfo {
    uint8_t type = 0;       // Event type (LightFxEventType::*)
    uint16_t duration = 0;  // Duration in ms
    uint8_t param1 = 0;     // Event-specific param (e.g., brightness)
};

/**
 * @brief LED sequence queue information
 */
struct LightFxSeqQueue {
    uint8_t channel = 0;
    uint8_t count = 0;
    uint8_t currentIndex = 0;
    bool playing = false;
    uint8_t brightness = 0;  // Current calculated brightness 0-100
    LightFxEventInfo events[24];  // Max 24 events
};

/**
 * @brief Landing light deploy/retract progress status
 *
 * Wire format: [slot:u8][phase:u8][finished:u8] = 3 bytes
 */
struct LightFxLandingLightStatus {
    uint8_t slot = 0;       // Landing light slot (1-3)
    uint8_t phase = 0;      // LandingLightPhase value
    bool finished = false;  // true when operation complete
};

/**
 * @brief Servo configuration for motion profiling
 *
 * Wire format: [id:u8][min:u16LE][max:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]
 * Total: 11 bytes
 */
struct LightFxServoConfig {
    uint8_t servoId = 0;
    uint16_t minUs = 500;                  // Minimum pulse width limit          // µs
    uint16_t maxUs = 2500;                 // Maximum pulse width limit          // µs
    uint16_t maxSpeedUsPerSec = 4000;      // Maximum speed                     // µs/s
    uint16_t maxAccelUsPerSec2 = 8000;     // Acceleration                      // µs/s²
    uint16_t maxDecelUsPerSec2 = 8000;     // Deceleration                      // µs/s²
    bool reversed = false;                 // Direction: false=open@max, true=open@min (optional byte 12)
};

// ============================================================================
// Callback Types
// ============================================================================

// Client callbacks
using LightFxSeqStatusCallback = std::function<void(const LightFxSeqStatus& status)>;
using LightFxChannelStatusCallback = std::function<void(const LightFxChannelStatus& status)>;
using LightFxSeqQueueCallback = std::function<void(const LightFxSeqQueue& queue)>;
using LightFxLandingLightStatusCallback = std::function<void(const LightFxLandingLightStatus& status)>;

// Server callbacks - return error code (LightFxError::OK for success)
using LedSetCallback = std::function<uint8_t(uint8_t channel, uint8_t brightness)>;
using LedOffCallback = std::function<uint8_t(uint8_t channel)>;
using LedSeqClearCallback = std::function<uint8_t(uint8_t channel)>;
using LedSeqAddCallback = std::function<uint8_t(uint8_t channel, uint8_t eventType,
                                                 uint16_t param1, uint16_t param2,
                                                 uint8_t param3, uint8_t param4)>;
using LedSeqStartCallback = std::function<uint8_t(uint8_t channel)>;
using LedSeqStopCallback = std::function<uint8_t(uint8_t channel)>;
using LedSeqRestartCallback = std::function<uint8_t(uint8_t channel)>;
using LedSeqStatusCallback = std::function<void(uint8_t channel, LightFxSeqStatus& status)>;
using LedSeqQueueCallback = std::function<void(uint8_t channel, LightFxSeqQueue& queue)>;
using LedChannelStatusCallback = std::function<void(uint8_t channel, LightFxChannelStatus& status)>;
using ServoSetCallback = std::function<uint8_t(uint8_t id, int pulseUs)>;
using ServoSettingsCallback = std::function<uint8_t(const LightFxServoConfig& config)>;
using LedMasterBrightnessCallback = std::function<uint8_t(uint8_t pct)>;
using LedResetCallback = std::function<uint8_t(uint8_t channel)>;
using LedEnableCallback = std::function<uint8_t(uint8_t channel, uint8_t enabled)>;

// Landing light callbacks
using LandingLightBindCallback = std::function<uint8_t(uint8_t slot, uint8_t servoId, uint8_t channelMask,
                                                       uint8_t brightness)>;
using LandingLightSlotCallback = std::function<uint8_t(uint8_t slot)>;

// Battery safety callback (LightFX-specific auto-cutoff toggle)
using LightFxBatteryAutoCutoffCallback = std::function<uint8_t(bool enabled)>;

// Light program runtime callbacks
using LightProgramSelectCallback = std::function<uint8_t(uint8_t programIndex)>;
using LightProgramResetCallback  = std::function<uint8_t()>;

#endif // SERIAL_LIGHTFX_H
