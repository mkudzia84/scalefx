/*
 * SbusInputRole implementation.
 */

#include "sbus_input_role.h"

#include <serial/ports.h>   // InputPortFlags::SBUS

#if SFX_PLATFORM_ESP32
#  include <rx_input/rx_common.h>
#endif

namespace sfx_core {

bool SbusInputRole::bind(sfx_peripherals::InputPort* port) {
    if (!port) return false;
    if ((port->capabilities() & InputPortFlags::SBUS) == 0) return false;
    if (!port->configureSbus()) return false;

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
#endif

    _port = port;
    return true;
}

uint16_t SbusInputRole::channel_us(uint8_t ch1based) const {
#if SFX_PLATFORM_ESP32
    return _decoder.channel_us(ch1based);
#else
    (void)ch1based;
    return 1500;
#endif
}

uint8_t SbusInputRole::channelCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.channelCount();
#else
    return 0;
#endif
}

bool SbusInputRole::valid() const {
#if SFX_PLATFORM_ESP32
    return _decoder.isValid();
#else
    return false;
#endif
}

bool SbusInputRole::failsafe()  const {
#if SFX_PLATFORM_ESP32
    return _decoder.failsafe();
#else
    return false;
#endif
}

bool SbusInputRole::frameLost() const {
#if SFX_PLATFORM_ESP32
    return _decoder.frameLost();
#else
    return false;
#endif
}

bool SbusInputRole::ch17() const {
#if SFX_PLATFORM_ESP32
    return _decoder.ch17();
#else
    return false;
#endif
}

bool SbusInputRole::ch18() const {
#if SFX_PLATFORM_ESP32
    return _decoder.ch18();
#else
    return false;
#endif
}

uint32_t SbusInputRole::frameCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.frameCount();
#else
    return 0;
#endif
}

uint32_t SbusInputRole::errorCount() const {
#if SFX_PLATFORM_ESP32
    return _decoder.errorCount();
#else
    return 0;
#endif
}

void SbusInputRole::setBroadcastHz(uint8_t hz) {
    _bcast.subscribe(hz);   // host wire subscribe; local feed keeps ticking
}

void SbusInputRole::tick() {
    if (!_port) return;

#if SFX_PLATFORM_ESP32
    _decoder.update();
#endif

    // Local effect feed + (when subscribed) wire broadcast — same cadence.
    if (_onBroadcast && _bcast.due(millis()))
        _onBroadcast(channelCount(), valid(), failsafe(), frameLost());
}

}  // namespace sfx_core
