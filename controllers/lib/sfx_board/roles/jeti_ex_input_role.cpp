/*
 * JetiExInputRole implementation — a thin handle that delegates to the
 * board-unique JetiExpander, whose decode it drives from tick() in the main
 * loop (no separate task).  See the header.
 */

#include "jeti_ex_input_role.h"

#include <serial/ports.h>   // InputPortFlags::JETI_EX

namespace sfx_core {

bool JetiExInputRole::bind(sfx_peripherals::InputPort* port, uint32_t baud) {
    (void)baud;   // the expander owns the configure/baud
    if (!port) return false;
    if ((port->capabilities() & InputPortFlags::JETI_EX) == 0) return false;
    _port = port;
    return true;
}

#if SFX_PLATFORM_ESP32
#  define JEXP() JetiEx::JetiExpander::instance()
uint16_t JetiExInputRole::channel_us(uint8_t ch1based) const { return JEXP().channel_us(ch1based); }
uint8_t  JetiExInputRole::channelCount() const              { return JEXP().channelCount(); }
bool     JetiExInputRole::valid()        const              { return JEXP().valid(); }
uint32_t JetiExInputRole::rxFrameCount()    const           { return JEXP().rxFrames(); }
uint32_t JetiExInputRole::rxErrorCount()    const           { return JEXP().rxErrors(); }
uint32_t JetiExInputRole::txResponseCount() const           { return JEXP().txResp(); }
uint32_t JetiExInputRole::rxByteCount()     const           { return JEXP().rxBytes(); }
#  undef JEXP
#else
uint16_t JetiExInputRole::channel_us(uint8_t) const { return 1500; }
uint8_t  JetiExInputRole::channelCount() const      { return 0; }
bool     JetiExInputRole::valid()        const      { return false; }
uint32_t JetiExInputRole::rxFrameCount()    const   { return 0; }
uint32_t JetiExInputRole::rxErrorCount()    const   { return 0; }
uint32_t JetiExInputRole::txResponseCount() const   { return 0; }
uint32_t JetiExInputRole::rxByteCount()     const   { return 0; }
#endif

void JetiExInputRole::setBroadcastHz(uint8_t hz) {
    // Host wire subscribe — gates ONLY the wire broadcast; the local effect
    // feed keeps ticking at the fixed firmware rate.
    _broadcastHz = hz;
    _wireEnabled = (hz != 0);
}

void JetiExInputRole::tick() {
    if (!_port) return;
#if SFX_PLATFORM_ESP32
    // Drive the expander's decode IN THE MAIN LOOP (cooperative) — it no longer
    // runs on a prio-3 Core-0 task that preempted the storage/upload pipeline.
    JetiEx::JetiExpander::instance().update();
#endif
    // Broadcast timer only.
    if (_broadcastInterval_ms == 0 || !_onBroadcast) return;
    const uint32_t now = millis();
    if (now - _lastBroadcastMs >= _broadcastInterval_ms) {
        _lastBroadcastMs = now;
        _onBroadcast(channelCount(), valid(), rxFrameCount(), rxErrorCount());
    }
}

}  // namespace sfx_core
