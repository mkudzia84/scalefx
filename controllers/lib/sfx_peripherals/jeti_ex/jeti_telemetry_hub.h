/*
 * jeti_telemetry_hub.h — JetiTelemetryHub
 *
 * Board-unique singleton (Rule 14) that aggregates Jeti EX telemetry from
 * multiple sources and presents it to the master Jeti EX Bus channel (the one
 * wired to the receiver) as a set of distinct Jeti telemetry DEVICES — exactly
 * the EX Bus Expander topology from the spec (docs/EX_Bus_protocol_v1.21_EN.pdf,
 * page 1).  The radio keys devices by (USN manufacturer, LSN serial), so the
 * downstream device (e.g. an ESC) is served PASS-THROUGH with its own identity
 * and shows on the transmitter under its own name (e.g. "MEZON: RPM") next to
 * the HubFX-own device ("HubFx: Version").
 *
 * Concentrator topology:
 *   downstream EX Bus slave (ESC) ─► UART2 monitor ─┐
 *   HubFX-internal sensors        ──────────────────┼─► JetiTelemetryHub (N devices)
 *                                                    │            │
 *   radio (Rx) ◄── UART1 master responder ◄──────────┴────────────┘  (rotates devices)
 *
 * Identity is preserved end-to-end: each device keeps its USN/LSN/name and its
 * sensors keep their ORIGINAL wire ids/labels/units.  Local (HubFX-own) devices
 * never expire; downstream devices + sensors that stop refreshing are marked
 * inactive so a disconnected ESC drops out of telemetry.
 *
 * Thread-safety: a dedicated Core-0 task (the expander) and the main loop both
 * touch the hub.  Hold lock()/unlock() (or ScopedLock) around a read that spans
 * multiple sensors so a value can't change mid-frame.  Mutex is a no-op until
 * begin() wires it; single-threaded callers can ignore it.
 */

#ifndef SFX_JETI_TELEMETRY_HUB_H
#define SFX_JETI_TELEMETRY_HUB_H

#include <cstdint>
#include <cstring>

#include <platform/sfx_platform.h>   // SfxMutex
#include "jeti_ex_common.h"

namespace JetiEx {

class JetiTelemetryHub {
public:
    static constexpr uint8_t kMaxDevices         = 6;    ///< HubFX-own + downstream
    static constexpr uint8_t kMaxSensorsPerDevice = 16;  ///< EX value ids 0..15

    struct Sensor {
        uint8_t    id       = 0;          ///< wire sensor id within the device (1..15)
        ExDataType type     = ExDataType::Int14;
        uint8_t    decimals = 0;
        int32_t    value    = 0;
        char       label[21] = {};
        char       unit[6]   = {};
        uint32_t   lastMs    = 0;
        bool       active    = false;
    };

    struct Device {
        uint16_t usn   = 0;               ///< manufacturer id (identity key)
        uint16_t lsn   = 0;               ///< device serial   (identity key)
        char     name[24] = {};
        bool     local  = false;          ///< true = HubFX-own (never expires)
        bool     active = false;
        uint32_t lastMs = 0;
        Sensor   sensors[kMaxSensorsPerDevice];
        uint8_t  sensorCount = 0;
    };

    static JetiTelemetryHub& instance() {
        static JetiTelemetryHub inst;       // C++11 thread-safe static local
        return inst;
    }

    // ── Mutex ────────────────────────────────────────────────────────
    void lock()   { sfxMutexLock(_mutex); }
    void unlock() { sfxMutexUnlock(_mutex); }
    /// RAII guard for spanning reads (responder building a multi-sensor frame).
    struct ScopedLock {
        explicit ScopedLock(JetiTelemetryHub& h) : _h(h) { _h.lock(); }
        ~ScopedLock() { _h.unlock(); }
        JetiTelemetryHub& _h;
    };

    // ── Device registry ──────────────────────────────────────────────
    /// Find-or-create a device by (usn, lsn).  Updates the name when given.
    /// `local` devices never expire (HubFX-own telemetry).  Returns the device
    /// index, or 0xFF if the table is full.
    uint8_t upsertDevice(uint16_t usn, uint16_t lsn, const char* name,
                         bool local, uint32_t nowMs) {
        Device* d = findDevice(usn, lsn);
        if (!d) {
            if (_deviceCount >= kMaxDevices) return 0xFF;
            d = &_devices[_deviceCount++];
            d->usn = usn;
            d->lsn = lsn;
        }
        if (name && name[0]) copyStr(d->name, sizeof d->name, name);
        d->local  = local;
        d->active = true;
        d->lastMs = nowMs;
        return (uint8_t)(d - _devices);
    }

    // ── Sensor upsert (by device index) ──────────────────────────────
    /// Upsert a sensor VALUE within a device.  Returns false on bad index or a
    /// full per-device table.
    bool setSensor(uint8_t devIdx, uint8_t id, ExDataType type,
                   uint8_t decimals, int32_t value, uint32_t nowMs) {
        if (devIdx >= _deviceCount) return false;
        Device& d = _devices[devIdx];
        Sensor* s = findSensor(d, id);
        if (!s) {
            if (d.sensorCount >= kMaxSensorsPerDevice) return false;
            s = &d.sensors[d.sensorCount++];
            s->id = id;
        }
        s->type     = type;
        s->decimals = decimals;
        s->value    = value;
        s->lastMs   = nowMs;
        s->active   = true;
        d.lastMs    = nowMs;
        d.active    = true;
        return true;
    }

    /// Set just label/unit for a sensor (from an EX text frame).  Creates the
    /// sensor if not seen yet (some devices send text before the first value).
    void setLabel(uint8_t devIdx, uint8_t id, const char* label, const char* unit) {
        if (devIdx >= _deviceCount) return;
        Device& d = _devices[devIdx];
        Sensor* s = findSensor(d, id);
        if (!s) {
            if (d.sensorCount >= kMaxSensorsPerDevice) return;
            s = &d.sensors[d.sensorCount++];
            s->id = id;
        }
        if (label) copyStr(s->label, sizeof s->label, label);
        if (unit)  copyStr(s->unit,  sizeof s->unit,  unit);
    }

    /// Mark non-local devices/sensors not refreshed within `timeoutMs` inactive
    /// (a disconnected ESC drops out).  Identity stays in the table so the same
    /// slot is reused on reconnect.
    void expireStale(uint32_t nowMs, uint32_t timeoutMs) {
        for (uint8_t i = 0; i < _deviceCount; ++i) {
            Device& d = _devices[i];
            if (d.local) continue;
            bool anyActive = false;
            for (uint8_t j = 0; j < d.sensorCount; ++j) {
                Sensor& s = d.sensors[j];
                if (s.active && (nowMs - s.lastMs) > timeoutMs) s.active = false;
                anyActive |= s.active;
            }
            if (d.active && !anyActive && (nowMs - d.lastMs) > timeoutMs) d.active = false;
        }
    }

    // ── Read-side (responder) ────────────────────────────────────────
    uint8_t       deviceCount()      const { return _deviceCount; }
    const Device* device(uint8_t i)  const { return (i < _deviceCount) ? &_devices[i] : nullptr; }

    /// Total active sensors across all active devices (diagnostics).
    uint8_t activeSensorCount() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < _deviceCount; ++i) {
            if (!_devices[i].active) continue;
            for (uint8_t j = 0; j < _devices[i].sensorCount; ++j)
                if (_devices[i].sensors[j].active) ++n;
        }
        return n;
    }

private:
    JetiTelemetryHub() { sfxMutexInit(_mutex); }
    JetiTelemetryHub(const JetiTelemetryHub&) = delete;
    JetiTelemetryHub& operator=(const JetiTelemetryHub&) = delete;

    Device* findDevice(uint16_t usn, uint16_t lsn) {
        for (uint8_t i = 0; i < _deviceCount; ++i)
            if (_devices[i].usn == usn && _devices[i].lsn == lsn) return &_devices[i];
        return nullptr;
    }
    static Sensor* findSensor(Device& d, uint8_t id) {
        for (uint8_t i = 0; i < d.sensorCount; ++i)
            if (d.sensors[i].id == id) return &d.sensors[i];
        return nullptr;
    }
    static void copyStr(char* dst, size_t cap, const char* src) {
        size_t n = 0;
        for (; src[n] && n < cap - 1; ++n) dst[n] = src[n];
        dst[n] = '\0';
    }

    Device   _devices[kMaxDevices];
    uint8_t  _deviceCount = 0;
    SfxMutex _mutex;
};

}  // namespace JetiEx

#endif  // SFX_JETI_TELEMETRY_HUB_H
