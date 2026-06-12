/*
 * JetiExInputRole implementation — a thin handle that delegates to the
 * board-unique JetiExpander, whose decode it drives from tick() in the main
 * loop (no separate task).  See the header.
 */

#include "jeti_ex_input_role.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

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
uint32_t JetiExInputRole::pollsSeen()       const           { return JEXP().pollsSeen(); }
uint32_t JetiExInputRole::echoShort()       const           { return JEXP().echoShort(); }
uint32_t JetiExInputRole::maxTxDurUs()      const           { return JEXP().maxTxDurUs(); }
uint32_t JetiExInputRole::slotOverruns()    const           { return JEXP().slotOverruns(); }
bool     JetiExInputRole::responding()      const           { return JEXP().responding(); }
#  undef JEXP
#else
uint16_t JetiExInputRole::channel_us(uint8_t) const { return 1500; }
uint8_t  JetiExInputRole::channelCount() const      { return 0; }
bool     JetiExInputRole::valid()        const      { return false; }
uint32_t JetiExInputRole::rxFrameCount()    const   { return 0; }
uint32_t JetiExInputRole::rxErrorCount()    const   { return 0; }
uint32_t JetiExInputRole::txResponseCount() const   { return 0; }
uint32_t JetiExInputRole::rxByteCount()     const   { return 0; }
uint32_t JetiExInputRole::pollsSeen()       const   { return 0; }
uint32_t JetiExInputRole::echoShort()       const   { return 0; }
uint32_t JetiExInputRole::maxTxDurUs()      const   { return 0; }
uint32_t JetiExInputRole::slotOverruns()    const   { return 0; }
bool     JetiExInputRole::responding()      const   { return false; }
#endif

void JetiExInputRole::setBroadcastHz(uint8_t hz) {
    _bcast.subscribe(hz);   // host wire subscribe; local feed keeps ticking
}

void JetiExInputRole::tick() {
    if (!_port) return;
#if SFX_PLATFORM_ESP32
    // Drive the expander's cooperative decode — but ONLY when it is NOT running
    // its dedicated IN_1 task (responding mode owns the UART on a Core-0 task so
    // the ~4 ms telemetry slot is met regardless of main-loop lag).  tickMainLoop
    // is a no-op while the task is up, so the two never double-drive the port.
    JetiEx::JetiExpander::instance().tickMainLoop();
#endif
    // Local effect feed + (when subscribed) wire broadcast — same cadence.
    if (_onBroadcast && _bcast.due(SFX_MILLIS()))
        _onBroadcast(channelCount(), valid(), rxFrameCount(), rxErrorCount());
}

}  // namespace sfx_core
