/*
 * JetiExTelemetry — Higher-level typed sensor and config API for ScaleFX
 *
 * Wraps a JetiExBus instance and provides:
 *   - Pre-built sensor type presets (voltage, current, power, temp, RPM, ...)
 *   - ScaleFX state sensors (engine, gear, gun, audio, smoke, lights, board status)
 *   - Unit-safe value setters following ScaleFX Rule 5 naming (_mV, _mA, ...)
 *   - Alarm thresholds with automatic triggering on the bus
 *   - Config parameter presets (volume, brightness, mode, toggle, trim, ...)
 *
 * The caller owns the JetiExBus and calls jeti.update() in the loop.
 * JetiExTelemetry only manages sensor registration, value conversion,
 * alarm checking, and provides convenience config methods.
 *
 * Usage (HubFX — board + effect state telemetry):
 *
 *   JetiExBus jeti;
 *   jeti.begin(&Serial2);
 *
 *   JetiExTelemetry telem(jeti);
 *   telem.setDeviceInfo(0xA4CB, 0x0001, "ScaleFX HubFX");
 *
 *   // Power monitoring (INA226):
 *   uint8_t vId = telem.addVoltageSensor("BusV");
 *
 *   // ScaleFX state sensors:
 *   uint8_t engId  = telem.addEngineStateSensor("Engine");
 *   uint8_t gearId = telem.addGearStateSensor("Gear");
 *   uint8_t gunId  = telem.addGunStateSensor("Gun");
 *   uint8_t audId  = telem.addAudioChannelsSensor("Audio");
 *   uint8_t smoId  = telem.addSmokeStateSensor("Smoke");
 *   uint8_t navId  = telem.addLightStateSensor("NavLts");
 *
 *   // Board connection tracking (one per slave):
 *   uint8_t gfxId  = telem.addBoardStateSensor("GunFX");
 *   uint8_t lfxId  = telem.addBoardStateSensor("LightFX");
 *   uint8_t gcId   = telem.addBoardStateSensor("GearCtl");
 *
 *   // Alarm: warn if battery drops below 10.5V
 *   telem.setAlarmLow(vId, 10500);  // 10500 mV
 *   telem.setAlarmText(vId, "LOW BATTERY");
 *
 *   // Config params (editable from transmitter):
 *   uint8_t volP  = telem.addVolumeParam();
 *   uint8_t brtP  = telem.addBrightnessParam();
 *   uint8_t modeP = telem.addModeParam("Effect", 4);
 *   telem.onParamChange([](uint8_t id, int32_t oldV, int32_t newV) { ... });
 *
 *   // In loop:
 *   jeti.update();
 *   telem.setVoltage_mV(vId, monitor.busVoltage_mV());
 *   telem.setEngineState(engId, engineRunning);
 *   telem.setGearState(gearId, gearPosition);  // 0=retracted,1=transit,2=deployed
 *   telem.setGunState(gunId, gunFiring);
 *   telem.setAudioChannels(audId, mixer.activeChannels());
 *   telem.setSmokeState(smoId, smokeOn);
 *   telem.setLightState(navId, navLightsOn);
 *   telem.setBoardState(gfxId, gunfxConnected ? 1 : 0);
 *   telem.setBoardState(lfxId, lightfxConnected ? 1 : 0);
 *   telem.setBoardState(gcId, gearConnected ? 1 : 0);
 *   telem.checkAlarms();
 */

#ifndef SFX_JETI_EX_TELEMETRY_H
#define SFX_JETI_EX_TELEMETRY_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include <functional>
#include "jeti_ex_bus.h"
#include "jeti_ex_common.h"

// ════════════════════════════════════════════════════════════════
//  Sensor type preset enumeration
// ════════════════════════════════════════════════════════════════

enum class JetiSensorType : uint8_t {
    Voltage,        ///< V, dp=2, Int14 — input: millivolts
    Current,        ///< A, dp=2, Int14 — input: milliamps
    Power,          ///< W, dp=1, Int22 — input: milliwatts
    Temperature,    ///< °C, dp=1, Int14 — input: deci-Celsius (23.4°C → 234)
    Rpm,            ///< rpm, dp=0, Int22 — input: RPM
    Percent,        ///< %, dp=0, Int14 — input: integer percent
    Time,           ///< s, dp=1, Int22 — input: milliseconds
    EngineState,    ///< on/off (0/1), Int6, dp=0
    GearState,      ///< position (0=retracted,1=transit,2=deployed,3=error), Int6, dp=0
    GunState,       ///< trigger (0=idle,1=firing), Int6, dp=0
    AudioChannels,  ///< active channels (0–8), Int6, dp=0
    SmokeState,     ///< on/off (0/1), Int6, dp=0
    LightState,     ///< on/off (0/1), Int6, dp=0
    BoardState,     ///< connection (0=offline,1=online,2=error), Int6, dp=0
    Custom,         ///< User-defined (no auto-conversion)
};

// ════════════════════════════════════════════════════════════════
//  Sensor metadata (tracks conversion + alarms)
// ════════════════════════════════════════════════════════════════

struct JetiSensorMeta {
    uint8_t        sensorId = 0xFF;    ///< Bus sensor ID (0xFF = unused)
    JetiSensorType type = JetiSensorType::Custom;
    int32_t        divisor = 1;        ///< raw input ÷ divisor = display value
    int32_t        lastRaw = 0;        ///< Last raw value in input units
    int32_t        alarmLow = 0;       ///< Low threshold in input units
    int32_t        alarmHigh = 0;      ///< High threshold in input units
    bool           alarmLowEnabled = false;
    bool           alarmHighEnabled = false;
    bool           alarmTriggered = false; ///< Currently in alarm state
    const char*    alarmText = nullptr;    ///< Custom alarm text (or auto-generated)
};

// ════════════════════════════════════════════════════════════════
//  JetiExTelemetry class
// ════════════════════════════════════════════════════════════════

class JetiExTelemetry {
public:
    /**
     * @brief Construct with reference to an existing JetiExBus
     * @param bus  JetiExBus instance (must outlive this object)
     */
    explicit JetiExTelemetry(JetiExBus& bus);
    ~JetiExTelemetry() = default;

    JetiExTelemetry(const JetiExTelemetry&) = delete;
    JetiExTelemetry& operator=(const JetiExTelemetry&) = delete;

    // ════════════════════════════════════════════════════════════
    //  Device identification
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Set device identification (delegates to bus)
     * @param manufacturerId  Jeti manufacturer ID
     * @param deviceId        Unique device ID
     * @param name            Device name (max 20 chars, shown on TX)
     */
    void setDeviceInfo(uint16_t manufacturerId, uint16_t deviceId,
                       const char* name = nullptr);

    // ════════════════════════════════════════════════════════════
    //  Typed sensor registration
    //
    //  Each method returns the sensor ID (0–14), or INVALID_ID on failure.
    //  IDs are auto-assigned starting from 1 (ID 0 = device name).
    // ════════════════════════════════════════════════════════════

    static constexpr uint8_t INVALID_ID = 0xFF;

    /** @brief Voltage sensor — input: millivolts, display: X.XX V */
    uint8_t addVoltageSensor(const char* label = "Voltage");

    /** @brief Current sensor — input: milliamps, display: X.XX A */
    uint8_t addCurrentSensor(const char* label = "Current");

    /** @brief Power sensor — input: milliwatts, display: XX.X W */
    uint8_t addPowerSensor(const char* label = "Power");

    /** @brief Temperature sensor — input: deci-Celsius (234 = 23.4°C), display: XX.X °C */
    uint8_t addTemperatureSensor(const char* label = "Temp");

    /** @brief RPM sensor — input: RPM, display: XXXXX rpm */
    uint8_t addRpmSensor(const char* label = "RPM");

    /** @brief Percentage sensor — input: integer percent, display: XXX % */
    uint8_t addPercentSensor(const char* label, const char* unit = "%");

    /** @brief Time sensor — input: milliseconds, display: XXXX.X s */
    uint8_t addTimeSensor(const char* label = "Time");

    // ════════════════════════════════════════════════════════════
    //  ScaleFX state sensors
    //
    //  Each reports a small integer status code displayed on the
    //  Jeti transmitter.  All use Int6 encoding (0–31).
    // ════════════════════════════════════════════════════════════

    /** @brief Engine running state — 0=off, 1=on */
    uint8_t addEngineStateSensor(const char* label = "Engine");

    /** @brief Gear position — 0=retracted, 1=transit, 2=deployed, 3=error */
    uint8_t addGearStateSensor(const char* label = "Gear");

    /** @brief Gun trigger state — 0=idle, 1=firing */
    uint8_t addGunStateSensor(const char* label = "Gun");

    /** @brief Active audio channel count (0–8) */
    uint8_t addAudioChannelsSensor(const char* label = "Audio");

    /** @brief Smoke generator state — 0=off, 1=on */
    uint8_t addSmokeStateSensor(const char* label = "Smoke");

    /** @brief Light state — 0=off, 1=on */
    uint8_t addLightStateSensor(const char* label = "Lights");

    /** @brief Board connection state — 0=offline, 1=online, 2=error */
    uint8_t addBoardStateSensor(const char* label);

    /**
     * @brief Custom sensor with explicit parameters
     * @param label    Display name (max 20 chars)
     * @param unit     Unit string (max 5 chars)
     * @param type     EX data type
     * @param dp       Decimal point position (0–3)
     * @param divisor  Input value ÷ divisor = display value (1 = no conversion)
     * @return Sensor ID or INVALID_ID
     */
    uint8_t addCustomSensor(const char* label, const char* unit,
                            JetiEx::ExDataType type, uint8_t dp,
                            int32_t divisor = 1);

    /**
     * @brief Custom sensor with explicit ID (for advanced use)
     * @param id       Specific sensor ID (0–14)
     * @param label    Display name
     * @param unit     Unit string
     * @param type     EX data type
     * @param dp       Decimal point position
     * @param divisor  Input value ÷ divisor = display value
     * @return Sensor ID or INVALID_ID
     */
    uint8_t addCustomSensorWithId(uint8_t id, const char* label, const char* unit,
                                  JetiEx::ExDataType type, uint8_t dp,
                                  int32_t divisor = 1);

    // ════════════════════════════════════════════════════════════
    //  Unit-safe value setters (ScaleFX Rule 5 naming)
    //
    //  Each setter takes the value in the sensor's native input unit,
    //  converts it to the display value, and updates the bus.
    // ════════════════════════════════════════════════════════════

    /** @brief Set voltage in millivolts (12340 mV → 12.34 V) */
    void setVoltage_mV(uint8_t sensorId, int32_t millivolts);

    /** @brief Set current in milliamps (1560 mA → 1.56 A) */
    void setCurrent_mA(uint8_t sensorId, int32_t milliamps);

    /** @brief Set power in milliwatts (19250 mW → 19.2 W) */
    void setPower_mW(uint8_t sensorId, int32_t milliwatts);

    /** @brief Set temperature in deci-Celsius (234 → 23.4 °C) */
    void setTemperature_C10(uint8_t sensorId, int32_t deciCelsius);

    /** @brief Set RPM directly (1350 → 1350 rpm) */
    void setRpm(uint8_t sensorId, int32_t rpm);

    /** @brief Set percentage (72 → 72 %) */
    void setPercent(uint8_t sensorId, int32_t percent);

    /** @brief Set time in milliseconds (12345 ms → 12.3 s) */
    void setTime_ms(uint8_t sensorId, uint32_t milliseconds);

    // ScaleFX state setters

    /** @brief Set engine running state (true=running, false=off) */
    void setEngineState(uint8_t sensorId, bool running);

    /** @brief Set gear position (0=retracted, 1=transit, 2=deployed, 3=error) */
    void setGearState(uint8_t sensorId, uint8_t position);

    /** @brief Set gun trigger state (true=firing, false=idle) */
    void setGunState(uint8_t sensorId, bool firing);

    /** @brief Set active audio channel count (0–8) */
    void setAudioChannels(uint8_t sensorId, uint8_t channels);

    /** @brief Set smoke generator state (true=on, false=off) */
    void setSmokeState(uint8_t sensorId, bool on);

    /** @brief Set light state (true=on, false=off) */
    void setLightState(uint8_t sensorId, bool on);

    /** @brief Set board connection state (0=offline, 1=online, 2=error) */
    void setBoardState(uint8_t sensorId, uint8_t state);

    /** @brief Set raw value directly (already in display units × 10^dp) */
    void setRaw(uint8_t sensorId, int32_t value);

    // ════════════════════════════════════════════════════════════
    //  Alarm thresholds
    //
    //  Thresholds are in the same units as the corresponding setter.
    //  Call checkAlarms() in your loop after updating values.
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Set low alarm threshold (triggers when value drops BELOW)
     * @param sensorId  Sensor ID returned by addXxxSensor()
     * @param threshold Threshold in sensor's input units (e.g., mV for voltage)
     */
    void setAlarmLow(uint8_t sensorId, int32_t threshold);

    /**
     * @brief Set high alarm threshold (triggers when value goes ABOVE)
     * @param sensorId  Sensor ID returned by addXxxSensor()
     * @param threshold Threshold in sensor's input units
     */
    void setAlarmHigh(uint8_t sensorId, int32_t threshold);

    /** @brief Clear all alarms for a sensor */
    void clearAlarmThresholds(uint8_t sensorId);

    /**
     * @brief Set custom alarm text for a sensor
     *
     * When this sensor triggers an alarm, this text is shown on the
     * transmitter instead of the auto-generated default.
     */
    void setAlarmText(uint8_t sensorId, const char* text);

    /**
     * @brief Check all sensor values against alarm thresholds
     *
     * Call this in your loop after updating sensor values.  If a
     * threshold is crossed, triggers an alarm message on the bus.
     * Alarms auto-clear when the value returns to normal range.
     */
    void checkAlarms();

    /**
     * @brief Alarm callback — notified when alarm state changes
     * @param cb  Function(sensorId, isTriggered, isLow, rawValue, threshold)
     */
    using AlarmCallback = std::function<void(uint8_t sensorId, bool triggered,
                                             bool isLow, int32_t value, int32_t threshold)>;
    void onAlarm(AlarmCallback cb) { _alarmCb = cb; }

    /** @brief Check if any sensor is currently alarming */
    bool anyAlarmActive() const;

    /** @brief Check if a specific sensor is currently alarming */
    bool isAlarming(uint8_t sensorId) const;

    // ════════════════════════════════════════════════════════════
    //  Config parameter presets
    //
    //  Each method returns the parameter ID, or INVALID_ID on failure.
    //  IDs are auto-assigned sequentially.
    // ════════════════════════════════════════════════════════════

    /** @brief Volume parameter (0–100, default 50) */
    uint8_t addVolumeParam(const char* label = "Volume", int32_t defaultVal = 50);

    /** @brief Brightness parameter (0–100, default 100) */
    uint8_t addBrightnessParam(const char* label = "Brightness", int32_t defaultVal = 100);

    /** @brief Gain/sensitivity parameter (0–100, default 50) */
    uint8_t addGainParam(const char* label = "Gain", int32_t defaultVal = 50);

    /** @brief Boolean toggle parameter (0 or 1) */
    uint8_t addToggleParam(const char* label, bool defaultVal = false);

    /** @brief Mode selection parameter (0 to modeCount-1) */
    uint8_t addModeParam(const char* label, uint8_t modeCount, int32_t defaultVal = 0);

    /** @brief Trim parameter (-100 to +100, default 0) */
    uint8_t addTrimParam(const char* label, int32_t defaultVal = 0);

    /**
     * @brief Custom range parameter
     * @param label  Display name
     * @param minVal Minimum value
     * @param maxVal Maximum value
     * @param defaultVal Default value
     * @return Parameter ID or INVALID_ID
     */
    uint8_t addRangeParam(const char* label, int32_t minVal, int32_t maxVal,
                          int32_t defaultVal);

    /** @brief Set parameter change callback (delegates to bus) */
    void onParamChange(JetiExBus::ParamChangeCallback cb);

    /** @brief Get current parameter value (delegates to bus) */
    int32_t paramValue(uint8_t paramId) const;

    /** @brief Set a parameter value programmatically (delegates to bus) */
    void setParamValue(uint8_t paramId, int32_t value);

    // ════════════════════════════════════════════════════════════
    //  Access
    // ════════════════════════════════════════════════════════════

    /** @brief Get the underlying bus reference */
    JetiExBus& bus() { return _bus; }
    const JetiExBus& bus() const { return _bus; }

    /** @brief Number of registered sensors */
    uint8_t sensorCount() const { return _metaCount; }

private:
    JetiExBus& _bus;

    JetiSensorMeta _meta[JetiEx::MAX_SENSOR_VALUES] = {};
    uint8_t _metaCount = 0;
    uint8_t _nextSensorId = 1;  ///< Auto-incrementing sensor ID (0 = device name)
    uint8_t _nextParamId = 0;   ///< Auto-incrementing param ID

    AlarmCallback _alarmCb;

    /// Internal: register a sensor with auto-ID assignment
    uint8_t registerSensor(JetiSensorType type, const char* label, const char* unit,
                           JetiEx::ExDataType exType, uint8_t dp, int32_t divisor);

    /// Internal: register a sensor with specific ID
    uint8_t registerSensorWithId(uint8_t id, JetiSensorType type, const char* label,
                                 const char* unit, JetiEx::ExDataType exType,
                                 uint8_t dp, int32_t divisor);

    /// Internal: set a sensor value in raw input units
    void setValueRaw(uint8_t sensorId, int32_t rawInput);

    /// Internal: find metadata by sensor ID
    JetiSensorMeta* findMeta(uint8_t sensorId);
    const JetiSensorMeta* findMeta(uint8_t sensorId) const;
};

#endif // SFX_PLATFORM_ESP32
#endif // SFX_JETI_EX_TELEMETRY_H
