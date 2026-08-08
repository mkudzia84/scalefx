/*
 * Serial Bus - Client-Side SerialBus Implementation
 * 
 * SerialBus class for HubFX (USB Host CDC communication).
 * CoreProtocol functions are implemented in serial_core.cpp.
 */

#include "bus.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include "serial/diag_log.h"

#ifndef SCALEFX_SERVER

#include "usb/sfx_usb_host.h"  // Full include — only needed for client builds

// ============================================================================
// SerialBus Implementation
// ============================================================================

bool SerialBus::begin(int deviceIndex) {
    if (_initialized) return true;

    _deviceIndex = deviceIndex;
    _reader.reset();
    _stats = {};
    _initialized = true;

    SFX_LOG_DEBUG("[SerialBus] Initialized for device %d", deviceIndex);
    return true;
}

void SerialBus::end() {
    if (!_initialized) return;

    _initialized = false;
    SFX_LOG_DEBUG("[SerialBus] Deinitialized");
}

void SerialBus::setDevice(int deviceIndex) {
    _deviceIndex = deviceIndex;
    _reader.reset();
    SFX_LOG_DEBUG("[SerialBus] Device changed to %d", deviceIndex);
}

int SerialBus::sendPacket(uint8_t type, const uint8_t* payload, size_t len, uint8_t tag) {
    if (!_initialized) return -1;
    if (len > _peerMaxPayload) return -1;  // Validate against peer's capacity
    
    UsbHost& usb = UsbHost::instance();
    
    uint8_t encoded[SfxWire::COBS_BUFFER_SIZE];
    size_t encLen = SfxWire::encodePacket(encoded, type, tag, payload, len);
    if (encLen == 0) {
        SFX_LOG_WARN("[SerialBus] Encode failed for type 0x%02X", type);
        return -1;
    }
    
    int written = usb.cdcWrite(_deviceIndex, encoded, encLen);
    if (written < 0) {
        SFX_LOG_WARN("[SerialBus] Write failed for type 0x%02X", type);
        return -1;
    }
    
    _stats.packets_sent++;
    _lastSendMs = SFX_MILLIS();  // Track time of successful send
    return 0;
}

int SerialBus::sendKeepalive() {
    return sendPacket(CorePacket::KEEPALIVE);
}

int SerialBus::process() {
    if (!_initialized) return 0;

    // Keep the public stats() API consistent — the inline buffer-overflow
    // counter that used to live on `_stats.framing_errors` is now owned
    // by `_reader`.  Sync it back into the visible struct on every poll;
    // the decode-too-short side of the same counter stays put in
    // processFrame() (post-reader concern).
    _stats.framing_errors = _reader.framingErrors();

    UsbHost& usb = UsbHost::instance();
    uint8_t readBuf[64];
    int n = usb.cdcRead(_deviceIndex, readBuf, sizeof(readBuf));
    if (n <= 0) return 0;

    // PacketReader owns the byte→frame state machine + overflow recovery.
    // processFrame() handles cobs-decode + parse + dispatch + per-frame
    // diagnostic counters.  The overflow framing_error counter lives on
    // `_reader` and is folded into stats() at read time.
    return _reader.feedBytes(readBuf, static_cast<size_t>(n),
        [this](const uint8_t* frame, size_t frameLen) {
            processFrame(frame, frameLen);
        });
}

void SerialBus::processFrame(const uint8_t* frame, size_t frameLen) {
    if (frameLen < 5) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t decoded[SfxWire::MAX_PACKET_SIZE];
    size_t decLen = SfxWire::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
    if (decLen < 5) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t type;
    uint8_t tag;
    const uint8_t* payload;
    size_t payloadLen;
    
    if (!SfxWire::parsePacket(decoded, decLen, &type, &tag, &payload, &payloadLen)) {
        _stats.crc_errors++;
        // Hex-dump the head of the rejected packet — the decisive tool for
        // wire-corruption hunts (input-gap saga, 2026-07-15; re-deployed for
        // the deterministic post-endstop parse failures, 2026-08-08).
        char hex[3 * 24 + 1];
        const size_t nDump = decLen < 24 ? decLen : 24;
        for (size_t i = 0; i < nDump; ++i) {
            static const char* d = "0123456789ABCDEF";
            hex[i * 3]     = d[decoded[i] >> 4];
            hex[i * 3 + 1] = d[decoded[i] & 0xF];
            hex[i * 3 + 2] = ' ';
        }
        hex[nDump * 3] = '\0';
        SFX_LOG_WARN("[SerialBus] Packet parse failed, len=%zu head: %s", decLen, hex);
        return;
    }
    
    _stats.packets_received++;
    
    if (_rxCallback) {
        _rxCallback(type, tag, payload, payloadLen);
    }
}

void SerialBus::setKeepaliveInterval(unsigned long intervalMs) {
    _keepaliveIntervalMs = intervalMs;
    _lastSendMs = SFX_MILLIS();  // Reset send timer
}

bool SerialBus::processKeepalive() {
    if (!_initialized || _keepaliveIntervalMs == 0) return false;
    
    // Only send keepalive if no other message was sent within the interval
    // This ensures at least one message (of any type) per interval
    unsigned long now = SFX_MILLIS();
    if (now - _lastSendMs >= _keepaliveIntervalMs) {
        sendKeepalive();
        return true;
    }
    return false;
}

bool SerialBus::isConnected() const {
    if (!_initialized) return false;
    
    const CdcDeviceInfo* device = UsbHost::instance().getCdcDevice(_deviceIndex);
    return (device != nullptr && device->connected);
}

void SerialBus::resetStats() {
    _stats = {};
    _reader.resetStats();   // zero the reader-side framing_errors too
}

#endif // !SCALEFX_SERVER
