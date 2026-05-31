/*
 * jeti_ex_frame.h — JetiExFrameParser
 *
 * ONE streaming EX Bus frame parser, shared by both expander links (the Rx
 * side and the downstream/ESC side) and any standalone Jeti consumer.  Feed it
 * UART bytes; it accumulates a frame, validates the CRC16, and hands the full
 * validated frame (header byte through the trailing CRC16) to a callback.
 *
 * Frame: [0x3E/0x3D/0x3B][type][len][pktId][dataId][subLen][payload…][crc16LE]
 *   - 0x3E/0x3D = master (Rx) start, 0x3B = slave (response) start; the parser
 *     accepts all three so the same code frames both directions.
 *   - `len` (byte 2) is the TOTAL frame length incl. header + CRC.
 *   - CRC16 = reflected CCITT 0x8408 over bytes [0..len-3], LSB then MSB.
 *
 * Replaces the byte-identical IDLE→READ_TYPE→READ_LENGTH→READ_BODY state
 * machines that JetiExBus and JetiExTelemetryMonitor each used to hand-roll.
 */

#ifndef SFX_JETI_EX_FRAME_H
#define SFX_JETI_EX_FRAME_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <platform/sfx_stream.h>   // sfx::Stream (was <Arduino.h>)
#include <cstdint>
#include <functional>

#include "jeti_ex_common.h"

namespace JetiEx {

class JetiExFrameParser {
public:
    /// Invoked once per CRC-valid frame with the full frame bytes + length.
    using FrameCallback = std::function<void(const uint8_t* frame, uint8_t len)>;
    void onFrame(FrameCallback cb) { _onFrame = std::move(cb); }

    /// Accept the slave/response header 0x3B as a frame start.  TRUE for a
    /// downstream MONITOR (it parses the ESC's 0x3B replies); FALSE for the
    /// Rx-side responder — on that wire a 0x3B is ALWAYS our own half-duplex
    /// echo, and framing it would re-trigger a reply (feedback loop / RX
    /// corruption).  Master headers (0x3E/0x3D) are always accepted.
    void setAcceptSlave(bool b) { _acceptSlave = b; }

    void reset() { _state = IDLE; _idx = 0; _len = 0; }

    /// Max bytes consumed per drain() call.  A floating / noisy UART at 125 k
    /// produces a CONTINUOUS byte stream (no inter-frame gaps), so an unbounded
    /// `while(available())` never returns — it traps the caller (the expander's
    /// main-loop tick) and stalls board.process(), timing out EVERY host command
    /// (file.list / diag / audio-status).  Bound it: at ~11.4 kB/s line rate
    /// even a 100 Hz main loop keeps up with a REAL bursty stream inside this
    /// budget (leftover bytes wait for the next pass), while a noise storm is
    /// capped instead of trapping the loop.
    static constexpr uint16_t kMaxDrainBytesPerCall = 512;

    /// Drain available bytes through the parser, bounded to kMaxDrainBytesPerCall
    /// so a noisy/floating UART can't trap the caller in an unbounded read loop.
    void drain(sfx::Stream* s) {
        for (uint16_t n = 0; n < kMaxDrainBytesPerCall && s && s->available(); ++n)
            feed((uint8_t)s->read());
    }

    /// Feed a single byte.
    void feed(uint8_t b) {
        _rxBytes++;
        switch (_state) {
        case IDLE:
            if (b == START_ADDR0 || b == START_ADDR1 ||
                (_acceptSlave && b == RESPONSE_HEADER)) {
                _buf[0] = b; _idx = 1; _state = READ_TYPE;
            }
            break;
        case READ_TYPE:                       // byte 1 = packet type (0x01/0x03)
            _buf[1] = b; _idx = 2; _state = READ_LENGTH;
            break;
        case READ_LENGTH:                     // byte 2 = total length
            _buf[2] = b; _len = b; _idx = 3;
            if (_len < MIN_FRAME_SIZE || _len > MAX_FRAME_SIZE) { _errors++; _state = IDLE; }
            else _state = READ_BODY;
            break;
        case READ_BODY:
            if (_idx < MAX_FRAME_SIZE) _buf[_idx] = b;
            _idx++;
            if (_idx >= _len) { finish(); _state = IDLE; }
            break;
        }
    }

    uint32_t rxBytes() const { return _rxBytes; }
    uint32_t frames()  const { return _frames; }
    uint32_t errors()  const { return _errors; }

private:
    void finish() {
        if (_len < MIN_FRAME_SIZE) { _errors++; return; }
        const uint16_t rx = (uint16_t)(_buf[_len - 2] | (_buf[_len - 1] << 8));
        if (crc16_ccitt(_buf, _len - 2) != rx) { _errors++; return; }
        _frames++;
        if (_onFrame) _onFrame(_buf, _len);
    }

    enum State : uint8_t { IDLE, READ_TYPE, READ_LENGTH, READ_BODY };
    State    _state = IDLE;
    bool     _acceptSlave = true;        // monitor: yes; Rx responder: no
    uint8_t  _buf[MAX_FRAME_SIZE] = {};
    uint8_t  _idx = 0, _len = 0;
    FrameCallback _onFrame;
    uint32_t _rxBytes = 0, _frames = 0, _errors = 0;
};

}  // namespace JetiEx

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_JETI_EX_FRAME_H
