/*
 * jeti_ex_telemetry_monitor.h — JetiExTelemetryMonitor
 *
 * LISTEN-ONLY decoder for Jeti EX Bus TELEMETRY on a secondary UART.  Sits
 * on the wire to a downstream EX Bus slave (e.g. an ESC) and decodes the
 * sensor values it reports (`dataId 0x3A` frames carrying EX data), feeding
 * them into the shared JetiTelemetryHub under SRC_DOWNSTREAM.  The master
 * channel's responder later serves the merged hub to the receiver.
 *
 * This phase is RX-only (no master polling / TX) — see input_monitor bench
 * notes on why half-duplex TX is validated separately.  The frame parser
 * mirrors JetiExBus (header / type / length / body / reflected-0x8408 CRC16,
 * with the byte-1 type field).
 */

#ifndef SFX_JETI_EX_TELEMETRY_MONITOR_H
#define SFX_JETI_EX_TELEMETRY_MONITOR_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include <cstdint>

#include "jeti_ex_common.h"
#include "jeti_telemetry_hub.h"

namespace JetiEx {

class JetiExTelemetryMonitor {
public:
    bool begin(Stream* serial) {
        if (!serial) return false;
        end();
        _serial = serial;
        return true;
    }
    void end() {
        _serial = nullptr;
        _state  = IDLE;
        _idx = 0; _need = 0;
        _rxBytes = _frames = _telemFrames = _sensors = _errors = 0;
    }

    /// Drain the UART, parse frames, decode telemetry → hub.  `nowMs` stamps
    /// hub freshness; call from the role tick().
    void update(uint32_t nowMs) {
        if (!_serial) return;
        while (_serial->available()) {
            const uint8_t b = (uint8_t)_serial->read();
            _rxBytes++;
            switch (_state) {
            case IDLE:
                // Slave telemetry responses use the 0x3B response header; we
                // also accept the master headers so we stay framed on a bus.
                if (b == START_ADDR0 || b == START_ADDR1 || b == RESPONSE_HEADER) {
                    _buf[0] = b; _idx = 1; _state = READ_TYPE;
                }
                break;
            case READ_TYPE:
                _buf[1] = b; _idx = 2; _state = READ_LENGTH;
                break;
            case READ_LENGTH:
                _buf[2] = b; _need = b; _idx = 3;
                if (_need < MIN_FRAME_SIZE || _need > MAX_FRAME_SIZE) {
                    _errors++; _state = IDLE;
                } else {
                    _state = READ_BODY;
                }
                break;
            case READ_BODY:
                if (_idx < MAX_FRAME_SIZE) _buf[_idx] = b;
                _idx++;
                if (_idx >= _need) { processFrame(nowMs); _state = IDLE; }
                break;
            }
        }
    }

    uint32_t rxByteCount()        const { return _rxBytes; }
    uint32_t frameCount()         const { return _frames; }
    uint32_t telemetryFrames()    const { return _telemFrames; }
    uint32_t sensorUpdates()      const { return _sensors; }
    uint32_t errorCount()         const { return _errors; }

private:
    void processFrame(uint32_t nowMs) {
        const uint16_t want = (uint16_t)(_buf[_need - 2] | (_buf[_need - 1] << 8));
        if (crc16_ccitt(_buf, _need - 2) != want) { _errors++; return; }
        _frames++;
        const uint8_t dataId = _buf[4];
        const uint8_t subLen = _buf[5];
        if (dataId != DATA_TELEMETRY || subLen == 0) return;   // not telemetry data
        if (6u + subLen > (uint16_t)(_need - 2)) { _errors++; return; }
        _telemFrames++;
        decodeExData(&_buf[6], subLen, nowMs);
    }

    // EX data block (per docs/EX_Bus_protocol_v1.21_EN.pdf, verified on radio):
    //   [0x9F sep][type|len][USN16LE][LSN16LE][reserved][entries...][crc8]
    //   type bits 7-6: 0b01 = data (values), 0b00 = text (name/label).
    // The device is passed through with its OWN identity (USN/LSN/name) so the
    // radio shows it as a distinct device (Rule: native expander pass-through).
    void decodeExData(const uint8_t* p, uint8_t len, uint32_t nowMs) {
        if (len < 9 || (p[0] & 0x0F) != 0x0F) { _errors++; return; }   // 0xNF separator
        const uint8_t  frameType = p[1] >> 6;                  // 0b01=data, 0b00=text
        const uint16_t usn = (uint16_t)(p[2] | (p[3] << 8));   // manufacturer
        const uint16_t lsn = (uint16_t)(p[4] | (p[5] << 8));   // device serial
        auto& hub = JetiTelemetryHub::instance();
        const uint8_t end = (uint8_t)(len - 1);                // exclude trailing crc8

        if (frameType == 1) {                                  // DATA: one or more values
            const uint8_t dev = hub.upsertDevice(usn, lsn, nullptr, /*local=*/false, nowMs);
            if (dev == 0xFF) { _errors++; return; }
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

    Stream* _serial = nullptr;
    enum ParseState : uint8_t { IDLE, READ_TYPE, READ_LENGTH, READ_BODY };
    ParseState _state = IDLE;
    uint8_t  _buf[MAX_FRAME_SIZE] = {};
    uint8_t  _idx = 0, _need = 0;
    uint32_t _rxBytes = 0, _frames = 0, _telemFrames = 0, _sensors = 0, _errors = 0;
};

}  // namespace JetiEx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_JETI_EX_TELEMETRY_MONITOR_H
