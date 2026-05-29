/*
 * JetiExBus — Jeti EX Bus bidirectional protocol handler
 *
 * Full peripheral for the Jeti Duplex EX Bus protocol:
 *   - Channel data reception (16 proportional channels)
 *   - EX telemetry value reporting (register sensors, auto-respond)
 *   - Device configuration parameter read/write with change callbacks
 *
 * This is a SEPARATE PERIPHERAL from rx_input — not just a ChannelSource.
 * For channel-only access through RxInputs<>, use JetiExChannelSource adapter.
 *
 * The class uses Arduino Stream* for UART I/O.  The caller configures the
 * HardwareSerial with the correct baud rate before calling begin().
 *
 * Platform setup examples:
 *
 *   // ESP32 (two-wire, separate RX/TX):
 *   Serial2.begin(125000, SERIAL_8N1, RX_PIN, TX_PIN);
 *   JetiExBus jeti;
 *   jeti.begin(&Serial2);
 *
 *   // Pico:
 *   Serial1.setRX(RX_PIN);
 *   Serial1.setTX(TX_PIN);
 *   Serial1.begin(125000, SERIAL_8N1);
 *   JetiExBus jeti;
 *   jeti.begin(&Serial1);
 *
 * Usage:
 *
 *   // Setup:
 *   jeti.begin(&Serial2);
 *   jeti.setSensorInfo(0xA4CB, 0x0001, "ScaleFX");
 *   jeti.addSensorValue(0, "Voltage", "V", JetiEx::ExDataType::Int14, 2);
 *   jeti.addParam(0, "Volume", JetiEx::ParamType::Int8, 0, 100, 50);
 *   jeti.onParamChange([](uint8_t id, int32_t old, int32_t val) { ... });
 *
 *   // Loop:
 *   jeti.update();
 *   jeti.setSensorValue(0, 1234);  // 12.34V (dp=2)
 *   uint16_t throttle = jeti.channel_us(3);
 */

#ifndef SFX_JETI_EX_BUS_H
#define SFX_JETI_EX_BUS_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include <functional>
#include "../rx_input/rx_common.h"
#include "jeti_ex_common.h"

class JetiExBus {
public:
    JetiExBus() = default;
    ~JetiExBus() { end(); }

    JetiExBus(const JetiExBus&) = delete;
    JetiExBus& operator=(const JetiExBus&) = delete;

    // ════════════════════════════════════════════════════════════
    //  Lifecycle
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Attach to an already-configured serial port
     * @param serial  HardwareSerial configured at 125000 or 250000, 8N1
     * @return true if serial is non-null
     */
    bool begin(Stream* serial);

    /**
     * @brief Process incoming frames and send telemetry/config responses
     *
     * MUST be called every loop iteration.  Reads all available bytes,
     * parses frames, updates channel values, and auto-responds to
     * telemetry and config requests during the response window.
     */
    void update();

    /**
     * @brief Detach from serial (does NOT close the port)
     */
    void end();

    // ════════════════════════════════════════════════════════════
    //  Channel data (ChannelSource-compatible interface)
    // ════════════════════════════════════════════════════════════

    /** @brief Channel value in µs (1-based, 1..16) */
    uint16_t channel_us(uint8_t ch) const;

    /** @brief Number of channels decoded in last valid frame */
    uint8_t channelCount() const { return _channelCount; }

    /** @brief true if channel data received within SIGNAL_TIMEOUT_MS */
    bool isValid() const;

    /** @brief true after begin() */
    bool isInitialized() const { return _serial != nullptr; }

    // ════════════════════════════════════════════════════════════
    //  Telemetry sensor values
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Set device identification (shown on transmitter display)
     * @param manufacturerId  Jeti manufacturer ID (request from Jeti, or use test range)
     * @param deviceId        Unique device ID within manufacturer
     * @param name            Device name (max 20 chars, shown during discovery)
     */
    void setSensorInfo(uint16_t manufacturerId, uint16_t deviceId, const char* name = nullptr);

    /**
     * @brief Register a sensor value slot
     * @param id        Value ID (0–14)
     * @param label     Display name (max 20 chars)
     * @param unit      Unit string (max 5 chars, e.g. "V", "A", "°C")
     * @param type      Data type (Int6, Int14, Int22, Int30)
     * @param decimals  Decimal point position (0–3)
     * @return true if registered (false if id out of range or slot full)
     */
    bool addSensorValue(uint8_t id, const char* label, const char* unit,
                        JetiEx::ExDataType type, uint8_t decimals = 0);

    /**
     * @brief Update a sensor value (integer, scaled by 10^decimals)
     *
     * Example: to report 12.34V with decimals=2, pass value=1234.
     */
    void setSensorValue(uint8_t id, int32_t value);

    /** @brief Number of registered sensor values */
    uint8_t sensorValueCount() const { return _sensorCount; }

    // ════════════════════════════════════════════════════════════
    //  Configuration parameters
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Register a config parameter
     * @param id         Parameter ID (0–15)
     * @param label      Display name (max 16 chars)
     * @param type       Parameter type
     * @param minVal     Minimum value
     * @param maxVal     Maximum value
     * @param defaultVal Default value
     * @return true if registered
     */
    bool addParam(uint8_t id, const char* label, JetiEx::ParamType type,
                  int32_t minVal, int32_t maxVal, int32_t defaultVal);

    /** @brief Set a parameter value programmatically */
    void setParamValue(uint8_t id, int32_t value);

    /** @brief Get current parameter value */
    int32_t getParamValue(uint8_t id) const;

    /** @brief Number of registered parameters */
    uint8_t paramCount() const { return _paramCount; }

    /**
     * @brief Callback when a parameter is changed by the transmitter
     * @param cb  Function(paramId, oldValue, newValue)
     */
    using ParamChangeCallback = std::function<void(uint8_t paramId, int32_t oldValue, int32_t newValue)>;
    void onParamChange(ParamChangeCallback cb) { _paramChangeCb = cb; }

    // ════════════════════════════════════════════════════════════
    //  Diagnostics
    // ════════════════════════════════════════════════════════════

    // ════════════════════════════════════════════════════════════
    //  Alarm messages
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Queue an alarm message for transmitter display
     *
     * The alarm will be sent in the next telemetry response slot.
     * The transmitter plays an alarm tone and shows the message.
     * Call with nullptr/empty to clear the alarm.
     *
     * @param alarmChar  Single-character alarm identifier ('A'-'Z')
     * @param message    Alarm text (max 16 chars, shown on TX display)
     */
    void setAlarm(char alarmChar, const char* message);

    /** @brief Clear any active alarm */
    void clearAlarm();

    /** @brief True if an alarm message is queued */
    bool alarmActive() const { return _alarmMessage != nullptr; }

    // ════════════════════════════════════════════════════════════
    //  Diagnostics
    // ════════════════════════════════════════════════════════════

    uint32_t rxFrameCount() const { return _frameCount; }
    uint32_t rxErrorCount() const { return _errorCount; }
    uint32_t txResponseCount() const { return _txCount; }
    /// Total bytes seen on the UART. Diagnoses wrong-baud (bytes climb but
    /// frames/CRC stay at 0) vs no-wire (zero bytes) — see input_monitor rig.
    uint32_t rxByteCount() const { return _rxByteCount; }

private:
    Stream* _serial = nullptr;

    // ── Frame parser state ──────────────────────────────────────
    enum ParseState : uint8_t { IDLE, READ_TYPE, READ_LENGTH, READ_BODY };
    ParseState _parseState = IDLE;
    uint8_t    _frameBuf[JetiEx::MAX_FRAME_SIZE] = {};
    uint8_t    _frameIdx = 0;
    uint8_t    _frameLen = 0;

    // ── Channel data ────────────────────────────────────────────
    uint16_t _channels_us[RxConfig::MAX_CHANNELS] = {};
    uint8_t  _channelCount = 0;
    uint32_t _lastChannelMs = 0;

    // ── Telemetry ───────────────────────────────────────────────
    uint16_t _manufacturerId = 0;
    uint16_t _deviceId = 0;
    const char* _deviceName = nullptr;
    JetiEx::SensorValue _sensorValues[JetiEx::MAX_SENSOR_VALUES] = {};
    uint8_t _sensorCount = 0;
    uint8_t _nextSensorDataIdx = 0;     ///< Round-robin for data values
    uint8_t _nextSensorTextIdx = 0;     ///< Round-robin for text labels
    uint16_t _telemetryCounter = 0;     ///< Counts telemetry requests
    bool    _deviceNameSent = false;    ///< Device name text sent at least once

    // ── Alarm ────────────────────────────────────────────────────
    char        _alarmChar = 'A';
    const char* _alarmMessage = nullptr;

    // ── Config ──────────────────────────────────────────────────
    JetiEx::ConfigParam _params[JetiEx::MAX_CONFIG_PARAMS] = {};
    uint8_t _paramCount = 0;
    ParamChangeCallback _paramChangeCb;

    // ── Stats ───────────────────────────────────────────────────
    uint32_t _frameCount = 0;
    uint32_t _errorCount = 0;
    uint32_t _txCount = 0;
    uint32_t _rxByteCount = 0;

    // ── Internal methods ────────────────────────────────────────
    void processFrame();
    void parseChannelData();
    void handleTelemetryRequest(uint8_t packetId);
    void sendTelemetryDataResponse(uint8_t packetId);
    void sendTelemetryTextResponse(uint8_t packetId);
    void sendAlarmResponse(uint8_t packetId);
    void sendDeviceNameText(uint8_t packetId);
    void sendExBusResponse(uint8_t packetId, uint8_t dataId,
                           const uint8_t* payload, uint8_t payloadLen);

    /** @brief Find sensor value by ID, or nullptr */
    JetiEx::SensorValue* findSensor(uint8_t id);
    const JetiEx::SensorValue* findSensor(uint8_t id) const;

    /** @brief Find config param by ID, or nullptr */
    JetiEx::ConfigParam* findParam(uint8_t id);
    const JetiEx::ConfigParam* findParam(uint8_t id) const;
};

#endif // SFX_PLATFORM_ESP32
#endif // SFX_JETI_EX_BUS_H
