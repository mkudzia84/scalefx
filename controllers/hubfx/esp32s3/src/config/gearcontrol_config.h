/*
 * gearcontrol_config.h — `/gearcontrol.yaml` schema.
 *
 * Gear definition table for the hub's GearControlService.  Each gear is
 * one H-bridge motor (BiDcMotor role) addressed by PortRef — typically
 * pointing at a GearControl expander board by GUID.  The motor's INA226
 * current sensor on the expander drives MOTOR_STALL_EVENT → the hub's
 * per-gear timeout/stall state machine.
 *
 * Status LEDs are NOT configured here — the GearControl expander lights
 * its own small per-motor direction LEDs locally from its H-bridge state.
 *
 * `/hubfx.yaml`'s `features.gears:` flag is the master kill-switch
 * (overrides the local `enabled:` here).
 *
 * YAML shape (v2 — the motor / door PortRefs may use the flow form `{ … }`):
 *
 *   schema_version: 2
 *   enabled: true
 *   coord: independent           # independent | door_sync | full_sync | sequenced
 *   input:                       # OPTIONAL — one RC up/down channel drives ALL gears
 *     name: gear_updown          # named channel from /hubfx.yaml inputs[] (Rule 43)
 *     threshold_us: 1500         # Boolean threshold
 *     hysteresis_us: 50
 *     invert: false              # false: above threshold (ON) = DEPLOY (gear down)
 *   sounds:                      # OPTIONAL — transit sounds on the Gear mixer channel
 *     deploy:  /sounds/gear/deploy.wav    # looped while any gear is deploying
 *     retract: /sounds/gear/retract.wav   # looped while any gear is retracting
 *     output_mask: 3             # 1 = left, 2 = right, 3 = both
 *   strut_mode: hbridge          # hbridge | servo | servo_shared (2.45.0)
 *   strut_shared:                # servo_shared only — ONE channel, all struts
 *     port: { kind: servo, idx: 4 }
 *     travel_ms: 5000            # fixed stroke duration (black-box controller)
 *   gears:
 *     - id: 0
 *       name: nose
 *       motor: { guid: "AB12", kind: hbridge, idx: 0 }   # hbridge mode
 *       strut_servo: { kind: servo, idx: 4 }             # servo mode (own channel)
 *       travel_ms: 5000          # servo mode: fixed stroke duration —
 *                                # doors engage only after it elapses
 *       reverse: false           # deploy runs the H-bridge reversed /
 *                                # drives the servo pulse to the MIN end
 *       timeout_ms:   30000      # full-travel watchdog (hbridge)
 *       motor_voltage_mv: 6000   # motor DRIVE voltage — the strut seeks
 *                                # full-scale and the role cap delivers
 *                                # exactly this at the motor on any pack
 *                                # (absent = 6000; floor 1000).  The raw
 *                                # deploy_duty/retract_duty keys are
 *                                # RETIRED (2.44.0) and ignored.
 *       doors:                   # ≤2 ServoActuator door servos
 *         - { port: { kind: servo, idx: 0 }, open: 10000, close: 0 }
 *         - { port: { kind: servo, idx: 1 }, open: 10000, close: 0 }
 *       door_mode: sync          # sync | delay | sequence (| single | none)
 *       door_delay_ms: 500       # DUAL_DELAY only
 *       close_policy: both       # both | first | none (post-deploy close)
 *
 * The `guid` is the expander's 4-hex deviceName suffix (e.g.
 * "GearCtrl-AB12" → "AB12"); omit it for a hub-local port.  Direct
 * YamlNode traversal (the declarative DSL doesn't compose the gears
 * sequence with the nested motor map).
 *
 * `input:` and `sounds:` are append-only optional blocks (Rule 11 spirit) —
 * a v2 file without them parses identically to before.
 */

#ifndef HUBFX_GEARCONTROL_CONFIG_H
#define HUBFX_GEARCONTROL_CONFIG_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <serial/diag_log.h>
#include <serial/ports.h>

#include "../effects/effect_id.h"
#include "../effects/gearcontrol/gear.h"
#include "../effects/gearcontrol/gearcontrol_service.h"   // kMaxGears
#include "port_ref_yaml.h"

struct GearControlYamlPool {
    static constexpr size_t MAX_NODES        = 256;
    static constexpr size_t STRING_POOL_SIZE = 3072;
    static constexpr size_t MAX_DEPTH        = 12;
};

/// OPTIONAL one-channel RC activation: the named up/down channel that
/// commands ALL gears (resolved against /hubfx.yaml inputs[] by the
/// GearActivationDriver — Rule 43; the service never sees the name).
struct GearActivation {
    char     input[24]    = {};     ///< named channel; empty = manual only
    uint16_t thresholdUs  = 1500;   ///< Boolean threshold
    uint16_t hysteresisUs = 50;
    bool     invert       = false;  ///< false: above threshold (ON) = DEPLOY (down)
};

/// OPTIONAL transit sounds, played on the dedicated Gear mixer channel:
/// the matching loop starts when any gear begins moving and stops when
/// the whole set settles.  Empty path = silent (no audio for that
/// direction).
struct GearSounds {
    char    deploy[64]  = {};       ///< looped while deploying (gear going down)
    char    retract[64] = {};       ///< looped while retracting (gear going up)
    uint8_t outputMask  = 0x03;     ///< 1 = left, 2 = right, 3 = both
};

/// Parsed form of `/gearcontrol.yaml` — a thin wrapper around the
/// firmware-side `GearDef[]` so apply can hand the array straight to
/// `GearControlServicePolicy::configure()`.
struct GearControlConfig {
    static constexpr uint8_t kSchemaVersion = 2;

    bool enabled = true;
    uint8_t coordMode =
        hubfx::effects::gearctrl::CoordMode::Independent;  ///< cross-channel coordination
    bool deployOnConnectionLoss = false;  ///< item 6: emergency-deploy on input link loss
    GearActivation activation = {};   ///< RC up/down channel (optional)
    GearSounds     sounds     = {};   ///< transit sounds (optional)
    hubfx::effects::gearctrl::GearDef
        gears[hubfx::effects::gearctrl::kMaxGears] = {};
    uint8_t numGears = 0;
};

// ─── ConfigStore adapter ────────────────────────────────────────────

struct GearControlConfigSchema {
    using DataType = GearControlConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        using namespace hubfx::effects::gearctrl;
        using hubfx::config::portRefFromNode;

        d.numGears = 0;
        const auto* root = p.root();
        d.enabled = root ? root->template childAs<bool>("enabled", true) : true;
        d.deployOnConnectionLoss = root
            ? root->template childAs<bool>("deploy_on_connection_loss", false) : false;

        // Cross-channel coordination (v2): independent | door_sync | full_sync | sequenced.
        d.coordMode = CoordMode::Independent;
        if (root) {
            const char* coord = root->template childAs<const char*>("coord", "independent");
            if (coord) {
                if      (std::strcmp(coord, "door_sync") == 0) d.coordMode = CoordMode::DoorSync;
                else if (std::strcmp(coord, "full_sync") == 0) d.coordMode = CoordMode::FullSync;
                else if (std::strcmp(coord, "sequenced") == 0) d.coordMode = CoordMode::Sequenced;
                else                                           d.coordMode = CoordMode::Independent;
            }
        }

        // ── OPTIONAL `input:` block — one RC up/down channel for all gears.
        d.activation = GearActivation{};
        if (const auto* inNode = root ? root->child("input") : nullptr) {
            const char* nm = inNode->template childAs<const char*>("name", "");
            if (nm && nm[0]) std::strncpy(d.activation.input, nm, sizeof(d.activation.input) - 1);
            d.activation.thresholdUs  = (uint16_t)inNode->template childAs<int32_t>("threshold_us", 1500);
            d.activation.hysteresisUs = (uint16_t)inNode->template childAs<int32_t>("hysteresis_us", 50);
            d.activation.invert       = inNode->template childAs<bool>("invert", false);
        }

        // ── OPTIONAL `sounds:` block — transit loops on the Gear channel.
        d.sounds = GearSounds{};
        if (const auto* sNode = root ? root->child("sounds") : nullptr) {
            const char* dep = sNode->template childAs<const char*>("deploy", "");
            const char* ret = sNode->template childAs<const char*>("retract", "");
            if (dep && dep[0]) std::strncpy(d.sounds.deploy,  dep, sizeof(d.sounds.deploy)  - 1);
            if (ret && ret[0]) std::strncpy(d.sounds.retract, ret, sizeof(d.sounds.retract) - 1);
            d.sounds.outputMask = (uint8_t)sNode->template childAs<int32_t>("output_mask", 3);
            if (d.sounds.outputMask == 0 || d.sounds.outputMask > 3) d.sounds.outputMask = 3;
        }

        // ── Strut drive mode (2.45.0) — how the strut STAGE moves.  ONE
        // selector for the whole undercarriage (the three supported setups):
        //   strut_mode: hbridge       — custom DC motor via H-bridge per strut
        //   strut_mode: servo         — one PWM (servo) channel per strut
        //                               (integrated retract controller)
        //   strut_mode: servo_shared  — a SINGLE PWM channel drives ALL struts
        // Servo modes are BLACK BOXES: completion = per-strut (or shared)
        // `travel_ms` fixed time; the door stage waits for it.
        uint8_t strutMode = StrutDrive::HBridge;
        if (root) {
            const char* sm = root->template childAs<const char*>("strut_mode", "hbridge");
            if (sm) {
                if      (std::strcmp(sm, "servo") == 0)        strutMode = StrutDrive::ServoOwn;
                else if (std::strcmp(sm, "servo_shared") == 0) strutMode = StrutDrive::ServoShared;
                else                                           strutMode = StrutDrive::HBridge;
            }
        }
        // servo_shared: strut_shared: { port: {kind: servo, idx}, travel_ms,
        //   deploy_us, retract_us } — ONE channel, ONE pair of positions.
        auto     sharedPort      = portRefFromNode(nullptr);   // empty PortRef
        uint32_t sharedTravelMs  = 5000;
        uint16_t sharedDeployUs  = 0xFFFF;   // defaults → calibrated ends
        uint16_t sharedRetractUs = 0;
        if (strutMode == StrutDrive::ServoShared) {
            const auto* sh = root ? root->child("strut_shared") : nullptr;
            if (sh) {
                sharedPort      = portRefFromNode(sh->child("port"));
                sharedTravelMs  = (uint32_t)sh->template childAs<int32_t>("travel_ms", 5000);
                sharedDeployUs  = (uint16_t)sh->template childAs<int32_t>("deploy_us",  0xFFFF);
                sharedRetractUs = (uint16_t)sh->template childAs<int32_t>("retract_us", 0);
            }
            if (sharedPort.portKind == 0) {
                SFX_LOG_WARN("[gearcontrol-config] strut_mode=servo_shared but "
                             "`strut_shared.port` is missing/invalid — struts will not move");
            }
        }

        const auto* gearsNode = root ? root->child("gears") : nullptr;
        if (!gearsNode || gearsNode->type != YamlNode::Sequence) {
            return true;   // empty table — service stays inert
        }
        const int n = gearsNode->childCount();
        for (int i = 0; i < n && d.numGears < kMaxGears; ++i) {
            const auto* g = gearsNode->childAt(i);
            if (!g) continue;
            GearDef& def = d.gears[d.numGears];

            def.id = (uint8_t)g->template childAs<int32_t>("id", 0);
            const char* nm = g->template childAs<const char*>("name", "");
            std::memset(def.name, 0, sizeof(def.name));
            if (nm && nm[0]) std::strncpy(def.name, nm, sizeof(def.name) - 1);

            def.strutDrive = strutMode;
            if (strutMode == StrutDrive::ServoOwn) {
                // strut_servo: { guid?, kind: servo, idx } + travel_ms per strut
                def.strutServo = portRefFromNode(g->child("strut_servo"));
                def.travelMs   = (uint32_t)g->template childAs<int32_t>("travel_ms", 5000);
                // ABSOLUTE deploy/retract pulses (2.46.0) — absent keys keep
                // the 0xFFFF/0 defaults, which clamp to the calibrated ends.
                def.deployUs   = (uint16_t)g->template childAs<int32_t>("deploy_us",  0xFFFF);
                def.retractUs  = (uint16_t)g->template childAs<int32_t>("retract_us", 0);
                if (def.strutServo.portKind == 0) {
                    SFX_LOG_WARN("[gearcontrol-config] gears[%d] (id=%u): strut_mode=servo "
                                 "but `strut_servo` is missing/invalid", i, (unsigned)def.id);
                    continue;
                }
            } else if (strutMode == StrutDrive::ServoShared) {
                def.strutServo = sharedPort;      // every strut drives the ONE channel
                def.travelMs   = sharedTravelMs;
                def.deployUs   = sharedDeployUs;  // ONE channel ⇒ ONE pair of positions
                def.retractUs  = sharedRetractUs;
                if (sharedPort.portKind == 0) continue;   // warned above
            }

            // motor: { guid?, kind: hbridge, idx } — HBridge mode only.
            def.motor = portRefFromNode(g->child("motor"));
            if (strutMode == StrutDrive::HBridge && def.motor.portKind == 0) {
                SFX_LOG_WARN("[gearcontrol-config] gears[%d] (id=%u): missing/invalid `motor`",
                             i, (unsigned)def.id);
                continue;
            }

            def.reverse   = g->template childAs<bool>("reverse", false);
            def.timeoutMs = (uint32_t)g->template childAs<int32_t>("timeout_ms", 30000);
            // Motor DRIVE voltage — the strut seeks at full scale and the
            // role's cap delivers exactly this average at the motor on any
            // pack (sag-compensated).  The legacy raw deploy_duty /
            // retract_duty keys are RETIRED (2.44.0) and ignored.  Floor at
            // 1 V: 0 would mean "no cap" = full pack voltage into the motor.
            int32_t mv = g->template childAs<int32_t>("motor_voltage_mv", 6000);
            if (mv < 1000) {
                SFX_LOG_WARN("[gearcontrol-config] gears[%d]: motor_voltage_mv=%d "
                             "below the 1 V floor — using 6000", i, (int)mv);
                mv = 6000;
            }
            def.motorVoltageMv = (uint16_t)mv;

            // guard: { mode, ratio_x100, sample_ms, window_ms, threshold_ma,
            //   ceiling_ma } — persisted stall-guard calibration (Studio's
            //   "Save to strut").  Absent block → keep the GearDef defaults
            //   (= the role's LiveRatio default).  Pushed to the motor before
            //   each seek; see Gear::commandGuard.
            if (const auto* gd = g->child("guard")) {
                const char* gm = gd->template childAs<const char*>("mode", "live");
                def.guardMode        = (gm && std::strcmp(gm, "fixed") == 0) ? 0 : 1;
                def.guardRatioX100   = (uint16_t)gd->template childAs<int32_t>("ratio_x100",   250);
                def.guardSampleMs    = (uint16_t)gd->template childAs<int32_t>("sample_ms",    200);
                def.guardWindowMs    = (uint16_t)gd->template childAs<int32_t>("window_ms",     80);
                def.guardThresholdMa = (uint16_t)gd->template childAs<int32_t>("threshold_ma", 1000);
                def.guardCeilingMa   = (uint16_t)gd->template childAs<int32_t>("ceiling_ma",     0);
            }

            // ── Door servos (2.46.0 — explicit absolute positions) ────
            // doors: [ { port: {kind: servo, idx}, open_us: N, close_us: M }, … ]
            // Open/close are ABSOLUTE µs targets set with the endpoints
            // widget; absent keys keep the 0xFFFF/0 defaults, which the
            // servo role clamps to its calibrated ends (the old behaviour).
            def.numDoors = 0;
            const auto* doorsNode = g->child("doors");
            if (doorsNode && doorsNode->type == YamlNode::Sequence) {
                const int dn = doorsNode->childCount();
                for (int di = 0; di < dn && def.numDoors < 2; ++di) {
                    const auto* dNode = doorsNode->childAt(di);
                    if (!dNode) continue;
                    DoorDef& door = def.doors[def.numDoors];
                    door.servo   = portRefFromNode(dNode->child("port"));
                    door.openUs  = (uint16_t)dNode->template childAs<int32_t>("open_us",  0xFFFF);
                    door.closeUs = (uint16_t)dNode->template childAs<int32_t>("close_us", 0);
                    if (door.servo.portKind == 0) {
                        SFX_LOG_WARN("[gearcontrol-config] gears[%d].doors[%d]: missing/invalid `port`",
                                     i, di);
                        continue;
                    }
                    def.numDoors++;
                }
            }

            // door_mode: sync | delay | sequence  (maps onto DoorMode)
            def.openMode = DoorMode::DUAL_SYNC;
            const char* dm = g->template childAs<const char*>("door_mode", "sync");
            if (dm) {
                if      (std::strcmp(dm, "delay")    == 0) def.openMode = DoorMode::DUAL_DELAY;
                else if (std::strcmp(dm, "sequence") == 0) def.openMode = DoorMode::DUAL_SEQ;
                else if (std::strcmp(dm, "single")   == 0) def.openMode = DoorMode::SINGLE;
                else if (std::strcmp(dm, "none")     == 0) def.openMode = DoorMode::NONE;
                else                                       def.openMode = DoorMode::DUAL_SYNC;
            }
            // With one configured door, force SINGLE so a stray DUAL mode
            // doesn't wait on a non-existent door1.
            if (def.numDoors == 1 &&
                (def.openMode == DoorMode::DUAL_SYNC || def.openMode == DoorMode::DUAL_DELAY ||
                 def.openMode == DoorMode::DUAL_SEQ)) {
                def.openMode = DoorMode::SINGLE;
            }
            if (def.numDoors == 0) def.openMode = DoorMode::NONE;

            def.doorDelayMs = (uint16_t)g->template childAs<int32_t>("door_delay_ms", 500);

            // close_policy: both | first | none
            def.closePolicy = ClosePolicy::BOTH;
            const char* cp = g->template childAs<const char*>("close_policy", "both");
            if (cp) {
                if      (std::strcmp(cp, "first") == 0) def.closePolicy = ClosePolicy::FIRST;
                else if (std::strcmp(cp, "none")  == 0) def.closePolicy = ClosePolicy::NONE;
                else                                    def.closePolicy = ClosePolicy::BOTH;
            }

            d.numGears++;
        }
        return true;
    }

    static bool validate(const DataType& /*d*/, char* /*err*/, size_t /*errLen*/) {
        return true;
    }

    static const char* defaultPath() { return "/gearcontrol.yaml"; }
};

#endif  // HUBFX_GEARCONTROL_CONFIG_H
