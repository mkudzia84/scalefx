/*
 * PacketReader<TDispatch> — COBS frame reader + compile-time dispatcher.
 *
 * Replaces the legacy `CommandRouter` (chain-of-handlers via
 * `ICommandHandler*`) with a typed, single-target dispatcher resolved at
 * compile time:
 *
 *   PacketReader<HubFxBoard> reader;
 *   reader.begin(&Serial, &board);
 *   reader.process();                  // call from loop()
 *
 * The dispatch target — typically a `BoardServer<...Policies>` —
 * must expose:
 *
 *   void dispatch(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len);
 *
 * That method walks the policy tuple, sets the current correlation tag,
 * routes the packet to the first owning policy, and NACKs if none.  No
 * runtime virtual dispatch and no MAX_HANDLERS static array.
 */

#ifndef SFX_PACKET_READER_H
#define SFX_PACKET_READER_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "core.h"

namespace sfx_core {

template <typename TDispatch>
class PacketReader {
public:
    static constexpr size_t RX_BUFFER_SIZE = CoreProtocol::COBS_BUFFER_SIZE;

    PacketReader() = default;

    /// Bind to a serial stream and a dispatch target (BoardServer<...>&).
    /// Both pointers are non-owning.
    void begin(Stream* serial, TDispatch* dispatch) {
        _serial         = serial;
        _dispatch       = dispatch;
        _rxIndex        = 0;
        _lastActivityMs = 0;
    }

    /// Pump available bytes through the framer; call once per loop().
    /// Each complete COBS frame is parsed and routed to `_dispatch`.
    int process() {
        if (!_serial || !_dispatch) return 0;

        int packetsProcessed = 0;
        while (_serial->available()) {
            uint8_t b = _serial->read();
            _lastActivityMs = millis();

            if (b == CoreProtocol::FRAME_DELIMITER) {
                if (_rxIndex > 0) {
                    processFrame(_rxBuffer, _rxIndex);
                    packetsProcessed++;
                    _rxIndex = 0;
                }
            } else if (_rxIndex < RX_BUFFER_SIZE) {
                _rxBuffer[_rxIndex++] = b;
            } else {
                _rxIndex = 0;     // overflow — drop and resync on next delim
            }
        }
        return packetsProcessed;
    }

    /// Millis() of the most recent byte read (any direction).
    unsigned long lastActivityMs() const { return _lastActivityMs; }

private:
    void processFrame(const uint8_t* frame, size_t frameLen) {
        uint8_t decoded[CoreProtocol::MAX_PACKET_SIZE];
        size_t  decodedLen = CoreProtocol::cobsDecode(frame, frameLen,
                                                      decoded, sizeof(decoded));
        if (decodedLen < 5) return;   // type + tag + len(2) + crc

        uint8_t        type, tag;
        const uint8_t* payload;
        size_t         payloadLen;
        if (!CoreProtocol::parsePacket(decoded, decodedLen,
                                       &type, &tag, &payload, &payloadLen)) {
            return;                   // CRC/framing reject
        }

        _dispatch->dispatch(type, tag, payload, payloadLen);
    }

    Stream*       _serial         = nullptr;
    TDispatch*    _dispatch       = nullptr;
    uint8_t       _rxBuffer[RX_BUFFER_SIZE];
    size_t        _rxIndex        = 0;
    unsigned long _lastActivityMs = 0;
};

}  // namespace sfx_core

#endif  // SFX_PACKET_READER_H
