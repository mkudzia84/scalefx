/*
 * LightFxEffect — master-side orchestrator for LightFX-class slaves.
 *
 * Status: scaffolding only — no implementation.  Captures the API
 * surface for the LightFX migration step (see
 * instructions/15-GENERIC-SLAVE-REFACTOR.md § "Step 3 — LightFX").
 *
 * Slave fingerprint: 2 servos + 0 PWM + 8 LedDigital.
 * Master responsibilities:
 *   - resolve YAML programs[] into LedEventSeq → LED_PROGRAM_LOAD
 *   - resolve YAML landing_groups[] into per-channel address lists
 *   - watch RC channels + day/night switch → load/run programs
 *   - servo position bindings (gear + nav-light)
 *   - merge dedicated-LED and PWM-borrowed-LED address spaces
 *     transparently for groups (see SlavePacket::LedAddr in slave.h)
 *
 * The LedEventSeq runtime stays slave-side; this orchestrator only
 * loads programs once and triggers RUN/STOP commands.
 */

#ifndef HUBFX_LIGHTFX_EFFECT_H
#define HUBFX_LIGHTFX_EFFECT_H

#include <array>
#include <cstdint>

namespace hubfx::effects::lightfx {

constexpr uint8_t MAX_PROGRAMS_PER_CHANNEL = 4;   ///< program slots LED runtime keeps
constexpr uint8_t MAX_LANDING_GROUPS       = 8;
constexpr uint8_t MAX_CHANNELS_PER_GROUP   = 14;  ///< 8 LightFX-slave + 6 HubFX-local

/// Channel allocation on the LightFX slave side.
namespace SlaveLayout {
    constexpr uint8_t SERVO_GEAR    = 0;
    constexpr uint8_t SERVO_NAV     = 1;
    constexpr uint8_t LED_COUNT     = 8;
}

/// Resolved landing-light group — bundle of LED addresses (across
/// dedicated + PWM-borrowed) that share a single trigger.
struct LandingGroup {
    char     name[16]               = {0};
    uint8_t  addrs[MAX_CHANNELS_PER_GROUP];   ///< LED-address bytes (bit 7 = PWM-borrowed)
    uint8_t  addrCount              = 0;
    uint8_t  programIdDay           = 0;
    uint8_t  programIdNight         = 0;
    uint8_t  rcChannel              = 0;      ///< 1-based RC alias; 0 = always on
    uint16_t rcThresholdHigh_us     = 1700;
    uint16_t rcThresholdLow_us      = 1300;
};

struct LightFxConfig {
    LandingGroup groups[MAX_LANDING_GROUPS];
    uint8_t      groupCount         = 0;

    uint8_t      dayNightRcChannel  = 0;      ///< 0 = always day
    uint16_t     dayNightThreshold_us = 1500;

    uint16_t     gearDeploy_us      = 2000;
    uint16_t     gearRetract_us     = 1000;
    uint8_t      gearRcChannel      = 0;

    uint8_t      navRcChannel       = 0;
    std::array<uint16_t, 3> navPositions_us { 1000, 1500, 2000 };
};

class SlaveApi;
class RcInputs;
class HubStatusBuilder;

class LightFxEffect {
public:
    bool begin(SlaveApi*         slave,
               RcInputs*         rc,
               HubStatusBuilder* statusOut);

    /// Apply config (re)load — rebuilds resolved groups, pushes
    /// programs to slave via LED_PROGRAM_LOAD, leaves them stopped.
    /// Day/night re-switching reloads the program set.
    void applyConfig(const LightFxConfig& cfg);

    /// Drive the state machine — call from main loop().
    void update();

    // ── Slave-async hooks ────────────────────────────────────────────
    void onLedProgramDone   (uint8_t addr, uint8_t progId);
    void onServoTargetReached(uint8_t idx, uint16_t pos_us);

    // ── Manual control surface (CLI / Studio) ────────────────────────
    bool setGroupActive (uint8_t groupIdx, bool active);
    bool setGearPosition(bool deployed);
    bool setNavLevel    (uint8_t level);     ///< 0..2

    // ── Telemetry ────────────────────────────────────────────────────
    bool isGroupActive(uint8_t groupIdx) const;
    bool isDayMode()                     const;

private:
    SlaveApi*         _slave  = nullptr;
    RcInputs*         _rc     = nullptr;
    HubStatusBuilder* _status = nullptr;
    LightFxConfig     _cfg{};

    bool              _dayMode = true;
    std::array<bool, MAX_LANDING_GROUPS> _groupActive{};

    bool   matchesFingerprint() const;
    void   loadProgramsForMode();              ///< pushes day or night progset to slave
    void   activateGroup  (uint8_t groupIdx);  ///< LED_PROGRAM_RUN for each addr
    void   deactivateGroup(uint8_t groupIdx);  ///< LED_PROGRAM_STOP for each addr
};

}  // namespace hubfx::effects::lightfx

#endif  // HUBFX_LIGHTFX_EFFECT_H
