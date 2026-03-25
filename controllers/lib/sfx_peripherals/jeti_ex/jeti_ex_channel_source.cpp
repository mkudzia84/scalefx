/*
 * JetiExChannelSource — ChannelSource adapter implementation
 */

#include "jeti_ex_channel_source.h"

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include "jeti_ex_bus.h"

void JetiExChannelSource::update() {
    if (_bus) _bus->update();
}

uint16_t JetiExChannelSource::channel_us(uint8_t ch) const {
    return _bus ? _bus->channel_us(ch) : RxConfig::CENTER_US;
}

uint8_t JetiExChannelSource::channelCount() const {
    return _bus ? _bus->channelCount() : 0;
}

bool JetiExChannelSource::isValid() const {
    return _bus && _bus->isValid();
}

bool JetiExChannelSource::isInitialized() const {
    return _bus && _bus->isInitialized();
}

#endif // SFX_PLATFORM_ESP32
