/*
 * JetiExInputRole implementation.
 */

#include "jeti_ex_input_role.h"

#include <serial/ports.h>   // InputPortFlags::JETI_EX

namespace sfx_core {

bool JetiExInputRole::bind(sfx_peripherals::InputPort* port, uint32_t baud) {
    if (!port) return false;
    if ((port->capabilities() & InputPortFlags::JETI_EX) == 0) return false;
    if (!port->configureJetiEx(baud)) return false;

#if SFX_PLATFORM_ESP32
    Stream* s = port->uartStream();
    if (!s) {
        port->disable();
        return false;
    }
    if (!_decoder.begin(s)) {
        port->disable();
        return false;
    }
#else
    (void)baud;
#endif

    _port = port;
    return true;
}

uint16_t JetiExInputRole::channel_us(uint8_t ch1based) const {
#if SFX_PLATFORM_ESP32
    return _decoder.channel_us(ch1based);
#else
    (void)ch1based;
    return 1500;
#endif
}

uint8_t JetiExInputRole::channelCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.channelCount();
#else
    return 0;
#endif
}

bool JetiExInputRole::valid() const {
#if SFX_PLATFORM_ESP32
    return _decoder.isValid();
#else
    return false;
#endif
}

uint32_t JetiExInputRole::rxFrameCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.rxFrameCount();
#else
    return 0;
#endif
}

uint32_t JetiExInputRole::rxErrorCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.rxErrorCount();
#else
    return 0;
#endif
}

uint32_t JetiExInputRole::txResponseCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.txResponseCount();
#else
    return 0;
#endif
}

uint32_t JetiExInputRole::rxByteCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.rxByteCount();
#else
    return 0;
#endif
}

void JetiExInputRole::setSensorInfo(uint16_t manufacturerId, uint16_t deviceId, const char* name) {
#if SFX_PLATFORM_ESP32
    _decoder.setSensorInfo(manufacturerId, deviceId, name);
#else
    (void)manufacturerId; (void)deviceId; (void)name;
#endif
}

#if SFX_PLATFORM_ESP32
bool JetiExInputRole::addSensorValue(uint8_t id, const char* label, const char* unit,
                                     JetiEx::ExDataType type, uint8_t decimals) {
    return _decoder.addSensorValue(id, label, unit, type, decimals);
}
#endif

void JetiExInputRole::setSensorValue(uint8_t id, int32_t value) {
#if SFX_PLATFORM_ESP32
    _decoder.setSensorValue(id, value);
#else
    (void)id; (void)value;
#endif
}

uint8_t JetiExInputRole::sensorValueCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.sensorValueCount();
#else
    return 0;
#endif
}

#if SFX_PLATFORM_ESP32
bool JetiExInputRole::addParam(uint8_t id, const char* label, JetiEx::ParamType type,
                               int32_t minVal, int32_t maxVal, int32_t defaultVal) {
    return _decoder.addParam(id, label, type, minVal, maxVal, defaultVal);
}
#endif

void JetiExInputRole::setParamValue(uint8_t id, int32_t value) {
#if SFX_PLATFORM_ESP32
    _decoder.setParamValue(id, value);
#else
    (void)id; (void)value;
#endif
}

int32_t JetiExInputRole::getParamValue(uint8_t id) const {
#if SFX_PLATFORM_ESP32
    return _decoder.getParamValue(id);
#else
    (void)id;
    return 0;
#endif
}

uint8_t JetiExInputRole::paramCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.paramCount();
#else
    return 0;
#endif
}

void JetiExInputRole::onParamChange(ParamChangeCallback cb) {
#if SFX_PLATFORM_ESP32
    _decoder.onParamChange(std::move(cb));
#else
    (void)cb;
#endif
}

void JetiExInputRole::setAlarm(char alarmChar, const char* message) {
#if SFX_PLATFORM_ESP32
    _decoder.setAlarm(alarmChar, message);
#else
    (void)alarmChar; (void)message;
#endif
}

void JetiExInputRole::clearAlarm() {
#if SFX_PLATFORM_ESP32
    _decoder.clearAlarm();
#endif
}

bool JetiExInputRole::alarmActive() const {
#if SFX_PLATFORM_ESP32
    return _decoder.alarmActive();
#else
    return false;
#endif
}

void JetiExInputRole::setBroadcastHz(uint8_t hz) {
    _broadcastHz = hz;
    _broadcastInterval_ms = (hz == 0) ? 0 : (1000u / hz);
    _lastBroadcastMs = 0;
}

void JetiExInputRole::tick() {
    if (!_port) return;

#if SFX_PLATFORM_ESP32
    _decoder.update();
#endif

    if (_broadcastInterval_ms == 0 || !_onBroadcast) return;
    const uint32_t now = millis();
    if (now - _lastBroadcastMs >= _broadcastInterval_ms) {
        _lastBroadcastMs = now;
        _onBroadcast(channelCount(), valid(), rxFrameCount(), rxErrorCount());
    }
}

}  // namespace sfx_core
