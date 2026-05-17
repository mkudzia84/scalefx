/*
 * HubFX ESP32-S3 — Empty Entry Point (port + role framework ready)
 *
 * Built on top of `BoardOf<HubFxBoard>` — the consolidated board
 * framework that composes BoardServicePolicy + IndicatorServicePolicy
 * + PortServicePolicy + RoleServicePolicy and reads the board's static
 * port descriptors at begin() time.
 *
 * The hardware below (PCA9685 @ 0x70 / INA226 / etc.) is intentionally
 * NOT declared yet — this is the empty starting point.  Subsystems
 * come back online one port at a time as the new HubFX-specific code
 * lands in src/.
 *
 * Reference (frozen): controllers/archive/hubfx-esp32s3/
 */

#define FIRMWARE_VERSION "2.1.0"
#define BUILD_NUMBER 3

#include <Arduino.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <serial/serial.h>

#include <server/board_of.h>

class HubFxBoard : public sfx_core::BoardOf<HubFxBoard> {
public:
    // Hardware drivers go here, member-initialised:
    //
    //   Pca9685             pca       {Wire, 0x70};
    //   Pca9685PwmPort      pwm_local0{pca, 0};
    //   MicroservoPort      servo0    {GPIO_2};
    //   DualPwmHBridgePort  motor0    {pwm_local0, pwm_local1};
    //   Ina226              ina       {Wire, 0x40};
    //   Ina226CurrentSensor iSense0   {ina};
    //
    // Then declare them:
    //
    //   static constexpr auto kPwmPorts = sfx_core::ports::list(
    //       sfx_core::ports::pwm<&HubFxBoard::pwm_local0>()
    //           .with_iSense<&HubFxBoard::iSense0>());

    static constexpr const char* kName = "HubFx";
};

HubFxBoard board;

// Indicator LEDs (DevKitC-1 onboard RGB at GPIO48; no external error LED).
#define PIN_LED_CONNECTION  48
#define PIN_LED_ERROR       -1

void setup() {
    board.begin(FIRMWARE_VERSION, BUILD_NUMBER, PIN_LED_CONNECTION, PIN_LED_ERROR);
    board.setConnectionTimeoutEnabled(false);     // master — no upstream watchdog
    SFX_LOG_INFO("HubFX v%s build %u — empty entry point (port framework ready)",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    board.process();
}
