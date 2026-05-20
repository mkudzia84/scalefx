# 1 "C:\\Users\\marti\\AppData\\Local\\Temp\\tmps4fkm16x"
#include <Arduino.h>
# 1 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
# 32 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
#define FIRMWARE_VERSION "2.3.0-noop"
#define BUILD_NUMBER 100

#include <Arduino.h>
#include <Wire.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <server/board_of.h>


#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <ports/esp_input_port.h>
#include <pwm/pca9685.h>
#include <power/ina226.h>
#include <power/ina226_sensor.h>



#include <storage/bring_up.h>
#include <server/storage_service.h>





#include <audio/audio_mixer.h>
#include <audio/esp_i2s_output.h>
#include <audio/esp_dual_core_audio.h>
#include <audio/upload_exclusivity.h>
#include <codec/tas5825_p_codec.h>
#include <server/audio_service.h>

#include "effects/alerts/alert_service.h"
#include "effects/input/input_dispatcher.h"
#include "effects/landing_lights/landing_light_service.h"
#include "effects/lightfx/lightfx_service.h"
#include "expanders/expander_service.h"
#include "topology/topology_service.h"





namespace Gpio {

    constexpr int I2C_SDA = 8;
    constexpr int I2C_SCL = 9;


    constexpr int LED_CONNECTION = 48;
    constexpr int LED_ERROR = -1;



    constexpr int IN_1 = 5;
    constexpr int IN_2 = 6;
    constexpr int IN_3 = 7;
    constexpr int IN_4 = 10;
    constexpr int IN_5 = 11;
    constexpr int IN_6 = 12;
    constexpr int IN_7 = 13;
    constexpr int IN_8 = 14;
    constexpr int IN_9 = 15;
    constexpr int IN_10 = 4;
    constexpr int IN_11 = 3;
    constexpr int IN_12 = 2;


    constexpr int I2S_DOUT = 16;
    constexpr int I2S_BCLK = 17;
    constexpr int I2S_LRCLK = 18;




    constexpr int SD_CMD = 38;
    constexpr int SD_CLK = 39;
    constexpr int SD_D0 = 40;
    constexpr int SD_D1 = 41;
    constexpr int SD_D2 = 42;
    constexpr int SD_D3 = 45;
}

namespace I2cAddr {
    constexpr uint8_t PCA9685 = 0x70;
    constexpr uint8_t TAS5825P = 0x4C;



    constexpr uint8_t INA226_CH1 = 0x40;
    constexpr uint8_t INA226_CH2 = 0x41;
    constexpr uint8_t INA226_CH3 = 0x42;
    constexpr uint8_t INA226_CH4 = 0x43;
    constexpr uint8_t INA226_CH5 = 0x44;
    constexpr uint8_t INA226_CH6 = 0x45;
    constexpr uint8_t INA226_CH7 = 0x4A;
    constexpr uint8_t INA226_CH8 = 0x4F;
}

namespace Uart {
    constexpr uint8_t IN_1 = 1;
}

namespace Sense {
    constexpr float INA226_SHUNT_OHMS = 0.1f;
    constexpr float INA226_MAX_AMPS = 3.2f;
}

namespace Pwm {


    constexpr uint16_t FREQ_HZ = 1526;
}

namespace Codec {

    constexpr auto SUPPLY_VOLTAGE = sfx_audio::tas5825::Supply::V12;
}



#include "effects/audio_codec.h"



using Mixer = AudioMixer<EspI2SOutput, TAS5825PCodec>;
using AudioService = AudioServicePolicy<Mixer>;
using AlertService = hubfx::effects::alerts::AlertServicePolicyT<Mixer>;



using HubFxExpanderService = hubfx::expanders::ExpanderServicePolicyT<2, 4>;
using HubFxTopologyService =
    hubfx::topology::TopologyServicePolicyT<HubFxExpanderService>;






using InputDispatcherService =
    hubfx::effects::input::InputDispatcherServicePolicyT<HubFxTopologyService>;







using LandingLightService =
    hubfx::effects::landing::LandingLightServicePolicyT<HubFxTopologyService>;





using LightFxEffectService =
    hubfx::effects::lightfx::LightFxEffectServicePolicyT<HubFxTopologyService,
                                                          LandingLightService>;






class HubFxBoard : public sfx_core::BoardOf<HubFxBoard,
                                             HubFxExpanderService,
                                             HubFxTopologyService,
                                             InputDispatcherService,
                                             LandingLightService,
                                             LightFxEffectService,
                                             StorageService,
                                             AudioService,
                                             AlertService> {
public:

    PCA9685 pca;
    INA226 ina[8];

    sfx_peripherals::Pca9685PwmPort pwm[8] = {
        {pca, 0}, {pca, 1}, {pca, 2}, {pca, 3},
        {pca, 4}, {pca, 5}, {pca, 6}, {pca, 7},
    };
    sfx_peripherals::Ina226VoltageSensor vSense[8] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]},
        {ina[4]}, {ina[5]}, {ina[6]}, {ina[7]},
    };
    sfx_peripherals::Ina226CurrentSensor iSense[8] = {
        {ina[0]}, {ina[1]}, {ina[2]}, {ina[3]},
        {ina[4]}, {ina[5]}, {ina[6]}, {ina[7]},
    };


    sfx_peripherals::EspInputPort in[1] = {
        {Gpio::IN_1, Uart::IN_1},
    };


    sfx_peripherals::MicroservoPort servoOut[11] = {
        {Gpio::IN_2}, {Gpio::IN_3}, {Gpio::IN_4},
        {Gpio::IN_5}, {Gpio::IN_6}, {Gpio::IN_7},
        {Gpio::IN_8}, {Gpio::IN_9}, {Gpio::IN_10},
        {Gpio::IN_11}, {Gpio::IN_12},
    };






    struct InaProbe {
        uint8_t addr = 0;
        uint8_t wireAck = 0xFF;
        bool begun = false;
        bool idMatches = false;
        uint16_t mfgId = 0;
        uint16_t dieId = 0;
    };
    struct HwInitState {
        bool pcaPreAck = false;
        bool pcaBegun = false;
        uint8_t pcaMode1 = 0xFF;
        uint8_t pcaMode2 = 0xFF;
        uint8_t pcaPrescale = 0xFF;




        bool pcaAfterCh[8] = {false};




        bool pcaPostInitAckRaw = false;
        bool pcaPostInitAck = false;
        uint8_t pcaPostInitMode1 = 0xFF;
        InaProbe ina[8];
    };
    HwInitState hwInit{};
# 286 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
    void initHardware() {
        Wire.begin(Gpio::I2C_SDA, Gpio::I2C_SCL);
        Wire.setClock(400000);







        PCA9685::broadcastReset(Wire);
        delay(2);


        hwInit.pcaPreAck = pcaBusProbe(I2cAddr::PCA9685);
        hwInit.pcaBegun = pca.begin(Wire, I2cAddr::PCA9685, Pwm::FREQ_HZ);
        if (hwInit.pcaBegun) {
            hwInit.pcaMode1 = pcaRead(I2cAddr::PCA9685, 0x00);
            hwInit.pcaMode2 = pcaRead(I2cAddr::PCA9685, 0x01);
            hwInit.pcaPrescale = pcaRead(I2cAddr::PCA9685, 0xFE);
# 314 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
            for (uint8_t k = 0; k < 8; k++) {
                pca.setChannel(k, 0);
                hwInit.pcaAfterCh[k] = pcaBusProbe(I2cAddr::PCA9685);
            }
        }
# 328 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
        constexpr uint8_t kInaAddrs[8] = {
            I2cAddr::INA226_CH1, I2cAddr::INA226_CH2,
            I2cAddr::INA226_CH3, I2cAddr::INA226_CH4,
            I2cAddr::INA226_CH5, I2cAddr::INA226_CH6,
            I2cAddr::INA226_CH7, I2cAddr::INA226_CH8,
        };
        for (uint8_t k = 0; k < 8; k++) {
            hwInit.ina[k].addr = kInaAddrs[k];
            Wire.beginTransmission(kInaAddrs[k]);
            hwInit.ina[k].wireAck = Wire.endTransmission();
            if (hwInit.ina[k].wireAck != 0) continue;

            hwInit.ina[k].begun =
                ina[k].begin(Wire, kInaAddrs[k],
                             Sense::INA226_SHUNT_OHMS, Sense::INA226_MAX_AMPS);



            hwInit.ina[k].mfgId = ina[k].bootMfgId();
            hwInit.ina[k].dieId = ina[k].bootDieId();
            hwInit.ina[k].idMatches = ina[k].isCanonical();
        }
    }
# 359 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
    void logHardwareStatus() {
        SFX_LOG_INFO("[I2C] Wire up: SDA=GP%d SCL=GP%d @ 400 kHz",
                     Gpio::I2C_SDA, Gpio::I2C_SCL);
# 371 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
        hwInit.pcaPostInitAck = pcaBusProbe(I2cAddr::PCA9685);
        hwInit.pcaPostInitAckRaw = hwInit.pcaPostInitAck;
        hwInit.pcaPostInitMode1 = pcaRead(I2cAddr::PCA9685, 0x00);
        if (!hwInit.pcaPostInitAck) {
            SFX_LOG_WARN("[PCA] post-init probe failed — chip wedged during "
                         "board.begin() port-init.  Running recovery: SWRST "
                         "→ re-init → re-push duties.");
            PCA9685::broadcastReset(Wire);
            delay(2);
            const bool reinitOk = pca.begin(Wire, I2cAddr::PCA9685, Pwm::FREQ_HZ);
            if (reinitOk) {
                for (uint8_t k = 0; k < 8; k++) {
                    pwm[k].setDuty(pwm[k].duty());
                }
            }
            hwInit.pcaPostInitAck = pcaBusProbe(I2cAddr::PCA9685);
            hwInit.pcaPostInitMode1 = pcaRead(I2cAddr::PCA9685, 0x00);
            if (hwInit.pcaPostInitAck) {
                SFX_LOG_WARN("[PCA] recovery OK — chip back at 0x%02X "
                             "MODE1=0x%02X (reinit %s)",
                             I2cAddr::PCA9685, hwInit.pcaPostInitMode1,
                             reinitOk ? "succeeded" : "FAILED");
            } else {
                SFX_LOG_ERROR("[PCA] recovery FAILED — chip still silent "
                              "after SWRST + re-init.  PWM unavailable.");
            }
        }

        if (!hwInit.pcaPreAck) {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: NO ACK on pre-begin probe",
                          I2cAddr::PCA9685);
        }
        if (!hwInit.pcaBegun) {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: begin() failed", I2cAddr::PCA9685);
        } else {



            SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: %u Hz  MODE1=0x%02X MODE2=0x%02X PRESCALE=0x%02X",
                         I2cAddr::PCA9685, Pwm::FREQ_HZ,
                         hwInit.pcaMode1, hwInit.pcaMode2, hwInit.pcaPrescale);





            uint8_t wedgedAt = 0xFF;
            for (uint8_t k = 0; k < 8; k++) {
                if (!hwInit.pcaAfterCh[k]) { wedgedAt = k; break; }
            }
            if (wedgedAt == 0xFF) {
                SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: 8/8 per-channel writes ACKed "
                             "(no address-comparator wedge)", I2cAddr::PCA9685);
            } else {
                SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: address comparator WEDGED "
                              "after pre-fire setChannel(%u, 0) — earlier %u "
                              "channel writes ACKed.  Investigate driver MODE1 "
                              "/ ALLCALLADR state.",
                              I2cAddr::PCA9685, (unsigned)wedgedAt,
                              (unsigned)wedgedAt);
            }

        }
        if (hwInit.pcaPostInitAck) {
            SFX_LOG_INFO("[PCA] PCA9685 @ 0x%02X: post-board.begin() OK  MODE1=0x%02X",
                         I2cAddr::PCA9685, hwInit.pcaPostInitMode1);
        } else {
            SFX_LOG_ERROR("[PCA] PCA9685 @ 0x%02X: WENT SILENT after board.begin() — "
                          "a PortServicePolicy port->begin() write may have wedged the chip.",
                          I2cAddr::PCA9685);
        }

        uint8_t inaOk = 0;
        uint8_t inaClones = 0;
        for (uint8_t k = 0; k < 8; k++) {
            const auto& p = hwInit.ina[k];
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




    void pollSense() {
        for (uint8_t k = 0; k < 8; k++) ina[k].update();
    }




    static constexpr auto kPwmPorts = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&HubFxBoard::pwm, 8>()
            .with_vSense_array<&HubFxBoard::vSense>()
            .with_iSense_array<&HubFxBoard::iSense>());

    static constexpr auto kInputPorts = sfx_core::ports::list(
        sfx_core::ports::input_array<&HubFxBoard::in, 1>());

    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&HubFxBoard::servoOut, 11>());

    static constexpr const char* kName = "HubFx";
};

HubFxBoard board;




static EspDualCoreAudio<Mixer> audio;






static hubfx::effects::lightfx::Program kLightFxPrograms[1] = {};
static void buildDefaultLightFxPrograms();
void setup();
void loop();
#line 533 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
static void buildDefaultLightFxPrograms() {
    using namespace hubfx::effects::lightfx;
    using hubfx::effects::PortRef;
    Program& p = kLightFxPrograms[0];
    std::strncpy(p.name, "AllOn", sizeof(p.name) - 1);
    p.numChannels = 8;
    for (uint8_t ch = 0; ch < 8; ++ch) {
        LedChannelSpec& spec = p.channels[ch];
        spec.addr = PortRef::local(PortKind::Pwm, ch);
        spec.perChannelBrightnessPct = 100;
        spec.events[0] = LightEvent::on( 100,
                                                                     0);
        spec.numEvents = 1;
    }
    p.numLandings = 0;
}

void setup() {



    board.initHardware();



    bringUpStorage({
        .clk = Gpio::SD_CLK, .cmd = Gpio::SD_CMD,
        .d0 = Gpio::SD_D0, .d1 = Gpio::SD_D1,
        .d2 = Gpio::SD_D2, .d3 = Gpio::SD_D3,
    });




    buildDefaultLightFxPrograms();
    board.policy<LightFxEffectService>().configure(kLightFxPrograms, 1);





    board.begin(FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);
    board.setConnectionTimeoutEnabled(false);
# 586 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
    {
        using hubfx::effects::PortRef;
        auto& topo = board.policy<HubFxTopologyService>();
        uint8_t attached = 0;
        for (uint8_t ch = 0; ch < 8; ++ch) {
            if (topo.attachRole(PortRef::local(PortKind::Pwm, ch),
                                RoleKind::LedAnimator)) {
                ++attached;
            }
        }
        SFX_LOG_INFO("[LightFx] attached LedAnimator to %u/8 hub PWM ports",
                     (unsigned)attached);
    }
# 607 "C:/data/code/scalefx/controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino"
    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);




    board.logHardwareStatus();



    board.enableI2CScan(Wire);
    board.addExpectedI2CDevice(I2cAddr::TAS5825P);
    board.addExpectedI2CDevice(I2cAddr::PCA9685);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH1);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH2);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH3);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH4);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH5);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH6);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH7);
    board.addExpectedI2CDevice(I2cAddr::INA226_CH8);




    audio.begin({
        .i2sDout = Gpio::I2S_DOUT,
        .i2sBclk = Gpio::I2S_BCLK,
        .i2sLrclk = Gpio::I2S_LRCLK,
    });




    wireUploadExclusivity<Mixer>(board.policy<StorageService>());




    {
        hubfx::effects::alerts::AlertServiceConfig cfg;
        cfg.enabled = true;
        cfg.channel = hubfx::effects::audio::HubFxLayout::Alert;
        strncpy(cfg.info.path,
                "/sounds/sys/hubfx_initialized.wav", sizeof(cfg.info.path) - 1);
        strncpy(cfg.warning.path,
                "/sounds/sys/lightfx_detected.wav", sizeof(cfg.warning.path) - 1);
        strncpy(cfg.error.path,
                "/sounds/sys/lightfx_fw_error.wav", sizeof(cfg.error.path) - 1);
        strncpy(cfg.critical.path,
                "/sounds/sys/gunfx_fw_error.wav", sizeof(cfg.critical.path) - 1);
        cfg.info.volume = 70;
        cfg.warning.volume = 80;
        cfg.error.volume = 90;
        cfg.critical.volume = 100;
        board.policy<AlertService>().configure(cfg);
    }





    auto& exp = board.policy<HubFxExpanderService>();
    exp.onConnect([](const hubfx::expanders::ExpanderEntry& e) {
        SFX_LOG_INFO("[Expander] mount %s addr=%u vid=%04X pid=%04X",
                     hubfx::expanders::ExpanderKind::getName(e.kind),
                     e.usbAddr, e.vid, e.pid);
    });
    exp.onIdentified([](const hubfx::expanders::ExpanderEntry& e) {
        SFX_LOG_INFO("[Expander] identified %s guid=%s v%s",
                     hubfx::expanders::ExpanderKind::getName(e.kind),
                     e.spec.guid, e.spec.firmwareVersion);
    });
    exp.onDisconnect([](const hubfx::expanders::ExpanderEntry& e) {
        SFX_LOG_INFO("[Expander] unmount %s addr=%u guid=%s",
                     hubfx::expanders::ExpanderKind::getName(e.kind),
                     e.usbAddr, e.spec.guid);
    });



    SFX_LOG_INFO("[InputDispatcher] up — %u/%u bindings",
                 (unsigned)board.policy<InputDispatcherService>().numBindings(),
                 (unsigned)InputDispatcherService::kMaxBindings);
    SFX_LOG_INFO("[LandingLight] up — %u/%u lights configured",
                 (unsigned)board.policy<LandingLightService>().count(),
                 (unsigned)hubfx::effects::landing::kMaxLandingLights);
    SFX_LOG_INFO("[LightFx] up — 1 program configured (\"AllOn\", 8 PWM channels)");

    SFX_LOG_INFO("HubFX v%s build %u — 8 PWM / 1 input / 11 servo-out + storage + audio + alerts + USB host + topology + input-dispatcher + landing + lightfx",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {





    auto& storage = board.policy<StorageService>();
    if (storage.isUploadActive()) {
        if (storage.isStreamActive()) {
            storage.processStream();
        } else {
            board.process();
        }
        storage.checkUploadTimeout();
        return;
    }

    board.process();
    storage.checkUploadTimeout();
    board.pollSense();
    vTaskDelay(pdMS_TO_TICKS(1));
}