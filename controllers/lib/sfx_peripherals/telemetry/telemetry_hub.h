/*
 * telemetry_hub.h — TelemetryHub, the board's protocol-agnostic telemetry
 * collection.
 *
 * Board-unique singleton (Rule 14) that aggregates telemetry from any producer
 * — HubFX-internal sensors (battery/expander rails, uptime), native ESC
 * telemetry streams (Kontronik / Scorpion / Hobbywing), a downstream Jeti
 * EX-Bus slave — into a flat DEVICE → SENSOR table.  Producers write plain
 * typed values; they carry NO radio-protocol knowledge.
 *
 * Consumers scrape the collection independently:
 *   - the Jeti EX responder (JetiExpander) — only while the jeti-ex-input
 *     role is running — maps each sensor onto the smallest EX wire type at
 *     frame-encode time and serves it to the radio,
 *   - the telemetry-collection wire service (0xEB–0xED) serializes it for
 *     the Studio Telemetry tab + CLI `telemetry`.
 *
 *                       producers                 consumers
 *   ESC monitor (native stream) ──┐        ┌──► JetiExpander → radio
 *   EX-Bus downstream monitor ────┼─► HUB ─┤
 *   HubFX-internal sensors ───────┘        └──► 0xEB–0xED → Studio / CLI
 *
 * Identity is preserved end-to-end: each device keeps its USN/LSN/name and
 * its sensors keep their original ids/labels/units, so a downstream device
 * (e.g. an ESC) shows on the transmitter under its own name.  Local
 * (HubFX-own) devices never expire; downstream devices + sensors that stop
 * refreshing are marked inactive so a disconnected ESC drops out.
 *
 * Thread-safety: a dedicated Core-0 task (the Jeti responder) and the main
 * loop both touch the hub.  Hold lock()/unlock() (or ScopedLock) around a
 * read that spans multiple sensors so a value can't change mid-frame.  Mutex
 * is a no-op until begin() wires it; single-threaded callers can ignore it.
 */

#ifndef SFX_TELEMETRY_HUB_H
#define SFX_TELEMETRY_HUB_H

#include <cstdint>
#include <cstring>
#include <new>       // placement-new for the PSRAM device table

#include <platform/sfx_platform.h>   // SfxMutex

namespace sfx_telemetry {

/// Value shape of one sensor — protocol-agnostic.  Numeric sensors store the
/// scaled integer (value × 10^decimals); Gps/DateTime keep the source's raw
/// 32-bit packed encoding for pass-through (a consumer that can't represent
/// them skips them).
enum class SensorKind : uint8_t {
    Int      = 0,   ///< scaled signed integer (decimals = implied dp)
    Gps      = 1,   ///< packed GPS coordinate (pass-through)
    DateTime = 2,   ///< packed date/time (pass-through)
};

class TelemetryHub {
public:
    static constexpr uint8_t kMaxDevices          = 6;   ///< HubFX-own + downstream
    static constexpr uint8_t kMaxSensorsPerDevice = 16;

    struct Sensor {                       // 4-aligned first, chars+u8 tail:
        int32_t    value    = 0;          //   packs to 40 B (was 44 with the
        uint32_t   lastMs   = 0;          //   u8-first layout) — 96 sensors
        char       label[21] = {};        //   live in the device table.
        char       unit[6]   = {};
        uint8_t    id       = 0;          ///< sensor id within the device (1..15)
        SensorKind kind     = SensorKind::Int;
        uint8_t    decimals = 0;
        bool       active   = false;
    };
    static_assert(sizeof(Sensor) <= 40, "Sensor grew — 96 of these in the hub table");

    /// A device's current textual condition — set by producers on fault-state
    /// CHANGES (never per-frame), consumed by radio responders (Jeti EX
    /// Message packet) and UIs.  Class semantics (protocol-agnostic, aligned
    /// 1:1 with the Jeti EX message classes):
    ///   0 basic info | 1 status | 2 warning | 3 recoverable error |
    ///   4 non-recoverable error.
    /// `seq` bumps on every change so consumers can detect + de-duplicate;
    /// empty text = condition cleared (consumers announce nothing).
    struct Message {
        uint32_t seq    = 0;
        uint32_t lastMs = 0;
        char     text[24] = {};
        uint8_t  cls    = 0;
    };
    static_assert(sizeof(Message) <= 36, "Message grew — one per device");

    struct Device {
        uint16_t usn   = 0;               ///< manufacturer id (identity key)
        uint16_t lsn   = 0;               ///< device serial   (identity key)
        char     name[24] = {};
        bool     local  = false;          ///< true = HubFX-own (never expires)
        bool     active = false;
        uint32_t lastMs = 0;
        Sensor   sensors[kMaxSensorsPerDevice];
        uint8_t  sensorCount = 0;
        Message  msg;
    };

    static TelemetryHub& instance() {
        static TelemetryHub inst;           // C++11 thread-safe static local
        return inst;
    }

    /// True once the device table is allocated (PSRAM on ESP32, heap on
    /// Pico).  Allocation happens in the constructor — i.e. on the first
    /// instance() call, which is always at runtime (never static-init), so
    /// PSRAM is up.  On allocation failure the hub is inert (capacity 0).
    bool ready() const { return _devices != nullptr; }

    // ── Mutex ────────────────────────────────────────────────────────
    void lock()   { sfxMutexLock(_mutex); }
    void unlock() { sfxMutexUnlock(_mutex); }
    /// RAII guard for spanning reads (a consumer building a multi-sensor frame).
    struct ScopedLock {
        explicit ScopedLock(TelemetryHub& h) : _h(h) { _h.lock(); }
        ~ScopedLock() { _h.unlock(); }
        TelemetryHub& _h;
    };

    // ── Device registry ──────────────────────────────────────────────
    /// Find-or-create a device by (usn, lsn).  Updates the name when given.
    /// `local` devices never expire (HubFX-own telemetry).  Returns the device
    /// index, or 0xFF if the table is full.
    uint8_t upsertDevice(uint16_t usn, uint16_t lsn, const char* name,
                         bool local, uint32_t nowMs) {
        if (!_devices) return 0xFF;
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
    bool setSensor(uint8_t devIdx, uint8_t id, SensorKind kind,
                   uint8_t decimals, int32_t value, uint32_t nowMs) {
        if (devIdx >= _deviceCount) return false;
        Device& d = _devices[devIdx];
        Sensor* s = findSensor(d, id);
        if (!s) {
            if (d.sensorCount >= kMaxSensorsPerDevice) return false;
            s = &d.sensors[d.sensorCount++];
            s->id = id;
        }
        s->kind     = kind;
        s->decimals = decimals;
        s->value    = value;
        s->lastMs   = nowMs;
        s->active   = true;
        d.lastMs    = nowMs;
        d.active    = true;
        return true;
    }

    /// Set just label/unit for a sensor (from a producer's metadata frame).
    /// Creates the sensor if not seen yet (some devices send text before the
    /// first value).
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

    /// Set/clear a device's condition message.  No-op when class AND text are
    /// unchanged (so producers may call it repeatedly); otherwise the seq
    /// bumps and consumers re-announce.  Empty text = cleared.
    void setMessage(uint8_t devIdx, uint8_t cls, const char* text, uint32_t nowMs) {
        if (devIdx >= _deviceCount) return;
        Device& d = _devices[devIdx];
        const char* t = text ? text : "";
        if (d.msg.cls == cls && std::strcmp(d.msg.text, t) == 0) { d.msg.lastMs = nowMs; return; }
        d.msg.cls = cls;
        copyStr(d.msg.text, sizeof d.msg.text, t);
        d.msg.seq++;
        d.msg.lastMs = nowMs;
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

    // ── Read-side (consumers) ────────────────────────────────────────
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
    TelemetryHub() {
        sfxMutexInit(_mutex);
        // Device table in PSRAM (perf audit, instructions/34): ~4.4 KB that
        // previously sat in .bss internal SRAM.  Access is 2 Hz publishes +
        // ~65 Hz reads of a few dozen bytes under the mutex — PSRAM latency
        // is irrelevant here.  sfxPsramCalloc falls back to plain heap on
        // boards without PSRAM (Pico).
        _devices = static_cast<Device*>(sfxPsramCalloc(kMaxDevices, sizeof(Device)));
        if (_devices)
            for (uint8_t i = 0; i < kMaxDevices; ++i) new (&_devices[i]) Device();
    }
    TelemetryHub(const TelemetryHub&) = delete;
    TelemetryHub& operator=(const TelemetryHub&) = delete;

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

    Device*  _devices = nullptr;          // PSRAM table (see ctor); null = inert
    uint8_t  _deviceCount = 0;
    SfxMutex _mutex;
};

}  // namespace sfx_telemetry

#endif  // SFX_TELEMETRY_HUB_H
