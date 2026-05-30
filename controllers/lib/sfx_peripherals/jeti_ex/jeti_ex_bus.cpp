/*
 * JetiExBus — implementation
 *
 * EX Bus frame from receiver (master → device):
 *   [0x3E/0x3D] [type 0x01/0x03] [totalLen] [pktId] [dataId] [subLen] [payload...] [crc16LE]
 *   (byte 1 = packet type — was missing in the original layout, which broke
 *    every frame; verified against live receiver frames 2026-05-29.)
 *
 * Our response (device → receiver), per docs/EX_Bus_protocol_v1.21_EN.pdf:
 *   [0x3B] [0x01] [totalLen] [pktId] [dataId] [subLen] [EX data...] [crc16LE]
 *   (the 0x01 second header byte was missing before — radio rejected every
 *    reply; the slave MUST echo the master's pktId.  Verified on a real radio.)
 *
 * EX telemetry data (within response payload):
 *   [0x9F sep] [type|len] [USN16LE] [LSN16LE] [reserved] [sensor entries...] [crc8]
 *   type bits 7-6: 0b01=data, 0b00=text, 0b10=alarm.  Value bytes little-endian.
 */

#include "jeti_ex_bus.h"

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <string.h>

using namespace JetiEx;

namespace {
// Lay down the shared 7-byte EX telemetry head:
//   [0] 0x9F sep · [1] type|len (caller fills) · [2,3] USN · [4,5] LSN · [6] reserved
// Returns the next write position (7).  USN+LSN must be identical across every
// message from this device so the radio groups them into one sensor source.
uint8_t writeExHead(uint8_t* b, uint16_t usn, uint16_t lsn) {
    b[0] = EX_SEPARATOR;
    // b[1] = type|len — filled by the caller after the body length is known.
    b[2] = usn & 0xFF;  b[3] = (usn >> 8) & 0xFF;
    b[4] = lsn & 0xFF;  b[5] = (lsn >> 8) & 0xFF;
    b[6] = 0x00;        // reserved
    return 7;
}
}  // namespace

// ─── begin ─────────────────────────────────────────────────────
bool JetiExBus::begin(Stream* serial)
{
    if (!serial) return false;
    end();
    _serial = serial;
    _parser.onFrame([this](const uint8_t* f, uint8_t l){ processFrame(f, l); });
    // Rx-side: only frame MASTER headers (0x3E/0x3D).  A 0x3B on this wire is
    // our own half-duplex echo — framing it would re-trigger a reply (feedback
    // loop) and inflate RX errors.
    _parser.setAcceptSlave(false);
    _parser.reset();
    return true;
}

// ─── end ───────────────────────────────────────────────────────
void JetiExBus::end()
{
    _serial       = nullptr;
    _parser.reset();
    _channelCount = 0;
    _lastChannelMs = 0;
    _sensorCount  = 0;
    _paramCount   = 0;
    _txCount      = 0;
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
    _parser.drain(_serial);     // shared parser → processFrame() per CRC-valid frame
}

// ─── processFrame ──────────────────────────────────────────────
// Called by the parser with a complete, CRC-validated frame.
void JetiExBus::processFrame(const uint8_t* frame, uint8_t len)
{
    // Forward the raw, CRC-valid master frame to any observer (the expander
    // mirrors it out to the downstream ESC so the ESC sees the Rx's polling).
    if (_onRawFrame) _onRawFrame(frame, len);

    // Frame layout: [0]header [1]type [2]len [3]pktId [4]dataId [5]subLen [6..]data [crc16].
    uint8_t packetId = frame[3];
    uint8_t dataId   = frame[4];

    switch (dataId) {
    case DATA_CHANNEL:
        parseChannelData(frame, len);
        break;

    case DATA_TELEMETRY:
        handleTelemetryRequest(packetId);
        break;

    case DATA_JETIBOX:
        // Rx-driven config (JetiBox / DeviceExplorer menu) — NOT IMPLEMENTED YET.
        // Deferred two-way config: HubFX's own structured DeviceExplorer config
        // is NDA-gated (needs a Jeti-signed device bin-file), and relaying the
        // downstream device's config means a synchronous Rx<->ESC proxy inside
        // the ~4 ms response slot for a proprietary packet format — a future
        // feature that needs bench research first.
        break;
    }
}

// ─── parseChannelData ──────────────────────────────────────────
void JetiExBus::parseChannelData(const uint8_t* frame, uint8_t len)
{
    if (len < 10) return; // 6-byte header + ≥1 channel (2B) + 2B CRC

    uint8_t subLen = frame[5];              // data-block length (byte 5)
    uint8_t numCh  = subLen / 2;
    if (numCh > RxConfig::MAX_CHANNELS) numCh = RxConfig::MAX_CHANNELS;

    // Verify we have enough bytes for all channels (data starts at byte 6).
    if (6 + subLen > len - 2) return;

    for (uint8_t i = 0; i < numCh; i++) {
        uint16_t raw = frame[6 + i * 2]
                     | (static_cast<uint16_t>(frame[7 + i * 2]) << 8);
        _channels_us[i] = JetiConfig::rawToUs(raw);
    }

    _channelCount  = numCh;
    _lastChannelMs = millis();
}

// ─── handleTelemetryRequest ────────────────────────────────────
void JetiExBus::handleTelemetryRequest(uint8_t packetId)
{
    // Expander override: serve multi-device from the shared hub instead of the
    // built-in single-device table.  The hook builds + sends its own frames.
    if (_onTelemetryRequest) { _onTelemetryRequest(packetId); return; }

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

    // Build EX data payload (shared builder; this device's USN/LSN).
    uint8_t exBuf[16];
    uint8_t len = buildExDataBlock(exBuf, _manufacturerId, _deviceId,
                                   sv.id, sv.type, sv.decimals, sv.value);
    sendExBusResponse(packetId, DATA_TELEMETRY, exBuf, len);
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

    // Build EX text payload (shared builder; this device's USN/LSN).
    uint8_t exBuf[40];
    uint8_t len = buildExTextBlock(exBuf, _manufacturerId, _deviceId,
                                   sv.id, sv.label, sv.unit);
    sendExBusResponse(packetId, DATA_TELEMETRY, exBuf, len);
}
// ─── sendDeviceNameText ──────────────────────────────────────────
void JetiExBus::sendDeviceNameText(uint8_t packetId)
{
    if (!_deviceName) return;

    // EX text for ID 0 = device name (shown in transmitter device list).
    uint8_t exBuf[40];
    uint8_t len = buildExTextBlock(exBuf, _manufacturerId, _deviceId,
                                   /*id=*/0, _deviceName, /*unit=*/nullptr);
    sendExBusResponse(packetId, DATA_TELEMETRY, exBuf, len);
}

// ─── sendAlarmResponse ───────────────────────────────────────────
void JetiExBus::sendAlarmResponse(uint8_t packetId)
{
    if (!_alarmMessage) return;

    // EX message/alarm (type bits 7-6 = 10):
    // [0x9F sep][type|len][USN16LE][LSN16LE][reserved][alarmChar][message...][crc8]
    uint8_t exBuf[32];
    uint8_t pos = writeExHead(exBuf, _manufacturerId, _deviceId);

    exBuf[pos++] = static_cast<uint8_t>(_alarmChar);

    size_t msgLen = strlen(_alarmMessage);
    if (msgLen > 16) msgLen = 16;
    memcpy(&exBuf[pos], _alarmMessage, msgLen);
    pos += msgLen;

    exBuf[1] = 0x80 | (pos - 1);   // type 0b10 (alarm) | len
    exBuf[pos] = crc8_ex(&exBuf[1], pos - 1);
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

    // Frame: [0x3B] [0x01] [totalLen] [pktId] [dataId] [subLen] [payload...] [crc16LE]
    uint8_t frame[56];
    uint8_t totalLen = 6 + payloadLen + 2;  // header(6) + payload + CRC(2)

    frame[0] = RESPONSE_HEADER;             // 0x3B
    frame[1] = 0x01;                        // second header byte — was missing
    frame[2] = totalLen;
    frame[3] = packetId;                    // echo the master's packet ID
    frame[4] = dataId;
    frame[5] = payloadLen;
    if (payloadLen > 0) {
        memcpy(&frame[6], payload, payloadLen);
    }

    // CRC-16 over bytes 0 to totalLen-3
    uint16_t crc = crc16_ccitt(frame, totalLen - 2);
    frame[totalLen - 2] = crc & 0xFF;
    frame[totalLen - 1] = (crc >> 8) & 0xFF;

    if (_txPort) {
        // Half-duplex: attach TX onto the wire, send, wait for the shift
        // register to empty, detach back to RX, then drain EXACTLY our own echo
        // (totalLen bytes).  NOT a `while(available)` drain — that can swallow
        // the master's next channel frame if it arrives right after the slot,
        // causing RC signal loss.
        _txPort->txEnable();
        _serial->write(frame, totalLen);
        _serial->flush();
        _txPort->txDisable();
        for (uint8_t i = 0; i < totalLen && _serial->available(); ++i) _serial->read();
    } else {
        _serial->write(frame, totalLen);
    }
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
