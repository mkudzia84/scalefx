/*
 * Serial LightFX Protocol - Binary Protocol Client/Server
 *
 * Binary COBS protocol client/server for LightFX controller.
 *   - LightFxClient: For HubFX (sends commands via USB)
 *   - LightFxServer: For LightFX Pico (receives commands, implements ICommandHandler)
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
 *   SERVO_SETTINGS (0x51)  - [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
 *   LANDING_LIGHT_BIND (0x52)    - [slot:u8][servo:u8][led:u8][deploy:u16][retract:u16][brightness:u8]
 *   LANDING_LIGHT_UNBIND (0x53)  - [slot:u8] (0=all)
 *   LANDING_LIGHT_DEPLOY (0x54)  - [slot:u8] (0=all)
 *   LANDING_LIGHT_RETRACT (0x55) - [slot:u8] (0=all)
 */

#ifndef SERIAL_LIGHTFX_H
#define SERIAL_LIGHTFX_H

#include <Arduino.h>
#include <functional>
#include "serial_core.h"
#include "serial_bus.h"
#include "serial_error.h"
#include "serial_command_handler.h"

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
    
    // Servo control
    constexpr uint8_t SERVO_SET         = 0x50;  // [id:u8][pulse:i16]
    constexpr uint8_t SERVO_SETTINGS    = 0x51;  // [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
    
    // Landing light control
    constexpr uint8_t LANDING_LIGHT_BIND    = 0x52;  // [slot:u8][servo_id:u8][led_ch:u8][deployUs:u16][retractUs:u16][brightness:u8]
    constexpr uint8_t LANDING_LIGHT_UNBIND  = 0x53;  // [slot:u8] (0=all)
    constexpr uint8_t LANDING_LIGHT_DEPLOY  = 0x54;  // [slot:u8] (0=all)
    constexpr uint8_t LANDING_LIGHT_RETRACT = 0x55;  // [slot:u8] (0=all)
    
    // Response packets (server -> client)
    constexpr uint8_t LED_SEQ_STATUS_RESP  = 0x5A;  // [ch:u8][playing:u8][events:u8][index:u8][loops:u32]
    constexpr uint8_t LED_STATUS_RESP      = 0x5B;  // [ch:u8][brightness:u8][seq_playing:u8][events:u8] x8
    constexpr uint8_t LED_SEQ_QUEUE_RESP   = 0x5D;  // [ch:u8][count:u8][events: type:u8,duration:u16 x count]
}

// LED event types for binary protocol
namespace LightFxEventType {
    constexpr uint8_t ON        = 0x00;  // [duration:u16][brightness:u8]
    constexpr uint8_t OFF       = 0x01;  // [duration:u16]
    constexpr uint8_t FLASH     = 0x02;  // [interval:u16][duration:u16][brightness:u8][duty:u8]
    constexpr uint8_t FADE_IN   = 0x03;  // [duration:u16][brightness:u8]
    constexpr uint8_t FADE_OUT  = 0x04;  // [duration:u16][brightness:u8]
    constexpr uint8_t FADING    = 0x05;  // [cycle:u16][duration:u16][min:u8][max:u8]
    constexpr uint8_t MAX_TYPE  = 0x05;  // Maximum valid event type
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
    
    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case OK:              return "OK";
            case INVALID_CHANNEL: return "Invalid LED channel";
            case SEQ_FULL:        return "Sequence buffer full";
            case INVALID_EVENT:   return "Invalid event type";
            case INVALID_PARAM:   return "Invalid parameter";
            case INVALID_SERVO:   return "Invalid servo ID";
            case INVALID_SLOT:    return "Invalid landing light slot";
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
    LightFxEventInfo events[24];  // Max 24 events
};

/**
 * @brief Board information returned during init
 */
struct LightFxBoardInfo {
    char deviceName[32] = "";
    char firmwareVersion[16] = "";
    char platform[16] = "";
    uint16_t cpuFrequencyMHz = 0;
    uint32_t freeRamBytes = 0;
    bool versionCompatible = true;
};

// ============================================================================
// Callback Types
// ============================================================================

// Client callbacks
using LightFxReadyCallback = std::function<void(const char* deviceName)>;
using LightFxSeqStatusCallback = std::function<void(const LightFxSeqStatus& status)>;
using LightFxChannelStatusCallback = std::function<void(const LightFxChannelStatus& status)>;
using LightFxErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;

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
using ServoSettingsCallback = std::function<uint8_t(uint8_t id, int minUs, int maxUs, int speed, int accel, int decel)>;
using LedMasterBrightnessCallback = std::function<uint8_t(uint8_t pct)>;

// Landing light callbacks
using LandingLightBindCallback = std::function<uint8_t(uint8_t slot, uint8_t servoId, uint8_t ledChannel,
                                                       uint16_t deployUs, uint16_t retractUs, uint8_t brightness)>;
using LandingLightSlotCallback = std::function<uint8_t(uint8_t slot)>;

// ============================================================================
// LightFxClient Class (Binary Protocol)
// ============================================================================

/**
 * @brief Client-side LightFX serial communication (binary COBS protocol)
 * 
 * Used by HubFX to send commands to LightFX server boards over USB.
 * Extends SerialBus with LightFX-specific commands.
 */
class LightFxClient : public SerialBus {
public:
    LightFxClient() = default;
    ~LightFxClient() = default;

    LightFxClient(const LightFxClient&) = delete;
    LightFxClient& operator=(const LightFxClient&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================
    
    /**
     * @brief Initialize with USB host
     */
    bool begin(UsbHost* usbHost, int deviceIndex);
    
    /**
     * @brief Process incoming packets (call in loop)
     */
    int process();

    // ========================================================================
    // LED Direct Control
    // ========================================================================
    
    bool ledSet(uint8_t channel, uint8_t brightness);
    bool ledOff(uint8_t channel = 0);

    // ========================================================================
    // LED Sequence Control
    // ========================================================================
    
    bool ledSeqClear(uint8_t channel);
    bool ledSeqAddOn(uint8_t channel, uint16_t durationMs, uint8_t brightness = 255);
    bool ledSeqAddOff(uint8_t channel, uint16_t durationMs);
    bool ledSeqAddFlash(uint8_t channel, uint16_t intervalMs, uint16_t durationMs,
                        uint8_t brightness = 255, uint8_t dutyPercent = 50);
    bool ledSeqAddFadeIn(uint8_t channel, uint16_t durationMs, uint8_t brightness = 255);
    bool ledSeqAddFadeOut(uint8_t channel, uint16_t durationMs, uint8_t brightness = 255);
    bool ledSeqAddFading(uint8_t channel, uint16_t cycleMs, uint16_t durationMs = 0,
                         uint8_t minBrightness = 0, uint8_t maxBrightness = 255);
    bool ledSeqStart(uint8_t channel);
    bool ledSeqStop(uint8_t channel);
    bool ledSeqRestart(uint8_t channel);
    bool ledSeqStatus(uint8_t channel);
    bool ledStatus();
    bool ledMasterBrightness(uint8_t pct);

    // ========================================================================
    // Servo Control
    // ========================================================================
    
    bool servoSet(uint8_t id, int16_t pulseUs);
    bool servoSettings(uint8_t id, uint16_t minUs, uint16_t maxUs,
                       uint16_t speed, uint16_t accel, uint16_t decel);

    // ========================================================================
    // Landing Light Control
    // ========================================================================
    
    bool landingLightBind(uint8_t slot, uint8_t servoId, uint8_t ledChannel,
                          uint16_t deployUs, uint16_t retractUs, uint8_t brightness = 255);
    bool landingLightUnbind(uint8_t slot = 0);
    bool landingLightDeploy(uint8_t slot = 0);
    bool landingLightRetract(uint8_t slot = 0);

    // ========================================================================
    // Connection Management
    // ========================================================================
    
    int sendInit(unsigned long keepaliveMs = 0);

    // ========================================================================
    // Configuration
    // ========================================================================
    
    void setCommandTimeout(unsigned long timeoutMs) { _commandTimeoutMs = timeoutMs; }
    void setBlockingMode(bool blocking) { _blockingMode = blocking; }

    // ========================================================================
    // Callbacks
    // ========================================================================
    
    void onReady(LightFxReadyCallback cb) { _readyCallback = cb; }
    void onSeqStatus(LightFxSeqStatusCallback cb) { _seqStatusCallback = cb; }
    void onChannelStatus(LightFxChannelStatusCallback cb) { _channelStatusCallback = cb; }
    void onError(LightFxErrorCallback cb) { _errorCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================
    
    bool isServerReady() const { return _serverReady; }
    const char* serverName() const { return _serverName; }
    const LightFxBoardInfo& boardInfo() const { return _boardInfo; }
    bool lastCommandSuccess() const { return _lastAckReceived; }

private:
    void handlePacket(uint8_t type, const uint8_t* payload, size_t len);
    bool sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len);
    bool waitForAckNack();

    UsbHost* _usbHostRef = nullptr;
    bool _serverReady = false;
    char _serverName[32] = "";
    LightFxBoardInfo _boardInfo;

    unsigned long _commandTimeoutMs = 1000;
    bool _blockingMode = true;

    volatile bool _pendingAckNack = false;
    volatile bool _lastAckReceived = false;
    volatile bool _receivedAck = false;
    volatile bool _receivedNack = false;
    uint8_t _lastNackErrorCode = 0;

    LightFxReadyCallback _readyCallback;
    LightFxSeqStatusCallback _seqStatusCallback;
    LightFxChannelStatusCallback _channelStatusCallback;
    LightFxErrorCallback _errorCallback;
};

// ============================================================================
// LightFxServer Class (Binary Protocol)
// ============================================================================

/**
 * @brief Server-side LightFX serial communication (binary COBS protocol)
 * 
 * Used by LightFX Pico to receive commands from HubFX client.
 * Implements ICommandHandler for use with CommandRouter.
 */
class LightFxServer : public ICommandHandler {
public:
    LightFxServer() = default;
    ~LightFxServer() override = default;

    // ========================================================================
    // Initialization
    // ========================================================================
    
    bool begin(Stream* serial);
    void end();

    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================
    
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override;
    const char* handlerName() const override { return "LightFxServer"; }

    // ========================================================================
    // Response Methods
    // ========================================================================
    
    int sendAck();
    int sendNack(uint8_t errorCode);
    int sendSeqStatus(const LightFxSeqStatus& status);
    int sendSeqQueue(const LightFxSeqQueue& queue);
    int sendChannelStatus(const LightFxChannelStatus* channels, uint8_t count);

    // ========================================================================
    // LED Callbacks
    // ========================================================================
    
    void onLedSet(LedSetCallback cb) { _ledSetCallback = cb; }
    void onLedOff(LedOffCallback cb) { _ledOffCallback = cb; }
    void onLedSeqClear(LedSeqClearCallback cb) { _ledSeqClearCallback = cb; }
    void onLedSeqAdd(LedSeqAddCallback cb) { _ledSeqAddCallback = cb; }
    void onLedSeqStart(LedSeqStartCallback cb) { _ledSeqStartCallback = cb; }
    void onLedSeqStop(LedSeqStopCallback cb) { _ledSeqStopCallback = cb; }
    void onLedSeqRestart(LedSeqRestartCallback cb) { _ledSeqRestartCallback = cb; }
    void onLedSeqStatus(LedSeqStatusCallback cb) { _ledSeqStatusCallback = cb; }
    void onLedSeqQueue(LedSeqQueueCallback cb) { _ledSeqQueueCallback = cb; }
    void onLedStatus(LedChannelStatusCallback cb) { _ledStatusCallback = cb; }
    void onLedMasterBrightness(LedMasterBrightnessCallback cb) { _ledMasterBrightnessCallback = cb; }

    // ========================================================================
    // Servo Callbacks
    // ========================================================================
    
    void onServoSet(ServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(ServoSettingsCallback cb) { _servoSettingsCallback = cb; }

    // ========================================================================
    // Landing Light Callbacks
    // ========================================================================
    
    void onLandingLightBind(LandingLightBindCallback cb) { _landingLightBindCallback = cb; }
    void onLandingLightUnbind(LandingLightSlotCallback cb) { _landingLightUnbindCallback = cb; }
    void onLandingLightDeploy(LandingLightSlotCallback cb) { _landingLightDeployCallback = cb; }
    void onLandingLightRetract(LandingLightSlotCallback cb) { _landingLightRetractCallback = cb; }

private:
    int sendRawPacket(uint8_t type, const uint8_t* payload = nullptr, size_t len = 0);
    
    Stream* _serial = nullptr;
    bool _initialized = false;

    LedSetCallback _ledSetCallback;
    LedOffCallback _ledOffCallback;
    LedSeqClearCallback _ledSeqClearCallback;
    LedSeqAddCallback _ledSeqAddCallback;
    LedSeqStartCallback _ledSeqStartCallback;
    LedSeqStopCallback _ledSeqStopCallback;
    LedSeqRestartCallback _ledSeqRestartCallback;
    LedSeqStatusCallback _ledSeqStatusCallback;
    LedSeqQueueCallback _ledSeqQueueCallback;
    LedChannelStatusCallback _ledStatusCallback;
    LedMasterBrightnessCallback _ledMasterBrightnessCallback;
    ServoSetCallback _servoSetCallback;
    ServoSettingsCallback _servoSettingsCallback;
    LandingLightBindCallback _landingLightBindCallback;
    LandingLightSlotCallback _landingLightUnbindCallback;
    LandingLightSlotCallback _landingLightDeployCallback;
    LandingLightSlotCallback _landingLightRetractCallback;
};

#endif // SERIAL_LIGHTFX_H
