/*
 * hubfx_hw_probe.h — PCA9685 + INA226 I²C bring-up + boot diagnostics.
 *
 *   The HubFX rev pairs one PCA9685 8-channel PWM driver with eight
 *   per-rail INA226 V/I monitors on a shared 400 kHz I²C bus.  Boot
 *   bring-up has to:
 *
 *     1. Reset the I²C bus + the PCA via gen-call SWRST (clears any
 *        latched address-comparator state from a previous boot).
 *     2. Probe each chip's ACK, drive `begin()`, capture MODE1 / die-id
 *        / mfg-id into a state snapshot.
 *     3. Re-probe the PCA AFTER `board.begin()` — the per-port
 *        port->begin() calls can wedge the chip on some lots; if they
 *        do, run a recovery (SWRST + re-init + restore duties).
 *     4. Replay the snapshot to DiagLog once Serial / wire stream is
 *        up so the boot log shows what happened at silent-probe time.
 *
 *   All of that used to live as ~270 lines inside the HubFxBoard class.
 *   This header pulls it out so the sketch is just port declarations +
 *   service-policy aliases + setup() / loop().  The board owns the
 *   chip drivers + PWM port array + a `HubFxHardwareProbe probe{...}`
 *   member; `initHardware()` / `logHardwareStatus()` forward to the
 *   probe.
 *
 *   INA-clone gate: TI INA226 reports MFG=0x5449, DIE=0x2260.  Chips
 *   at INA addresses that don't match those IDs are treated as clones —
 *   `INA226::begin()` returns false WITHOUT writing to the chip, so
 *   clones can't accidentally corrupt the bus (the HubFX rev hit this
 *   with a clone @ 0x40 wedging the PCA9685 @ 0x70 when written).
 */

#ifndef HUBFX_HW_PROBE_H
#define HUBFX_HW_PROBE_H

#include <Wire.h>
#include <cstdint>

#include <serial/diag_log.h>
#include <ports/pwm_port.h>          // sfx_peripherals::Pca9685PwmPort
#include <power/ina226.h>            // INA226
#include <pwm/pca9685.h>             // PCA9685

namespace hubfx_hw {

/// Per-INA226 readback captured during `init()`, replayed at log time.
struct InaProbe {
    uint8_t  addr      = 0;
    uint8_t  wireAck   = 0xFF;       ///< Wire.endTransmission() — 0 = ACK
    bool     begun     = false;      ///< driver bring-up + canonical-ID gate
    bool     idMatches = false;
    uint16_t mfgId     = 0;
    uint16_t dieId     = 0;
};

/// Whole-board bring-up snapshot.  Captured silently during `init()`
/// (BEFORE Serial is up) and replayed via `logStatus()` once DiagLog
/// has a sink.  Per-channel pre-fire breadcrumb helps pin-point any
/// regression where a specific channel write wedges the chip.
struct HwInitState {
    bool     pcaPreAck         = false;
    bool     pcaBegun          = false;
    uint8_t  pcaMode1          = 0xFF;
    uint8_t  pcaMode2          = 0xFF;
    uint8_t  pcaPrescale       = 0xFF;
    bool     pcaAfterCh[8]     = {false};
    bool     pcaPostInitAckRaw = false;
    bool     pcaPostInitAck    = false;
    uint8_t  pcaPostInitMode1  = 0xFF;
    InaProbe ina[8];
};

/// Probe driver — owns refs to the board's PCA/INA/PWM arrays and the
/// `PinConfig` constants.  Eight PWM channels + eight INA monitors is
/// hard-coded since that's the HubFX PCB rev; a future kit-board with
/// different channel counts would specialise this.
class HubFxHardwareProbe {
public:
    struct PinConfig {
        int      sda;
        int      scl;
        uint32_t i2cFreq;            ///< Hz (400 kHz on rev A)
        uint8_t  pcaAddr;            ///< PCA9685 I²C address
        uint16_t pwmFreqHz;          ///< PCA PWM frequency
        float    inaShuntOhms;
        float    inaMaxAmps;
        const uint8_t* inaAddrs;     ///< pointer to 8-element address array
    };

    HubFxHardwareProbe(PCA9685& pca, INA226* inas,
                       sfx_peripherals::Pca9685PwmPort* pwm,
                       const PinConfig& cfg)
        : _pca(pca), _inas(inas), _pwm(pwm), _cfg(cfg) {}

    HwInitState& state() { return _state; }

    /// Silent bring-up — runs BEFORE `board.begin()` so the PWM port
    /// instances can `setDuty(0)` against a live chip.  All visible
    /// output is deferred to `logStatus()` so it can land in the
    /// DiagLog ring instead of being dropped pre-Serial.
    void init() {
        Wire.begin(_cfg.sda, _cfg.scl);
        Wire.setClock(_cfg.i2cFreq);

        // General-call SWRST before any per-chip probe — recovers a
        // PCA whose address comparator latched out of normal state.
        PCA9685::broadcastReset(Wire);
        delay(2);

        _state.pcaPreAck = pcaBusProbe(_cfg.pcaAddr);
        _state.pcaBegun  = _pca.begin(Wire, _cfg.pcaAddr, _cfg.pwmFreqHz);
        if (_state.pcaBegun) {
            _state.pcaMode1    = pcaRead(_cfg.pcaAddr, 0x00);
            _state.pcaMode2    = pcaRead(_cfg.pcaAddr, 0x01);
            _state.pcaPrescale = pcaRead(_cfg.pcaAddr, 0xFE);

            // Pre-fire the same writes BoardOf::begin() will later
            // issue per PWM port — probing the bus between each so a
            // future regression can pin-point the channel that wedges.
            for (uint8_t k = 0; k < 8; ++k) {
                _pca.setChannel(k, 0);
                _state.pcaAfterCh[k] = pcaBusProbe(_cfg.pcaAddr);
            }
        }

        // Per-INA probe + driver begin + mfg/die ID read.  begin()
        // bails on non-canonical IDs without touching the chip; we
        // still capture the boot IDs so the log can identify clones.
        for (uint8_t k = 0; k < 8; ++k) {
            _state.ina[k].addr = _cfg.inaAddrs[k];
            Wire.beginTransmission(_cfg.inaAddrs[k]);
            _state.ina[k].wireAck = Wire.endTransmission();
            if (_state.ina[k].wireAck != 0) continue;

            _state.ina[k].begun =
                _inas[k].begin(Wire, _cfg.inaAddrs[k],
                               _cfg.inaShuntOhms, _cfg.inaMaxAmps);
            _state.ina[k].mfgId     = _inas[k].bootMfgId();
            _state.ina[k].dieId     = _inas[k].bootDieId();
            _state.ina[k].idMatches = _inas[k].isCanonical();
        }
    }

    /// Replay the captured snapshot to DiagLog.  Re-probes the PCA
    /// in-place (board.begin() has run by now) and, if the chip went
    /// silent during port->begin() calls, runs a recovery (SWRST →
    /// re-init → push last-commanded duties back).
    void logStatus() {
        SFX_LOG_INFO("[I2C] Wire up: SDA=GP%d SCL=GP%d @ %u Hz",
                     _cfg.sda, _cfg.scl, (unsigned)_cfg.i2cFreq);

        _state.pcaPostInitAck    = pcaBusProbe(_cfg.pcaAddr);
        _state.pcaPostInitAckRaw = _state.pcaPostInitAck;
        _state.pcaPostInitMode1  = pcaRead(_cfg.pcaAddr, 0x00);
        if (!_state.pcaPostInitAck) {
            SFX_LOG_WARN("[PCA] post-init probe failed — chip wedged during "
                         "board.begin() port-init.  Running recovery: SWRST "
                         "→ re-init → re-push duties.");
            PCA9685::broadcastReset(Wire);
            delay(2);
            const bool reinitOk = _pca.begin(Wire, _cfg.pcaAddr, _cfg.pwmFreqHz);
            if (reinitOk) {
                for (uint8_t k = 0; k < 8; ++k) {
                    _pwm[k].setDuty(_pwm[k].duty());
                }
            }
            _state.pcaPostInitAck   = pcaBusProbe(_cfg.pcaAddr);
            _state.pcaPostInitMode1 = pcaRead(_cfg.pcaAddr, 0x00);
            if (_state.pcaPostInitAck) {
                SFX_LOG_WARN("[PCA] recovery OK — chip back at 0x%02X "
                             "MODE1=0x%02X (reinit %s)",
                             _cfg.pcaAddr, _state.pcaPostInitMode1,
                             reinitOk ? "succeeded" : "FAILED");
            } else {
                SFX_LOG_ERROR("[PCA] recovery FAILED — chip still silent "
                              "after SWRST + re-init.  PWM unavailable.");
            }
        }

        if (!_state.pcaPreAck) {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: NO ACK on pre-begin probe",
                          _cfg.pcaAddr);
        }
        if (!_state.pcaBegun) {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: begin() failed", _cfg.pcaAddr);
        } else {
            SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: %u Hz  MODE1=0x%02X MODE2=0x%02X PRESCALE=0x%02X",
                         _cfg.pcaAddr, _cfg.pwmFreqHz,
                         _state.pcaMode1, _state.pcaMode2, _state.pcaPrescale);

            uint8_t wedgedAt = 0xFF;
            for (uint8_t k = 0; k < 8; ++k) {
                if (!_state.pcaAfterCh[k]) { wedgedAt = k; break; }
            }
            if (wedgedAt == 0xFF) {
                SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: 8/8 per-channel writes ACKed "
                             "(no address-comparator wedge)", _cfg.pcaAddr);
            } else {
                SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: address comparator WEDGED "
                              "after pre-fire setChannel(%u, 0) — earlier %u "
                              "channel writes ACKed.",
                              _cfg.pcaAddr, (unsigned)wedgedAt, (unsigned)wedgedAt);
            }
        }
        if (_state.pcaPostInitAck) {
            SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: post-board.begin() OK  MODE1=0x%02X",
                         _cfg.pcaAddr, _state.pcaPostInitMode1);
        } else {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: WENT SILENT after board.begin() — "
                          "a PortServicePolicy port->begin() write may have wedged the chip.",
                          _cfg.pcaAddr);
        }

        uint8_t inaOk     = 0;
        uint8_t inaClones = 0;
        for (uint8_t k = 0; k < 8; ++k) {
            const auto& p = _state.ina[k];
            if (p.wireAck != 0) {
                SFX_LOG_WARN("[INA] ch%u @ 0x%02X: NO ACK (Wire status=%u)",
                             (unsigned)(k + 1), p.addr, (unsigned)p.wireAck);
                continue;
            }
            if (!p.begun) {
                inaClones++;
                SFX_LOG_WARN("[INA] ch%u @ 0x%02X: NOT DRIVEN — non-canonical IDs "
                             "mfg=0x%04X die=0x%04X (expected 0x5449/0x2260, "
                             "TI INA226).  Clone detected — refusing to drive "
                             "to protect other chips on the shared I²C bus.",
                             (unsigned)(k + 1), p.addr, p.mfgId, p.dieId);
                continue;
            }
            SFX_LOG_INFO("[INA] ch%u @ 0x%02X: OK  mfg=0x%04X die=0x%04X (TI INA226)",
                         (unsigned)(k + 1), p.addr, p.mfgId, p.dieId);
            inaOk++;
        }
        if (inaClones > 0) {
            SFX_LOG_WARN("[INA] %u/8 monitors up (%u clone%s skipped — replace "
                         "to restore full V/I sense)",
                         inaOk, inaClones, inaClones == 1 ? "" : "s");
        } else {
            SFX_LOG_INFO("[INA] %u/8 monitors up (all genuine TI INA226)", inaOk);
        }
    }

    /// Periodic maintenance — refresh INA cached readings.  THROTTLED to
    /// `kSenseIntervalMs`: refreshing all 8 INA226 every main-loop pass is
    /// 8 sequential I²C reads (~10-16 ms) that dominated and jittered the
    /// loop period — which in turn jittered the LED animation tick.  V/I
    /// telemetry only needs ~10 Hz freshness, so polling at 100 ms lets the
    /// loop run fast + steady (LED ticks every ~1 ms) while a one-off
    /// ~12 ms read every 100 ms is invisible.  The PWM chip is event-driven;
    /// nothing to poll there.
    void pollSense() {
        const uint32_t now = millis();
        if (now - _lastSenseMs < kSenseIntervalMs) return;
        _lastSenseMs = now;
        for (uint8_t k = 0; k < 8; ++k) _inas[k].update();
    }

private:
    static bool pcaBusProbe(uint8_t addr) {
        Wire.beginTransmission(addr);
        return Wire.endTransmission() == 0;
    }
    static uint8_t pcaRead(uint8_t addr, uint8_t reg) {
        Wire.beginTransmission(addr);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0) return 0xFF;
        Wire.requestFrom((int)addr, 1);
        return Wire.available() ? (uint8_t)Wire.read() : 0xFF;
    }

    PCA9685& _pca;
    INA226*  _inas;
    sfx_peripherals::Pca9685PwmPort* _pwm;
    PinConfig _cfg;

    // INA refresh throttle — see pollSense().
    static constexpr uint32_t kSenseIntervalMs = 100;   // 10 Hz V/I refresh
    uint32_t _lastSenseMs = 0;
    HwInitState _state;
};

}  // namespace hubfx_hw

#endif  // HUBFX_HW_PROBE_H
