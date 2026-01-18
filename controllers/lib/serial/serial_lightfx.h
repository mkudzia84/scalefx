/*
 * Serial LightFX Protocol - Binary Protocol Master/Slave
 *
 * Binary COBS protocol master/slave for LightFX controller.
 *   - LightFxMaster: For HubFX (sends commands via USB)
 *   - LightFxSlave: For LightFX Pico (receives commands, implements ICommandHandler)
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
 *   SERVO_SET (0x50)       - [id:u8][pulse:i16]
 *   SERVO_SETTINGS (0x51)  - [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
 *   POWER_STATUS (0x58)    - Query power monitor
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
    
    // Servo control
    constexpr uint8_t SERVO_SET         = 0x50;  // [id:u8][pulse:i16]
    constexpr uint8_t SERVO_SETTINGS    = 0x51;  // [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
    
    // Power monitoring
    constexpr uint8_t POWER_STATUS      = 0x58;  // -> response packet
    
    // Response packets (slave -> master)
    constexpr uint8_t LED_SEQ_STATUS_RESP  = 0x5A;  // [ch:u8][playing:u8][events:u8][index:u8][loops:u32]
    constexpr uint8_t LED_STATUS_RESP      = 0x5B;  // [ch:u8][brightness:u8][seq_playing:u8][events:u8] x8
    constexpr uint8_t POWER_STATUS_RESP    = 0x5C;  // [voltage:u16(mV)][current:i16(mA)][power:u16(mW)][available:u8]
}

// LED event types for binary protocol
namespace LightFxEventType {
    constexpr uint8_t ON        = 0x00;  // [duration:u16][brightness:u8]
    constexpr uint8_t OFF       = 0x01;  // [duration:u16]
    constexpr uint8_t FLASH     = 0x02;  // [interval:u16][duration:u16][brightness:u8][duty:u8]
    constexpr uint8_t FADE_IN   = 0x03;  // [duration:u16][brightness:u8]
    constexpr uint8_t FADE_OUT  = 0x04;  // [duration:u16][brightness:u8]
    constexpr uint8_t FADING    = 0x05;  // [cycle:u16][duration:u16][min:u8][max:u8]
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
    
    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case OK:              return "OK";
            case INVALID_CHANNEL: return "Invalid LED channel";
            case SEQ_FULL:        return "Sequence buffer full";
            case INVALID_EVENT:   return "Invalid event type";
            case INVALID_PARAM:   return "Invalid parameter";
            case INVALID_SERVO:   return "Invalid servo ID";
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
 * @brief Power monitor status information
 */
struct LightFxPowerStatus {
    float voltage = 0.0f;      // Bus voltage in V
    float current = 0.0f;      // Current in mA
    float power = 0.0f;        // Power in mW
    bool available = false;     // INA226 detected
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

// Master callbacks
using LightFxReadyCallback = std::function<void(const char* deviceName)>;
using LightFxSeqStatusCallback = std::function<void(const LightFxSeqStatus& status)>;
using LightFxChannelStatusCallback = std::function<void(const LightFxChannelStatus& status)>;
using LightFxPowerStatusCallback = std::function<void(const LightFxPowerStatus& status)>;
using LightFxErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;

// Slave callbacks - return error code (LightFxError::OK for success)
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
using LedChannelStatusCallback = std::function<void(uint8_t channel, LightFxChannelStatus& status)>;
using ServoSetCallback = std::function<uint8_t(uint8_t id, int pulseUs)>;
using ServoSettingsCallback = std::function<uint8_t(uint8_t id, int minUs, int maxUs, int speed, int accel, int decel)>;
using PowerStatusCallback = std::function<void(LightFxPowerStatus& status)>;

// ============================================================================
// LightFxMaster Class (Binary Protocol)
// ============================================================================

/**
 * @brief Master-side LightFX serial communication (binary COBS protocol)
 * 
 * Used by HubFX to send commands to LightFX slave boards over USB.
 * Extends SerialBus with LightFX-specific commands.
 */
class LightFxMaster : public SerialBus {
public:
    LightFxMaster() = default;
    ~LightFxMaster() = default;

    LightFxMaster(const LightFxMaster&) = delete;
    LightFxMaster& operator=(const LightFxMaster&) = delete;

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

    // ========================================================================
    // Servo Control
    // ========================================================================
    
    bool servoSet(uint8_t id, int16_t pulseUs);
    bool servoSettings(uint8_t id, uint16_t minUs, uint16_t maxUs,
                       uint16_t speed, uint16_t accel, uint16_t decel);

    // ========================================================================
    // Power Monitor
    // ========================================================================
    
    bool powerStatus();

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
    void onPowerStatus(LightFxPowerStatusCallback cb) { _powerStatusCallback = cb; }
    void onError(LightFxErrorCallback cb) { _errorCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================
    
    bool isSlaveReady() const { return _slaveReady; }
    const char* slaveName() const { return _slaveName; }
    const LightFxBoardInfo& boardInfo() const { return _boardInfo; }
    bool lastCommandSuccess() const { return _lastAckReceived; }

private:
    void handlePacket(uint8_t type, const uint8_t* payload, size_t len);
    bool sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len);
    bool waitForAckNack();

    UsbHost* _usbHostRef = nullptr;
    bool _slaveReady = false;
    char _slaveName[32] = "";
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
    LightFxPowerStatusCallback _powerStatusCallback;
    LightFxErrorCallback _errorCallback;
};

// ============================================================================
// LightFxSlave Class (Binary Protocol)
// ============================================================================

/**
 * @brief Slave-side LightFX serial communication (binary COBS protocol)
 * 
 * Used by LightFX Pico to receive commands from HubFX master.
 * Implements ICommandHandler for use with CommandRouter.
 */
class LightFxSlave : public ICommandHandler {
public:
    LightFxSlave() = default;
    ~LightFxSlave() override = default;

    // ========================================================================
    // Initialization
    // ========================================================================
    
    bool begin(Stream* serial);
    void end();

    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================
    
    CommandHandleResult tryProcess(uint8_t type, const uint8_t* payload, size_t len) override;
    const char* handlerName() const override { return "LightFxSlave"; }

    // ========================================================================
    // Response Methods
    // ========================================================================
    
    int sendAck();
    int sendNack(uint8_t errorCode);
    int sendSeqStatus(const LightFxSeqStatus& status);
    int sendChannelStatus(const LightFxChannelStatus* channels, uint8_t count);
    int sendPowerStatus(const LightFxPowerStatus& status);

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
    void onLedStatus(LedChannelStatusCallback cb) { _ledStatusCallback = cb; }

    // ========================================================================
    // Servo Callbacks
    // ========================================================================
    
    void onServoSet(ServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(ServoSettingsCallback cb) { _servoSettingsCallback = cb; }

    // ========================================================================
    // Power Callbacks
    // ========================================================================
    
    void onPowerStatus(PowerStatusCallback cb) { _powerStatusCallback = cb; }

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
    LedChannelStatusCallback _ledStatusCallback;
    ServoSetCallback _servoSetCallback;
    ServoSettingsCallback _servoSettingsCallback;
    PowerStatusCallback _powerStatusCallback;
};

#endif // SERIAL_LIGHTFX_H
