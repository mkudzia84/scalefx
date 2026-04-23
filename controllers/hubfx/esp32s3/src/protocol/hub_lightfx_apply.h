/*
 * Hub-side LightFX config push
 *
 * Walks a loaded LightProgramConfig and pushes it to the connected LightFX
 * slave over the wire (LightFxClient / USB CDC). Replaces the old applier +
 * router + orchestrator + policy stack — there is exactly one LightFX slave
 * per hub, so a generic-callable fanout adds no value here.
 *
 * Apply order (must match LightFX standalone's local apply path so the slave
 * boots into the same state regardless of who holds the YAML):
 *   1. Master brightness    — bounds-independent; goes first.
 *   2. Per-servo settings   — limits + speed/accel + reversed + drive to default_us.
 *   3. Landing groups       — unbind all, then bind every populated slot.
 *
 * Returns true on full success. On the first non-zero wire response, logs
 * the failing operation + slot + error code (LightFxError or transport
 * SerialError, both uint8_t in the same numeric space) and returns false.
 *
 * Programs / input bands are not pushed yet — when LightProgramManager gains
 * load/select wire commands, extend this function with the matching loops.
 */

#ifndef HUB_LIGHTFX_APPLY_H
#define HUB_LIGHTFX_APPLY_H

#include <lightfx/client/lightfx_client.h>
#include <config/light_program_config.h>

bool pushLightFxConfigToSlave(const LightProgramConfig& cfg, LightFxClient& client);

#endif // HUB_LIGHTFX_APPLY_H
