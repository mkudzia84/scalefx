/*
 * HubFX ESP32-S3 — Empty Entry Point
 *
 * Fresh restart on top of the consolidated board framework:
 *   - BoardServer<...UserPolicies>  (sfx_board/server/board_server.h)
 *       composes BoardServicePolicy + IndicatorServicePolicy + the
 *       user's application policies, owns the COBS frame loop and
 *       lifecycle wiring.  One class, no wrappers around wrappers.
 *
 * Reference implementation: controllers/archive/hubfx-esp32s3/  (frozen).
 * Subsystems (audio, storage, USB host, RC inputs, engine FX, local LED
 * runtime, config store, battery) will be added back one policy at a
 * time as the new HubFX-specific code lands in this directory.
 */

#define FIRMWARE_VERSION "2.0.0"
#define BUILD_NUMBER 17

#include <Arduino.h>

#include <platform/sfx_platform.h>
#include <serial/diag_log.h>
#include <serial/serial.h>
#include <server/board_server.h>

using HubFxBoard = sfx_core::BoardServer<>;
HubFxBoard board;

// Indicator LEDs (DevKitC-1 onboard RGB at GPIO48; no external error LED).
#define PIN_LED_CONNECTION  48
#define PIN_LED_ERROR       -1

void setup() {
    board.begin("HubFx", FIRMWARE_VERSION, BUILD_NUMBER,
                PIN_LED_CONNECTION, PIN_LED_ERROR);
    // HubFX is the master — no upstream watchdog.
    board.setConnectionTimeoutEnabled(false);

    SFX_LOG_INFO("HubFX v%s build %u — empty entry point",
                 FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    board.process();
}
