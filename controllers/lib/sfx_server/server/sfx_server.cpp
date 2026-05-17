/*
 * SfxServer — non-template helpers.
 *
 * Most of SfxServer's logic is in the templated header (depends on the
 * policy pack).  Only the methods that don't reference the template
 * parameters live here:
 *   - buildDeviceName(prefix)     — reads silicon ID, formats string
 *   - addExpectedI2CDevice(...)   — pushes onto the expected-device table
 *   - performI2CScan()            — does the actual bus walk + filtering
 */

#include "sfx_server.h"

#include <Arduino.h>
#include <cstring>

#include "power/i2c_device.h"
#include "platform/sfx_platform.h"


void SfxServerBase::buildDeviceName(const char* prefix) {
    char boardId[16];
    sfxGetBoardId(boardId, sizeof(boardId));
    // sfxGetBoardId returns 8 hex chars — use last 4 for compact suffix.
    size_t len = strlen(boardId);
    const char* suffix = (len >= 4) ? &boardId[len - 4] : boardId;
    snprintf(_deviceName, sizeof(_deviceName), "%s-%s", prefix, suffix);
}

void SfxServerBase::addExpectedI2CDevice(uint8_t address, I2CDevice* device) {
    if (_numExpectedI2C < MAX_EXPECTED_I2C) {
        _expectedI2C[_numExpectedI2C].address = address;
        _expectedI2C[_numExpectedI2C].device = device;
        _numExpectedI2C++;
    }
}

I2CScanResult SfxServerBase::performI2CScan() {
    I2CScanResult result;
    if (!_i2cWire) return result;

    result.numExpected = _numExpectedI2C;
    for (uint8_t i = 0; i < _numExpectedI2C; i++) {
        result.expected[i].address    = _expectedI2C[i].address;
        result.expected[i].found      = I2CDevice::probe(*_i2cWire, _expectedI2C[i].address);
        result.expected[i].identified = _expectedI2C[i].device != nullptr
                                     && _expectedI2C[i].device->isAvailable();
    }

    uint8_t allAddrs[32];
    uint8_t totalFound = I2CDevice::scan(*_i2cWire, 0x08, 0x77, allAddrs, 32);

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
