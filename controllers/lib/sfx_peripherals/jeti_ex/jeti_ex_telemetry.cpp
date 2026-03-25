/*
 * JetiExTelemetry — implementation
 *
 * Conversion table (sensor type → Jeti EX encoding):
 *
 *   Type         | Unit  | ExType | dp | Divisor | Input example → Display
 *   -------------|-------|--------|----|---------|-----------------------
 *   Voltage      | V     | Int14  | 2  | 10      | 12340 mV → 1234 → 12.34V
 *   Current      | A     | Int14  | 2  | 10      | 1560 mA → 156 → 1.56A
 *   Power        | W     | Int22  | 1  | 100     | 19250 mW → 192 → 19.2W
 *   Temperature  | °C    | Int14  | 1  | 1       | 234 dC/10 → 234 → 23.4°C
 *   RPM          | rpm   | Int22  | 0  | 1       | 1350 → 1350rpm
 *   Percent      | %     | Int14  | 0  | 1       | 72 → 72%
 *   Time         | s     | Int22  | 1  | 100     | 12345 ms → 123 → 12.3s
 *   EngineState  | (none)| Int6   | 0  | 1       | 1 → 1 (on)
 *   GearState    | (none)| Int6   | 0  | 1       | 2 → 2 (deployed)
 *   GunState     | (none)| Int6   | 0  | 1       | 1 → 1 (firing)
 *   AudioChan.   | (none)| Int6   | 0  | 1       | 3 → 3 (channels)
 *   SmokeState   | (none)| Int6   | 0  | 1       | 1 → 1 (on)
 *   LightState   | (none)| Int6   | 0  | 1       | 1 → 1 (on)
 *   BoardState   | (none)| Int6   | 0  | 1       | 1 → 1 (online)
 */

#include "jeti_ex_telemetry.h"

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <string.h>

using namespace JetiEx;

// ═══════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════

JetiExTelemetry::JetiExTelemetry(JetiExBus& bus) : _bus(bus) {}

// ═══════════════════════════════════════════════════════════════
//  Device info
// ═══════════════════════════════════════════════════════════════

void JetiExTelemetry::setDeviceInfo(uint16_t manufacturerId, uint16_t deviceId,
                                     const char* name)
{
    _bus.setSensorInfo(manufacturerId, deviceId, name);
}

// ═══════════════════════════════════════════════════════════════
//  Internal: sensor registration
// ═══════════════════════════════════════════════════════════════

uint8_t JetiExTelemetry::registerSensor(JetiSensorType type, const char* label,
                                         const char* unit, ExDataType exType,
                                         uint8_t dp, int32_t divisor)
{
    if (_nextSensorId >= MAX_SENSOR_VALUES) return INVALID_ID;
    uint8_t id = _nextSensorId++;
    return registerSensorWithId(id, type, label, unit, exType, dp, divisor);
}

uint8_t JetiExTelemetry::registerSensorWithId(uint8_t id, JetiSensorType type,
                                               const char* label, const char* unit,
                                               ExDataType exType, uint8_t dp,
                                               int32_t divisor)
{
    if (_metaCount >= MAX_SENSOR_VALUES) return INVALID_ID;

    if (!_bus.addSensorValue(id, label, unit, exType, dp)) {
        return INVALID_ID;
    }

    auto& m = _meta[_metaCount++];
    m.sensorId   = id;
    m.type       = type;
    m.divisor    = (divisor > 0) ? divisor : 1;
    m.lastRaw    = 0;
    m.alarmLow   = 0;
    m.alarmHigh  = 0;
    m.alarmLowEnabled  = false;
    m.alarmHighEnabled = false;
    m.alarmTriggered   = false;
    m.alarmText  = nullptr;

    return id;
}

// ═══════════════════════════════════════════════════════════════
//  Typed sensor registration
// ═══════════════════════════════════════════════════════════════

uint8_t JetiExTelemetry::addVoltageSensor(const char* label)
{
    // mV → centi-V (÷10), displayed as X.XX V (dp=2)
    return registerSensor(JetiSensorType::Voltage, label, "V",
                          ExDataType::Int14, 2, 10);
}

uint8_t JetiExTelemetry::addCurrentSensor(const char* label)
{
    // mA → centi-A (÷10), displayed as X.XX A (dp=2)
    return registerSensor(JetiSensorType::Current, label, "A",
                          ExDataType::Int14, 2, 10);
}

uint8_t JetiExTelemetry::addPowerSensor(const char* label)
{
    // mW → deci-W (÷100), displayed as XX.X W (dp=1)
    return registerSensor(JetiSensorType::Power, label, "W",
                          ExDataType::Int22, 1, 100);
}

uint8_t JetiExTelemetry::addTemperatureSensor(const char* label)
{
    // deci-°C → deci-°C (÷1), displayed as XX.X °C (dp=1)
    return registerSensor(JetiSensorType::Temperature, label, "\xB0""C",
                          ExDataType::Int14, 1, 1);
}

uint8_t JetiExTelemetry::addRpmSensor(const char* label)
{
    // RPM → RPM (÷1), displayed as XXXXX rpm (dp=0)
    return registerSensor(JetiSensorType::Rpm, label, "rpm",
                          ExDataType::Int22, 0, 1);
}

uint8_t JetiExTelemetry::addPercentSensor(const char* label, const char* unit)
{
    // % → % (÷1), displayed as XXX % (dp=0)
    return registerSensor(JetiSensorType::Percent, label, unit,
                          ExDataType::Int14, 0, 1);
}

uint8_t JetiExTelemetry::addTimeSensor(const char* label)
{
    // ms → deci-s (÷100), displayed as XXXX.X s (dp=1)
    return registerSensor(JetiSensorType::Time, label, "s",
                          ExDataType::Int22, 1, 100);
}

uint8_t JetiExTelemetry::addEngineStateSensor(const char* label)
{
    return registerSensor(JetiSensorType::EngineState, label, "",
                          ExDataType::Int6, 0, 1);
}

uint8_t JetiExTelemetry::addGearStateSensor(const char* label)
{
    return registerSensor(JetiSensorType::GearState, label, "",
                          ExDataType::Int6, 0, 1);
}

uint8_t JetiExTelemetry::addGunStateSensor(const char* label)
{
    return registerSensor(JetiSensorType::GunState, label, "",
                          ExDataType::Int6, 0, 1);
}

uint8_t JetiExTelemetry::addAudioChannelsSensor(const char* label)
{
    return registerSensor(JetiSensorType::AudioChannels, label, "",
                          ExDataType::Int6, 0, 1);
}

uint8_t JetiExTelemetry::addSmokeStateSensor(const char* label)
{
    return registerSensor(JetiSensorType::SmokeState, label, "",
                          ExDataType::Int6, 0, 1);
}

uint8_t JetiExTelemetry::addLightStateSensor(const char* label)
{
    return registerSensor(JetiSensorType::LightState, label, "",
                          ExDataType::Int6, 0, 1);
}

uint8_t JetiExTelemetry::addBoardStateSensor(const char* label)
{
    return registerSensor(JetiSensorType::BoardState, label, "",
                          ExDataType::Int6, 0, 1);
}

uint8_t JetiExTelemetry::addCustomSensor(const char* label, const char* unit,
                                          ExDataType type, uint8_t dp,
                                          int32_t divisor)
{
    return registerSensor(JetiSensorType::Custom, label, unit, type, dp, divisor);
}

uint8_t JetiExTelemetry::addCustomSensorWithId(uint8_t id, const char* label,
                                                const char* unit, ExDataType type,
                                                uint8_t dp, int32_t divisor)
{
    return registerSensorWithId(id, JetiSensorType::Custom, label, unit,
                                type, dp, divisor);
}

// ═══════════════════════════════════════════════════════════════
//  Internal: value conversion
// ═══════════════════════════════════════════════════════════════

void JetiExTelemetry::setValueRaw(uint8_t sensorId, int32_t rawInput)
{
    auto* m = findMeta(sensorId);
    if (!m) return;

    m->lastRaw = rawInput;

    // Convert: display_value = raw_input / divisor
    int32_t displayVal = rawInput / m->divisor;
    _bus.setSensorValue(sensorId, displayVal);
}

// ═══════════════════════════════════════════════════════════════
//  Unit-safe value setters
// ═══════════════════════════════════════════════════════════════

void JetiExTelemetry::setVoltage_mV(uint8_t sensorId, int32_t millivolts)
{
    setValueRaw(sensorId, millivolts);
}

void JetiExTelemetry::setCurrent_mA(uint8_t sensorId, int32_t milliamps)
{
    setValueRaw(sensorId, milliamps);
}

void JetiExTelemetry::setPower_mW(uint8_t sensorId, int32_t milliwatts)
{
    setValueRaw(sensorId, milliwatts);
}

void JetiExTelemetry::setTemperature_C10(uint8_t sensorId, int32_t deciCelsius)
{
    setValueRaw(sensorId, deciCelsius);
}

void JetiExTelemetry::setRpm(uint8_t sensorId, int32_t rpm)
{
    setValueRaw(sensorId, rpm);
}

void JetiExTelemetry::setPercent(uint8_t sensorId, int32_t percent)
{
    setValueRaw(sensorId, percent);
}

void JetiExTelemetry::setTime_ms(uint8_t sensorId, uint32_t milliseconds)
{
    setValueRaw(sensorId, static_cast<int32_t>(milliseconds));
}

void JetiExTelemetry::setEngineState(uint8_t sensorId, bool running)
{
    setValueRaw(sensorId, running ? 1 : 0);
}

void JetiExTelemetry::setGearState(uint8_t sensorId, uint8_t position)
{
    setValueRaw(sensorId, static_cast<int32_t>(position));
}

void JetiExTelemetry::setGunState(uint8_t sensorId, bool firing)
{
    setValueRaw(sensorId, firing ? 1 : 0);
}

void JetiExTelemetry::setAudioChannels(uint8_t sensorId, uint8_t channels)
{
    setValueRaw(sensorId, static_cast<int32_t>(channels));
}

void JetiExTelemetry::setSmokeState(uint8_t sensorId, bool on)
{
    setValueRaw(sensorId, on ? 1 : 0);
}

void JetiExTelemetry::setLightState(uint8_t sensorId, bool on)
{
    setValueRaw(sensorId, on ? 1 : 0);
}

void JetiExTelemetry::setBoardState(uint8_t sensorId, uint8_t state)
{
    setValueRaw(sensorId, static_cast<int32_t>(state));
}

void JetiExTelemetry::setRaw(uint8_t sensorId, int32_t value)
{
    auto* m = findMeta(sensorId);
    if (m) m->lastRaw = value;
    _bus.setSensorValue(sensorId, value);
}

// ═══════════════════════════════════════════════════════════════
//  Alarm thresholds
// ═══════════════════════════════════════════════════════════════

void JetiExTelemetry::setAlarmLow(uint8_t sensorId, int32_t threshold)
{
    auto* m = findMeta(sensorId);
    if (!m) return;
    m->alarmLow = threshold;
    m->alarmLowEnabled = true;
}

void JetiExTelemetry::setAlarmHigh(uint8_t sensorId, int32_t threshold)
{
    auto* m = findMeta(sensorId);
    if (!m) return;
    m->alarmHigh = threshold;
    m->alarmHighEnabled = true;
}

void JetiExTelemetry::clearAlarmThresholds(uint8_t sensorId)
{
    auto* m = findMeta(sensorId);
    if (!m) return;
    m->alarmLowEnabled  = false;
    m->alarmHighEnabled = false;
    m->alarmTriggered   = false;
    m->alarmText        = nullptr;
}

void JetiExTelemetry::setAlarmText(uint8_t sensorId, const char* text)
{
    auto* m = findMeta(sensorId);
    if (m) m->alarmText = text;
}

void JetiExTelemetry::checkAlarms()
{
    bool anyActive = false;
    const char* firstAlarmText = nullptr;
    char firstAlarmChar = 'A';

    for (uint8_t i = 0; i < _metaCount; i++) {
        auto& m = _meta[i];
        bool wasTriggered = m.alarmTriggered;
        bool nowTriggered = false;
        bool isLow = false;

        if (m.alarmLowEnabled && m.lastRaw < m.alarmLow) {
            nowTriggered = true;
            isLow = true;
        }
        if (m.alarmHighEnabled && m.lastRaw > m.alarmHigh) {
            nowTriggered = true;
            isLow = false;
        }

        if (nowTriggered != wasTriggered) {
            m.alarmTriggered = nowTriggered;
            if (_alarmCb) {
                int32_t threshold = isLow ? m.alarmLow : m.alarmHigh;
                _alarmCb(m.sensorId, nowTriggered, isLow, m.lastRaw, threshold);
            }
        }

        if (nowTriggered) {
            anyActive = true;
            if (!firstAlarmText) {
                firstAlarmText = m.alarmText;
                // Use sensor ID offset as alarm character: 'A', 'B', 'C', ...
                firstAlarmChar = static_cast<char>('A' + (m.sensorId < 26 ? m.sensorId : 0));
            }
        }
    }

    // Update bus alarm state
    if (anyActive && firstAlarmText) {
        _bus.setAlarm(firstAlarmChar, firstAlarmText);
    } else if (anyActive) {
        _bus.setAlarm('!', "ALARM");
    } else {
        _bus.clearAlarm();
    }
}

bool JetiExTelemetry::anyAlarmActive() const
{
    for (uint8_t i = 0; i < _metaCount; i++) {
        if (_meta[i].alarmTriggered) return true;
    }
    return false;
}

bool JetiExTelemetry::isAlarming(uint8_t sensorId) const
{
    const auto* m = findMeta(sensorId);
    return m && m->alarmTriggered;
}

// ═══════════════════════════════════════════════════════════════
//  Config parameter presets
// ═══════════════════════════════════════════════════════════════

uint8_t JetiExTelemetry::addVolumeParam(const char* label, int32_t defaultVal)
{
    return addRangeParam(label, 0, 100, defaultVal);
}

uint8_t JetiExTelemetry::addBrightnessParam(const char* label, int32_t defaultVal)
{
    return addRangeParam(label, 0, 100, defaultVal);
}

uint8_t JetiExTelemetry::addGainParam(const char* label, int32_t defaultVal)
{
    return addRangeParam(label, 0, 100, defaultVal);
}

uint8_t JetiExTelemetry::addToggleParam(const char* label, bool defaultVal)
{
    uint8_t id = _nextParamId;
    if (!_bus.addParam(id, label, ParamType::Bool, 0, 1, defaultVal ? 1 : 0)) {
        return INVALID_ID;
    }
    _nextParamId++;
    return id;
}

uint8_t JetiExTelemetry::addModeParam(const char* label, uint8_t modeCount,
                                       int32_t defaultVal)
{
    int32_t maxMode = (modeCount > 1) ? (modeCount - 1) : 0;
    uint8_t id = _nextParamId;
    if (!_bus.addParam(id, label, ParamType::List, 0, maxMode, defaultVal)) {
        return INVALID_ID;
    }
    _nextParamId++;
    return id;
}

uint8_t JetiExTelemetry::addTrimParam(const char* label, int32_t defaultVal)
{
    return addRangeParam(label, -100, 100, defaultVal);
}

uint8_t JetiExTelemetry::addRangeParam(const char* label, int32_t minVal,
                                        int32_t maxVal, int32_t defaultVal)
{
    uint8_t id = _nextParamId;
    // Choose type based on range
    ParamType ptype = ParamType::Int8;
    if (minVal < -128 || maxVal > 127) ptype = ParamType::Int16;

    if (!_bus.addParam(id, label, ptype, minVal, maxVal, defaultVal)) {
        return INVALID_ID;
    }
    _nextParamId++;
    return id;
}

void JetiExTelemetry::onParamChange(JetiExBus::ParamChangeCallback cb)
{
    _bus.onParamChange(cb);
}

int32_t JetiExTelemetry::paramValue(uint8_t paramId) const
{
    return _bus.getParamValue(paramId);
}

void JetiExTelemetry::setParamValue(uint8_t paramId, int32_t value)
{
    _bus.setParamValue(paramId, value);
}

// ═══════════════════════════════════════════════════════════════
//  Internal: metadata lookup
// ═══════════════════════════════════════════════════════════════

JetiSensorMeta* JetiExTelemetry::findMeta(uint8_t sensorId)
{
    for (uint8_t i = 0; i < _metaCount; i++) {
        if (_meta[i].sensorId == sensorId) return &_meta[i];
    }
    return nullptr;
}

const JetiSensorMeta* JetiExTelemetry::findMeta(uint8_t sensorId) const
{
    for (uint8_t i = 0; i < _metaCount; i++) {
        if (_meta[i].sensorId == sensorId) return &_meta[i];
    }
    return nullptr;
}

#endif // SFX_PLATFORM_ESP32
