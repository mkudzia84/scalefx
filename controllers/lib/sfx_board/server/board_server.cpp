/*
 * BoardServerBase — non-template helpers.
 *
 * Wire helpers (sendRawPacket / sendNack), COBS frame reader, device-name
 * builder, and I²C-scan probe.  Template-dependent logic (dispatchPacket,
 * aggregateErrorMessage, lifecycle wiring) lives in board_server.h.
 */

#include "board_server.h"

#include <cstdio>
#include <cstring>

#include <power/i2c_device.h>
#include <platform/sfx_platform.h>

namespace sfx_core {

// ── Wire helpers ────────────────────────────────────────────────────

int BoardServerBase::sendRawPacket(uint8_t type, uint8_t tag,
                                   const uint8_t* payload, size_t len) {
    if (captureRawIfNeeded(type, tag, payload, len)) return 0;
    if (!_serial) return -1;
    uint8_t buf[SfxWire::COBS_BUFFER_SIZE];
    size_t  encoded = SfxWire::encodePacket(buf, type, tag, payload, len);
    if (encoded == 0) return -1;
    return static_cast<int>(_serial->write(buf, encoded));
}

int BoardServerBase::sendNack(uint8_t errorCode, const char* reason) {
    if (_captureNext) {
        _capturedAck = false;
        _capturedErr = errorCode;
        _captureNext = false;
        return 0;
    }
    uint8_t payload[64];
    payload[0] = errorCode;

    const char* msg = (reason && reason[0]) ? reason : aggregateErrorMessage(errorCode);
    if (!msg) msg = SerialError::getMessage(errorCode);
    // Final fallback — neither the caller, the effect aggregator, nor
    // SerialError had a message for this code (SerialError now returns
    // null for codes outside its allocated ranges instead of
    // "Domain-specific error").  Ship just the byte; the Go side
    // resolves the name via its errorNames registry.
    if (!msg) {
        return sendRawPacket(CorePacket::NACK, _currentTag, payload, 1);
    }

    size_t msgLen = std::strlen(msg);
    if (msgLen > sizeof(payload) - 1) msgLen = sizeof(payload) - 1;
    std::memcpy(&payload[1], msg, msgLen);

    return sendRawPacket(CorePacket::NACK, _currentTag, payload, 1 + msgLen);
}

// ── COBS frame reader ───────────────────────────────────────────────

int BoardServerBase::readFrames() {
    if (!_serial) return 0;

    int frames = 0;
    while (_serial->available()) {
        const uint8_t b = static_cast<uint8_t>(_serial->read());
        _lastActivityMs = SFX_MILLIS();
        // PacketReader owns the byte→frame state machine (FRAME_DELIMITER
        // detection, partial-frame buffering, overflow recovery).  The
        // lambda runs once per complete frame and does the COBS decode +
        // parsePacket + dispatch the inline loop used to do.
        if (_reader.feedByte(b, [this, &frames](const uint8_t* frame, size_t frameLen) {
                uint8_t        decoded[SfxWire::MAX_PACKET_SIZE];
                const size_t   decodedLen = SfxWire::cobsDecode(
                    frame, frameLen, decoded, sizeof(decoded));
                if (decodedLen < 5) return;   // type + tag + len(2) + crc

                uint8_t        type, tag;
                const uint8_t* payload;
                size_t         payloadLen;
                if (SfxWire::parsePacket(decoded, decodedLen,
                                         &type, &tag, &payload, &payloadLen)) {
                    dispatchPacket(type, tag, payload, payloadLen);
                    ++frames;
                }
            })) {
            // feedByte returned true → frame was emitted.  Counter
            // already incremented inside the lambda above.  Nothing
            // else to do.
        }
    }
    return frames;
}

// ── Device name ─────────────────────────────────────────────────────

void BoardServerBase::buildDeviceName(const char* prefix) {
    char boardId[16];
    sfxGetBoardId(boardId, sizeof(boardId));
    // sfxGetBoardId returns 8 hex chars — use the last 4 as a compact suffix.
    size_t len = std::strlen(boardId);
    const char* suffix = (len >= 4) ? &boardId[len - 4] : boardId;
    std::snprintf(_deviceName, sizeof(_deviceName), "%s-%s", prefix, suffix);
}

// ── I²C scan registration ────────────────────────────────────────────

bool BoardServerBase::addExpectedI2CDevice(uint8_t address) {
    if (_numExpectedI2C >= MAX_EXPECTED_I2C) {
        SFX_LOG_WARN("[I2C] addExpectedI2CDevice(0x%02X): table full "
                     "(cap=%u). Bump SFX_MAX_EXPECTED_I2C in platformio.ini.",
                     address, (unsigned)MAX_EXPECTED_I2C);
        return false;
    }
    _expectedI2C[_numExpectedI2C].address = address;
    _numExpectedI2C++;
    return true;
}

I2CScanResult BoardServerBase::performI2CScan() {
    I2CScanResult result;
    if (!_i2cBus) return result;

    result.numExpected = _numExpectedI2C;
    for (uint8_t i = 0; i < _numExpectedI2C; i++) {
        result.expected[i].address    = _expectedI2C[i].address;
        result.expected[i].found      = _i2cBus->probe(_expectedI2C[i].address);
        result.expected[i].identified = result.expected[i].found;
    }

    // ACK-scan the 7-bit address space (native bus probe).
    uint8_t allAddrs[32];
    uint8_t totalFound = 0;
    for (uint16_t a = 0x08; a <= 0x77 && totalFound < 32; ++a)
        if (_i2cBus->probe((uint8_t)a)) allAddrs[totalFound++] = (uint8_t)a;

    result.numExtra = 0;
    for (uint8_t j = 0; j < totalFound; j++) {
        bool isExpected = false;
        for (uint8_t i = 0; i < _numExpectedI2C; i++) {
            if (allAddrs[j] == _expectedI2C[i].address) { isExpected = true; break; }
        }
        if (!isExpected && result.numExtra < I2CScanResult::MAX_EXTRA) {
            result.extraAddresses[result.numExtra++] = allAddrs[j];
        }
    }
    return result;
}

}  // namespace sfx_core
