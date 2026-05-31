/*
 * BoardServicePolicy — implementation.
 *
 * Wire-helper wrappers (sendAck / sendNack / sendRawPacket / currentTag /
 * serial) forward to the owning BoardServerBase.  Lifecycle handlers
 * (INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE, STATUS_REQ, IDENTIFY,
 * I2C_SCAN, DIAG_HISTORY) decode payloads and emit responses through
 * those helpers.
 */

#include "board_service.h"
#include "board_server.h"          // BoardServerBase (complete type for the wrappers below)
#include <serial/core/core.h>      // CorePacket / CorePayload / SerialError
#include <serial/diag_log.h>       // DiagLog (history send for DIAG_HISTORY)
#include <serial/wire.h>           // SfxWire::TAG_ASYNC
#include <platform/sfx_platform.h> // sfxDramFree_bytes, sfxPsramFree_bytes

#include <Arduino.h>     // SFX_MILLIS()
#include <cstring>

namespace sfx_core {

// ── Wire-helper wrappers (defined here so BoardServerBase is complete) ─

int BoardServicePolicy::sendAck() {
    return _ctx->sendAck();
}

int BoardServicePolicy::sendNack(uint8_t errorCode, const char* reason) {
    return _ctx->sendNack(errorCode, reason);
}

int BoardServicePolicy::sendRawPacket(uint8_t type, uint8_t tag,
                                      const uint8_t* payload, size_t len) {
    return _ctx->sendRawPacket(type, tag, payload, len);
}

uint8_t BoardServicePolicy::currentTag() const { return _ctx->currentTag(); }
sfx::Stream* BoardServicePolicy::serial() const { return _ctx ? _ctx->serial() : nullptr; }

void BoardServicePolicy::setBoardInfo(const char* deviceName, const char* firmwareVersion,
                                      const char* platform, uint32_t cpuMHz, uint32_t freeRam,
                                      uint32_t buildNumber) {
    if (deviceName) {
        strncpy(_boardInfo.deviceName, deviceName, sizeof(_boardInfo.deviceName) - 1);
        _boardInfo.deviceName[sizeof(_boardInfo.deviceName) - 1] = '\0';
    }
    if (firmwareVersion) {
        const char* ver = firmwareVersion;
        if (ver[0] == 'v' || ver[0] == 'V') ver++;     // strip leading "v"
        strncpy(_boardInfo.firmwareVersion, ver, sizeof(_boardInfo.firmwareVersion) - 1);
        _boardInfo.firmwareVersion[sizeof(_boardInfo.firmwareVersion) - 1] = '\0';
    }
    if (platform) {
        strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
        _boardInfo.platform[sizeof(_boardInfo.platform) - 1] = '\0';
    }
    _boardInfo.cpuFrequencyMHz = cpuMHz;
    _boardInfo.freeRamBytes    = freeRam;
    _boardInfo.buildNumber     = buildNumber;
}

CommandHandleResult BoardServicePolicy::handle(uint8_t type, const uint8_t* payload, size_t len) {
    _prevActivityMs = _lastActivityMs;
    _lastActivityMs = SFX_MILLIS();
    if (_commandCounter == UINT32_MAX) _commandCounter = 1;
    else                                _commandCounter++;

    switch (type) {
        case CorePacket::INIT:
            handleInit(payload, len);
            return CommandHandleResult::Handled;

        case CorePacket::SHUTDOWN:
            sendAck();
            if (_shutdownCallback) _shutdownCallback();
            return CommandHandleResult::Handled;

        case CorePacket::REBOOT:
            // No ACK — device reboots immediately.
            if (_rebootCallback) _rebootCallback();
            return CommandHandleResult::Handled;

        case CorePacket::BOOTSEL:
            if (_bootselCallback) {
                _bootselCallback();
            } else {
                sendNack(SerialError::NOT_SUPPORTED);
            }
            return CommandHandleResult::Handled;

        case CorePacket::KEEPALIVE:
            if (_keepaliveCounter == UINT32_MAX) _keepaliveCounter = 1;
            else                                  _keepaliveCounter++;
            sendAck();
            if (_keepaliveCallback) _keepaliveCallback();
            return CommandHandleResult::Handled;

        case CorePacket::STATUS_REQ:
            sendStatus();
            return CommandHandleResult::Handled;

        case CorePacket::IDENTIFY:
            sendIdentify();
            return CommandHandleResult::Handled;

        case CorePacket::I2C_SCAN:
            if (_i2cScanCallback) {
                I2CScanResult result = _i2cScanCallback();
                sendI2CScanResult(result);
            } else {
                sendNack(SerialError::NOT_SUPPORTED);
            }
            return CommandHandleResult::Handled;

        case CorePacket::DIAG_HISTORY: {
            // payload[0] is the optional `max` cap (0 = entire ring).
            // The Go client trims tail-side after receiving but the
            // firmware honouring `max` keeps USB CDC traffic + reader
            // pressure under control — see DiagLog::sendHistory().
            uint16_t max = (len >= 1) ? payload[0] : 0;
            uint16_t sent = DiagLog::instance().sendHistory(max);
            uint8_t countPayload[2];
            SfxWire::putU16LE(countPayload, sent);
            sendRawPacket(CorePacket::ACK, currentTag(), countPayload, 2);
            return CommandHandleResult::Handled;
        }

        default:
            _commandCounter--;
            return CommandHandleResult::NotMyCommand;
    }
}

bool BoardServicePolicy::checkTimeout(unsigned long timeoutMs) {
    if (timeoutMs == 0 || _lastActivityMs == 0) return false;
    if (SFX_MILLIS() - _lastActivityMs > timeoutMs) {
        if (_initReceived) reset();
        return true;
    }
    return false;
}

void BoardServicePolicy::reset() {
    _initReceived   = false;
    _initFlags      = InitFlags::NONE;
    _transferActive = false;
    // boardState is NOT reset — owning SfxServer manages STANDALONE revert.
}

void BoardServicePolicy::handleInit(const uint8_t* payload, size_t len) {
    uint8_t mode  = (len >= 1) ? payload[0] : InitMode::SLAVE;
    uint8_t flags = (len >= 2) ? payload[1] : InitFlags::NONE;

    if (_initReceived) reset();
    _initFlags = flags;

    sendInitReady();
    _initReceived = true;

    if (_initCallback) _initCallback(mode, flags);
}

void BoardServicePolicy::sendInitReady() {
    uint8_t payload[128];
    size_t len = CorePayload::encodeInitReady(_boardInfo, payload);
    sendRawPacket(CorePacket::INIT_READY, currentTag(), payload, len);
}

void BoardServicePolicy::sendIdentify() {
    uint8_t payload[128];
    size_t len = CorePayload::encodeInitReady(_boardInfo, payload);
    sendRawPacket(CorePacket::IDENTIFY, currentTag(), payload, len);
}

void BoardServicePolicy::sendStatus() {
    // 22-byte core header + variable-length module-callback data
    // + 8-byte memory-extension trailer (freeDram + freePsram).  The
    // trailer is appended AFTER the module data so existing decoders
    // that read battery / gear / engine module-data at offset 22
    // keep working.  New clients pick up the trailer from `len-8`
    // (Rule 11 append-only; older firmware versions simply emit no
    // trailer and clients see len < 22+8 → no extension).  See
    // board_service.h for the wire layout.
    uint8_t payload[SfxWire::MAX_PAYLOAD_SIZE];
    size_t idx = 0;
    SfxWire::putU32LE(&payload[idx], _commandCounter);                idx += 4;
    SfxWire::putU32LE(&payload[idx], SFX_MILLIS());                        idx += 4;
    SfxWire::putU32LE(&payload[idx], _boardInfo.freeRamBytes);        idx += 4;
    uint32_t sinceActivity = (_prevActivityMs > 0) ? (SFX_MILLIS() - _prevActivityMs) : 0;
    SfxWire::putU32LE(&payload[idx], sinceActivity);                  idx += 4;
    SfxWire::putU32LE(&payload[idx], _keepaliveCounter);              idx += 4;
    payload[idx++] = _boardState;
    payload[idx++] = _initFlags;
    if (_statusDataCallback) {
        // Reserve 8 bytes at the tail so the module callback can't
        // overrun the buffer past where the mem-extension trailer
        // will land.
        idx += _statusDataCallback(&payload[idx],
                                   sizeof(payload) - idx - 8);
    }
    // Memory extension trailer.  Both fields are present on every
    // build — Pico boards return 0 for PSRAM (no external RAM).
    SfxWire::putU32LE(&payload[idx], (uint32_t)sfxDramFree_bytes());  idx += 4;
    SfxWire::putU32LE(&payload[idx], (uint32_t)sfxPsramFree_bytes()); idx += 4;
    sendRawPacket(CorePacket::STATUS, currentTag(), payload, idx);
}

void BoardServicePolicy::sendI2CScanResult(const I2CScanResult& result) {
    uint8_t payload[SfxWire::MAX_PAYLOAD_SIZE];
    size_t idx = 0;
    payload[idx++] = result.numExpected;
    for (uint8_t i = 0; i < result.numExpected && i < I2CScanResult::MAX_EXPECTED; i++) {
        payload[idx++] = result.expected[i].address;
        payload[idx++] = result.expected[i].found      ? 1 : 0;
        payload[idx++] = result.expected[i].identified ? 1 : 0;
    }
    payload[idx++] = result.numExtra;
    for (uint8_t i = 0; i < result.numExtra && i < I2CScanResult::MAX_EXTRA; i++) {
        if (idx >= sizeof(payload)) break;
        payload[idx++] = result.extraAddresses[i];
    }
    sendRawPacket(CorePacket::I2C_SCAN_RESULT, currentTag(), payload, idx);
}

void BoardServicePolicy::sendStatusUpdate(uint8_t source, uint8_t updateType,
                                          const uint8_t* data, size_t dataLen) {
    if (!isVerbose() || _transferActive || !serial()) return;
    uint8_t payload[SfxWire::MAX_PAYLOAD_SIZE];
    payload[0] = source;
    payload[1] = updateType;
    size_t len = 2;
    if (data && dataLen > 0) {
        size_t copyLen = (dataLen > sizeof(payload) - 2) ? (sizeof(payload) - 2) : dataLen;
        memcpy(&payload[2], data, copyLen);
        len += copyLen;
    }
    sendRawPacket(CorePacket::STATUS_UPDATE, SfxWire::TAG_ASYNC, payload, len);
}

void BoardServicePolicy::tickStatusBroadcast() {
    if (!isVerbose() || !_initReceived || _transferActive || !_statusDataCallback) return;
    if (_statusBroadcastInterval_ms == 0) return;
    unsigned long now = SFX_MILLIS();
    if (now - _lastStatusBroadcast_ms < _statusBroadcastInterval_ms) return;
    _lastStatusBroadcast_ms = now;
    uint8_t buf[SfxWire::MAX_PAYLOAD_SIZE - 2];
    size_t len = _statusDataCallback(buf, sizeof(buf));
    if (len > 0) {
        sendStatusUpdate(_statusBroadcastSource, StatusUpdateType::STATUS_BROADCAST, buf, len);
    }
}

}  // namespace sfx_core
