/*
 * JetiExBus — implementation
 *
 * EX Bus frame from receiver (master → device):
 *   [0x3E/0x3D] [type 0x01/0x03] [totalLen] [pktId] [dataId] [subLen] [payload...] [crc16LE]
 *   (byte 1 = packet type — was missing in the original layout, which broke
 *    every frame; verified against live receiver frames 2026-05-29.)
 *
 * Our response (device → receiver):
 *   [0x3B] [totalLen] [pktId] [dataId] [subLen] [EX data...] [crc16LE]
 *
 * EX telemetry data (within response payload):
 *   [0x7E sep] [typeLen] [mfr16LE] [dev16LE] [sensor values...] [crc8]
 */

#include "jeti_ex_bus.h"

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <string.h>

using namespace JetiEx;

// ─── begin ─────────────────────────────────────────────────────
bool JetiExBus::begin(Stream* serial)
{
    if (!serial) return false;
    end();
    _serial = serial;
    return true;
}

// ─── end ───────────────────────────────────────────────────────
void JetiExBus::end()
{
    _serial       = nullptr;
    _parseState   = IDLE;
    _frameIdx     = 0;
    _channelCount = 0;
    _lastChannelMs = 0;
    _sensorCount  = 0;
    _paramCount   = 0;
    _frameCount   = 0;
    _errorCount   = 0;
    _txCount      = 0;
    _rxByteCount  = 0;
    _telemetryCounter = 0;
    _nextSensorDataIdx = 0;
    _nextSensorTextIdx = 0;
    _deviceNameSent = false;
    _alarmMessage = nullptr;
    memset(_channels_us, 0, sizeof(_channels_us));
}

// ─── update ────────────────────────────────────────────────────
void JetiExBus::update()
{
    if (!_serial) return;

    while (_serial->available()) {
        uint8_t b = static_cast<uint8_t>(_serial->read());
        _rxByteCount++;

        switch (_parseState) {
        case IDLE:
            if (b == START_ADDR0 || b == START_ADDR1) {
                _frameBuf[0] = b;
                _frameIdx = 1;
                _parseState = READ_TYPE;
            }
            break;

        case READ_TYPE:
            // Byte 1 = packet type: 0x01 (response-allowed) / 0x03 (data-only).
            // This byte was missing from the original layout, which made
            // READ_LENGTH read it (1 or 3) as the length and reject every
            // frame.  Verified against live frames on the input_monitor rig.
            _frameBuf[1] = b;
            _frameIdx = 2;
            _parseState = READ_LENGTH;
            break;

        case READ_LENGTH:
            // Byte 2 = total packet length (header + payload + CRC).
            _frameBuf[2] = b;
            _frameLen = b;
            _frameIdx = 3;
            if (_frameLen < MIN_FRAME_SIZE || _frameLen > MAX_FRAME_SIZE) {
                _errorCount++;
                _parseState = IDLE;
            } else {
                _parseState = READ_BODY;
            }
            break;

        case READ_BODY:
            if (_frameIdx < MAX_FRAME_SIZE) {
                _frameBuf[_frameIdx] = b;
            }
            _frameIdx++;
            if (_frameIdx >= _frameLen) {
                processFrame();
                _parseState = IDLE;
            }
            break;
        }
    }
}

// ─── processFrame ──────────────────────────────────────────────
void JetiExBus::processFrame()
{
    if (_frameLen < MIN_FRAME_SIZE) {
        _errorCount++;
        return;
    }

    // Validate CRC-16/CCITT over bytes 0..len-3
    uint16_t rxCrc = _frameBuf[_frameLen - 2]
                   | (static_cast<uint16_t>(_frameBuf[_frameLen - 1]) << 8);
    uint16_t calcCrc = crc16_ccitt(_frameBuf, _frameLen - 2);
    if (rxCrc != calcCrc) {
        _errorCount++;
        return;
    }

    _frameCount++;
    // Frame layout: [0]header [1]type [2]len [3]pktId [4]dataId [5]subLen [6..]data [crc16].
    uint8_t packetId = _frameBuf[3];
    uint8_t dataId   = _frameBuf[4];

    switch (dataId) {
    case DATA_CHANNEL:
        parseChannelData();
        break;

    case DATA_TELEMETRY:
        handleTelemetryRequest(packetId);
        break;

    case DATA_JETIBOX:
        // JetiBox text menu — not implemented yet
        break;
    }
}

// ─── parseChannelData ──────────────────────────────────────────
void JetiExBus::parseChannelData()
{
    if (_frameLen < 10) return; // 6-byte header + ≥1 channel (2B) + 2B CRC

    uint8_t subLen = _frameBuf[5];          // data-block length (byte 5)
    uint8_t numCh  = subLen / 2;
    if (numCh > RxConfig::MAX_CHANNELS) numCh = RxConfig::MAX_CHANNELS;

    // Verify we have enough bytes for all channels (data starts at byte 6).
    if (6 + subLen > _frameLen - 2) {
        _errorCount++;
        return;
    }

    for (uint8_t i = 0; i < numCh; i++) {
        uint16_t raw = _frameBuf[6 + i * 2]
                     | (static_cast<uint16_t>(_frameBuf[7 + i * 2]) << 8);
        _channels_us[i] = JetiConfig::rawToUs(raw);
    }

    _channelCount  = numCh;
    _lastChannelMs = millis();
}

// ─── handleTelemetryRequest ────────────────────────────────────
void JetiExBus::handleTelemetryRequest(uint8_t packetId)
{
    if (_sensorCount == 0 && !_deviceName) return;

    _telemetryCounter++;

    // Priority 1: Send alarm message if active
    if (_alarmMessage) {
        sendAlarmResponse(packetId);
        return;
    }

    // Priority 2: Send device name text on first cycle
    if (!_deviceNameSent && _deviceName) {
        sendDeviceNameText(packetId);
        _deviceNameSent = true;
        return;
    }

    if (_sensorCount == 0) return;

    // Send text labels less frequently (every 5th request)
    if ((_telemetryCounter % 5) == 0) {
        sendTelemetryTextResponse(packetId);
    } else {
        sendTelemetryDataResponse(packetId);
    }
}

// ─── sendTelemetryDataResponse ─────────────────────────────────
void JetiExBus::sendTelemetryDataResponse(uint8_t packetId)
{
    // Find next active sensor value (round-robin)
    uint8_t attempts = 0;
    while (attempts < _sensorCount) {
        if (_nextSensorDataIdx >= _sensorCount) _nextSensorDataIdx = 0;
        if (_sensorValues[_nextSensorDataIdx].active) break;
        _nextSensorDataIdx++;
        attempts++;
    }
    if (attempts >= _sensorCount) return;

    const auto& sv = _sensorValues[_nextSensorDataIdx];
    _nextSensorDataIdx = (_nextSensorDataIdx + 1) % _sensorCount;

    // Build EX data payload:
    // [0x7E sep] [typeLen] [mfr16LE] [dev16LE] [encodedValue...] [crc8]
    uint8_t exBuf[16];
    uint8_t pos = 0;

    exBuf[pos++] = EX_SEPARATOR;

    // Type/length byte: bits 7-6 = 00 (data), bits 5-0 = data length (mfr+dev+value)
    uint8_t typeLenPos = pos++;  // placeholder, fill after encoding value

    exBuf[pos++] = _manufacturerId & 0xFF;
    exBuf[pos++] = (_manufacturerId >> 8) & 0xFF;
    exBuf[pos++] = _deviceId & 0xFF;
    exBuf[pos++] = (_deviceId >> 8) & 0xFF;

    // Encode sensor value
    uint8_t valLen = encodeSensorValue(&exBuf[pos], sv.id, sv.type, sv.value, sv.decimals);
    pos += valLen;

    // Fill type/length byte: type=00 (data), length = 4 (IDs) + valLen
    exBuf[typeLenPos] = 4 + valLen;  // data type (00 in upper bits)

    // CRC-8 over bytes [typeLenPos..pos-1] (excludes separator, includes type/len through value)
    exBuf[pos] = crc8_ex(&exBuf[typeLenPos], pos - typeLenPos);
    pos++;

    // Send as EX Bus response frame
    sendExBusResponse(packetId, DATA_TELEMETRY, exBuf, pos);
}

// ─── sendTelemetryTextResponse ─────────────────────────────────
void JetiExBus::sendTelemetryTextResponse(uint8_t packetId)
{
    // Find next sensor with a label (round-robin)
    uint8_t attempts = 0;
    while (attempts < _sensorCount) {
        if (_nextSensorTextIdx >= _sensorCount) _nextSensorTextIdx = 0;
        if (_sensorValues[_nextSensorTextIdx].active &&
            _sensorValues[_nextSensorTextIdx].label) break;
        _nextSensorTextIdx++;
        attempts++;
    }
    if (attempts >= _sensorCount) {
        // No text labels — send data instead
        sendTelemetryDataResponse(packetId);
        return;
    }

    const auto& sv = _sensorValues[_nextSensorTextIdx];
    _nextSensorTextIdx = (_nextSensorTextIdx + 1) % _sensorCount;

    // Build EX text payload:
    // [0x7E sep] [typeLen] [mfr16LE] [dev16LE] [valueId] [descLen] [label...unit\0] [crc8]
    uint8_t exBuf[40];
    uint8_t pos = 0;

    exBuf[pos++] = EX_SEPARATOR;
    uint8_t typeLenPos = pos++;  // placeholder

    exBuf[pos++] = _manufacturerId & 0xFF;
    exBuf[pos++] = (_manufacturerId >> 8) & 0xFF;
    exBuf[pos++] = _deviceId & 0xFF;
    exBuf[pos++] = (_deviceId >> 8) & 0xFF;

    // Value ID this text describes
    exBuf[pos++] = sv.id;

    // Description length (label + unit combined)
    uint8_t descLenPos = pos++;
    uint8_t textStart = pos;

    // Label text
    if (sv.label) {
        size_t labelLen = strlen(sv.label);
        if (labelLen > 20) labelLen = 20;
        memcpy(&exBuf[pos], sv.label, labelLen);
        pos += labelLen;
    }

    // Null separator between label and unit
    exBuf[pos++] = 0;

    // Unit text
    if (sv.unit) {
        size_t unitLen = strlen(sv.unit);
        if (unitLen > 5) unitLen = 5;
        memcpy(&exBuf[pos], sv.unit, unitLen);
        pos += unitLen;
    }

    exBuf[descLenPos] = pos - textStart;

    // Type/length byte: type=01 (text), length = 4 + text portion
    exBuf[typeLenPos] = 0x40 | (pos - typeLenPos - 1);  // upper 2 bits = 01 (text)

    // CRC-8
    exBuf[pos] = crc8_ex(&exBuf[typeLenPos], pos - typeLenPos);
    pos++;

    sendExBusResponse(packetId, DATA_TELEMETRY, exBuf, pos);
}
// ─── sendDeviceNameText ──────────────────────────────────────────
void JetiExBus::sendDeviceNameText(uint8_t packetId)
{
    if (!_deviceName) return;

    // EX text for ID 0 = device name (shown in transmitter device list)
    // [0x7E sep][typeLen][mfr16LE][dev16LE][valueId=0][descLen][name\0][crc8]
    uint8_t exBuf[40];
    uint8_t pos = 0;

    exBuf[pos++] = EX_SEPARATOR;
    uint8_t typeLenPos = pos++;

    exBuf[pos++] = _manufacturerId & 0xFF;
    exBuf[pos++] = (_manufacturerId >> 8) & 0xFF;
    exBuf[pos++] = _deviceId & 0xFF;
    exBuf[pos++] = (_deviceId >> 8) & 0xFF;

    exBuf[pos++] = 0;  // Value ID 0 = device name

    uint8_t descLenPos = pos++;
    uint8_t textStart = pos;

    size_t nameLen = strlen(_deviceName);
    if (nameLen > 20) nameLen = 20;
    memcpy(&exBuf[pos], _deviceName, nameLen);
    pos += nameLen;

    exBuf[descLenPos] = pos - textStart;

    // Type/length: bits 7-6 = 01 (text)
    exBuf[typeLenPos] = 0x40 | (pos - typeLenPos - 1);

    exBuf[pos] = crc8_ex(&exBuf[typeLenPos], pos - typeLenPos);
    pos++;

    sendExBusResponse(packetId, DATA_TELEMETRY, exBuf, pos);
}

// ─── sendAlarmResponse ───────────────────────────────────────────
void JetiExBus::sendAlarmResponse(uint8_t packetId)
{
    if (!_alarmMessage) return;

    // EX message/alarm (type bits 7-6 = 10):
    // [0x7E sep][typeLen][mfr16LE][dev16LE][alarmChar][message...][crc8]
    uint8_t exBuf[32];
    uint8_t pos = 0;

    exBuf[pos++] = EX_SEPARATOR;
    uint8_t typeLenPos = pos++;

    exBuf[pos++] = _manufacturerId & 0xFF;
    exBuf[pos++] = (_manufacturerId >> 8) & 0xFF;
    exBuf[pos++] = _deviceId & 0xFF;
    exBuf[pos++] = (_deviceId >> 8) & 0xFF;

    exBuf[pos++] = static_cast<uint8_t>(_alarmChar);

    size_t msgLen = strlen(_alarmMessage);
    if (msgLen > 16) msgLen = 16;
    memcpy(&exBuf[pos], _alarmMessage, msgLen);
    pos += msgLen;

    // Type/length: bits 7-6 = 10 (message/alarm)
    exBuf[typeLenPos] = 0x80 | (pos - typeLenPos - 1);

    exBuf[pos] = crc8_ex(&exBuf[typeLenPos], pos - typeLenPos);
    pos++;

    sendExBusResponse(packetId, DATA_TELEMETRY, exBuf, pos);
}

// ─── setAlarm / clearAlarm ───────────────────────────────────────
void JetiExBus::setAlarm(char alarmChar, const char* message)
{
    _alarmChar = alarmChar;
    _alarmMessage = (message && message[0]) ? message : nullptr;
}

void JetiExBus::clearAlarm()
{
    _alarmMessage = nullptr;
}
// ─── sendExBusResponse ─────────────────────────────────────────
void JetiExBus::sendExBusResponse(uint8_t packetId, uint8_t dataId,
                                   const uint8_t* payload, uint8_t payloadLen)
{
    if (!_serial) return;

    // Frame: [0x3B] [totalLen] [pktId] [dataId] [subLen] [payload...] [crc16LE]
    uint8_t frame[56];
    uint8_t totalLen = 5 + payloadLen + 2;  // header(5) + payload + CRC(2)

    frame[0] = RESPONSE_HEADER;
    frame[1] = totalLen;
    frame[2] = packetId;
    frame[3] = dataId;
    frame[4] = payloadLen;
    if (payloadLen > 0) {
        memcpy(&frame[5], payload, payloadLen);
    }

    // CRC-16 over bytes 0 to totalLen-3
    uint16_t crc = crc16_ccitt(frame, totalLen - 2);
    frame[totalLen - 2] = crc & 0xFF;
    frame[totalLen - 1] = (crc >> 8) & 0xFF;

    _serial->write(frame, totalLen);
    _txCount++;
}

// ─── channel_us ────────────────────────────────────────────────
uint16_t JetiExBus::channel_us(uint8_t ch) const
{
    if (ch < 1 || ch > _channelCount) return RxConfig::CENTER_US;
    return _channels_us[ch - 1];
}

// ─── isValid ───────────────────────────────────────────────────
bool JetiExBus::isValid() const
{
    if (_channelCount == 0 || _lastChannelMs == 0) return false;
    return (millis() - _lastChannelMs) < RxConfig::SIGNAL_TIMEOUT_MS;
}

// ─── setSensorInfo ─────────────────────────────────────────────
void JetiExBus::setSensorInfo(uint16_t manufacturerId, uint16_t deviceId, const char* name)
{
    _manufacturerId = manufacturerId;
    _deviceId       = deviceId;
    _deviceName     = name;
}

// ─── addSensorValue ────────────────────────────────────────────
bool JetiExBus::addSensorValue(uint8_t id, const char* label, const char* unit,
                                ExDataType type, uint8_t decimals)
{
    if (id >= MAX_SENSOR_VALUES || _sensorCount >= MAX_SENSOR_VALUES) return false;

    // Check for duplicate
    if (findSensor(id)) return false;

    auto& sv     = _sensorValues[_sensorCount++];
    sv.id        = id;
    sv.label     = label;
    sv.unit      = unit;
    sv.type      = type;
    sv.decimals  = (decimals > 3) ? 3 : decimals;
    sv.value     = 0;
    sv.active    = true;
    return true;
}

// ─── setSensorValue ────────────────────────────────────────────
void JetiExBus::setSensorValue(uint8_t id, int32_t value)
{
    auto* sv = findSensor(id);
    if (sv) sv->value = value;
}

// ─── addParam ──────────────────────────────────────────────────
bool JetiExBus::addParam(uint8_t id, const char* label, ParamType type,
                          int32_t minVal, int32_t maxVal, int32_t defaultVal)
{
    if (id >= MAX_CONFIG_PARAMS || _paramCount >= MAX_CONFIG_PARAMS) return false;
    if (findParam(id)) return false;

    auto& p       = _params[_paramCount++];
    p.id          = id;
    p.label       = label;
    p.type        = type;
    p.minVal      = minVal;
    p.maxVal      = maxVal;
    p.defaultVal  = defaultVal;
    p.value       = defaultVal;
    p.active      = true;
    return true;
}

// ─── setParamValue ─────────────────────────────────────────────
void JetiExBus::setParamValue(uint8_t id, int32_t value)
{
    auto* p = findParam(id);
    if (p) {
        if (value < p->minVal) value = p->minVal;
        if (value > p->maxVal) value = p->maxVal;
        p->value = value;
    }
}

// ─── getParamValue ─────────────────────────────────────────────
int32_t JetiExBus::getParamValue(uint8_t id) const
{
    const auto* p = findParam(id);
    return p ? p->value : 0;
}

// ─── findSensor ────────────────────────────────────────────────
JetiEx::SensorValue* JetiExBus::findSensor(uint8_t id)
{
    for (uint8_t i = 0; i < _sensorCount; i++) {
        if (_sensorValues[i].id == id) return &_sensorValues[i];
    }
    return nullptr;
}

const JetiEx::SensorValue* JetiExBus::findSensor(uint8_t id) const
{
    for (uint8_t i = 0; i < _sensorCount; i++) {
        if (_sensorValues[i].id == id) return &_sensorValues[i];
    }
    return nullptr;
}

// ─── findParam ─────────────────────────────────────────────────
JetiEx::ConfigParam* JetiExBus::findParam(uint8_t id)
{
    for (uint8_t i = 0; i < _paramCount; i++) {
        if (_params[i].id == id) return &_params[i];
    }
    return nullptr;
}

const JetiEx::ConfigParam* JetiExBus::findParam(uint8_t id) const
{
    for (uint8_t i = 0; i < _paramCount; i++) {
        if (_params[i].id == id) return &_params[i];
    }
    return nullptr;
}

#endif // SFX_PLATFORM_ESP32
