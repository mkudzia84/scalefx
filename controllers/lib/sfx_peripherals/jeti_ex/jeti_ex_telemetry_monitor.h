/*
 * jeti_ex_telemetry_monitor.h — JetiExTelemetryMonitor
 *
 * LISTEN-ONLY decoder for Jeti EX Bus TELEMETRY on a downstream link (e.g. to
 * an ESC).  Decodes the device's EX telemetry (`dataId 0x3A` frames) and feeds
 * it into the shared JetiTelemetryHub PASS-THROUGH — preserving the device's
 * own USN/LSN/name so the radio shows it as a distinct device.  Framing is the
 * shared JetiExFrameParser (CRC handled there); this class is just the EX-data
 * decode + hub upsert.
 */

#ifndef SFX_JETI_EX_TELEMETRY_MONITOR_H
#define SFX_JETI_EX_TELEMETRY_MONITOR_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include <cstdint>

#include "jeti_ex_common.h"
#include "jeti_ex_frame.h"
#include "jeti_telemetry_hub.h"

namespace JetiEx {

class JetiExTelemetryMonitor {
public:
    bool begin(Stream* serial) {
        if (!serial) return false;
        end();
        _serial = serial;
        _parser.onFrame([this](const uint8_t* f, uint8_t l){ onFrame(f, l); });
        _parser.reset();
        return true;
    }
    void end() {
        _serial = nullptr;
        _parser.reset();
        _now = _telemFrames = _sensors = _decErrors = 0;
    }

    /// Drain the UART, parse frames, decode telemetry → hub.  `nowMs` stamps
    /// hub freshness; call from the owning task/tick.
    void update(uint32_t nowMs) {
        _now = nowMs;
        _parser.drain(_serial);
    }

    uint32_t rxByteCount()     const { return _parser.rxBytes(); }
    uint32_t frameCount()      const { return _parser.frames(); }
    uint32_t telemetryFrames() const { return _telemFrames; }
    uint32_t sensorUpdates()   const { return _sensors; }
    uint32_t errorCount()      const { return _parser.errors() + _decErrors; }

private:
    // One CRC-valid frame: keep only telemetry (dataId 0x3A) with a payload.
    void onFrame(const uint8_t* frame, uint8_t len) {
        const uint8_t dataId = frame[4];
        const uint8_t subLen = frame[5];
        if (dataId != DATA_TELEMETRY || subLen == 0) return;     // channel/menu frame
        if (6u + subLen > (uint16_t)(len - 2)) { _decErrors++; return; }
        _telemFrames++;
        decodeExData(&frame[6], subLen, _now);
    }

    // EX data block (per docs/EX_Bus_protocol_v1.21_EN.pdf, verified on radio):
    //   [0x9F sep][type|len][USN16LE][LSN16LE][reserved][entries...][crc8]
    //   type bits 7-6: 0b01 = data (values), 0b00 = text (name/label).
    // The device is passed through with its OWN identity (USN/LSN/name).
    void decodeExData(const uint8_t* p, uint8_t len, uint32_t nowMs) {
        if (len < 9 || (p[0] & 0x0F) != 0x0F) { _decErrors++; return; }   // 0xNF separator
        const uint8_t  frameType = p[1] >> 6;                  // 0b01=data, 0b00=text
        const uint16_t usn = (uint16_t)(p[2] | (p[3] << 8));   // manufacturer
        const uint16_t lsn = (uint16_t)(p[4] | (p[5] << 8));   // device serial
        auto& hub = JetiTelemetryHub::instance();
        const uint8_t end = (uint8_t)(len - 1);                // exclude trailing crc8

        if (frameType == 1) {                                  // DATA: one or more values
            const uint8_t dev = hub.upsertDevice(usn, lsn, nullptr, /*local=*/false, nowMs);
            if (dev == 0xFF) { _decErrors++; return; }
            uint8_t off = 7;                                   // skip sep,typeLen,USN,LSN,reserved
            while (off < end) {
                uint8_t  id, dp, consumed;
                ExDataType type;
                int32_t  value;
                if (!decodeSensorValue(&p[off], (size_t)(end - off),
                                       id, type, value, dp, consumed)) break;
                hub.setSensor(dev, id, type, dp, value, nowMs);
                _sensors++;
                off = (uint8_t)(off + consumed);
            }
        } else if (frameType == 0) {                           // TEXT: device name (id 0) or label
            const uint8_t id    = p[7] & 0x0F;
            const uint8_t lens  = p[8];                         // bits7-3 = label len, 2-0 = unit len
            const uint8_t llen  = lens >> 3;
            const uint8_t ulen  = lens & 0x07;
            if (9u + llen + ulen > end) return;
            char text[24] = {}, unit[6] = {};
            for (uint8_t i = 0; i < llen && i < 23; ++i) text[i] = (char)p[9 + i];
            for (uint8_t i = 0; i < ulen && i < 5;  ++i) unit[i] = (char)p[9 + llen + i];
            if (id == 0) {                                     // device name
                hub.upsertDevice(usn, lsn, text, /*local=*/false, nowMs);
            } else {                                           // sensor label
                const uint8_t dev = hub.upsertDevice(usn, lsn, nullptr, false, nowMs);
                if (dev != 0xFF) hub.setLabel(dev, id, text, unit);
            }
        }
    }

    Stream*           _serial = nullptr;
    JetiExFrameParser _parser;
    uint32_t          _now = 0;
    uint32_t          _telemFrames = 0, _sensors = 0, _decErrors = 0;
};

}  // namespace JetiEx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_JETI_EX_TELEMETRY_MONITOR_H
