/*
 * led_program.h — data structures for a LightFX program.
 *
 *   A `LedChannelSpec` is one LED channel inside a program: a
 *   `PortRef` addressing the (board, port) target and a small list of
 *   `LightEvent`s that get serialized into the LED_QUEUE_LOAD payload
 *   when the program activates.  `perChannelBrightnessPct` is the
 *   per-channel scale baked into the queue; the master brightness
 *   (held by `LightController`) is applied on top via LED_SET_BRIGHTNESS.
 *
 *   A `Program` is a name + array of `LedChannelSpec`s + array of
 *   `LandingLightBinding`s (which landing-light ID should be ON / OFF
 *   while this program is active).  Programs are mutually exclusive
 *   inside the LightFX effect.
 *
 *   The diff-based program select algorithm lives in `light_controller.h`.
 *   Storage of these structs is intentionally fixed-size — no heap on
 *   embedded.
 */

#ifndef HUBFX_LED_PROGRAM_H
#define HUBFX_LED_PROGRAM_H

#include <cstdint>
#include <cstring>

#include "../effect_id.h"           // PortRef
#include "light_event.h"

namespace hubfx::effects::lightfx {

inline constexpr uint8_t kMaxEventsPerChannel = 16;
inline constexpr uint8_t kMaxChannelsPerProgram = 16;
inline constexpr uint8_t kMaxLandingBindingsPerProgram = 8;
inline constexpr uint8_t kMaxPrograms = 8;

/// Program name buffer (incl. NUL).  Must equal the program-selector's
/// range copy (`kSelectorProgramMax` in lightfx_config.h) so a selector
/// range name can match a program name in full — a static_assert in
/// lightfx_program_selector.h pins them together.  23 usable chars is
/// enough for descriptive names like "helicopter_landing".
inline constexpr uint8_t kProgramNameMax = 24;

/// One LED channel inside a program.
struct LedChannelSpec {
    PortRef    addr;                                ///< (guid, PortKind::Pwm, idx)
    LightEvent events[kMaxEventsPerChannel] = {};
    uint8_t    numEvents              = 0;
    uint8_t    perChannelBrightnessPct = 100;       ///< 0..100, default = full
};

/// Landing-light reference inside a program — by ID, not by port.
/// `state == 0` → OFF, `state == 1` → ON for the duration of the program.
struct LandingLightBinding {
    uint8_t id    = 0;
    uint8_t state = 0;   ///< LandingLightState::Off (0) / On (1)
};

/// One named program.  Mutually exclusive within the LightFX effect.
struct Program {
    char                name[kProgramNameMax]                          = {};
    LedChannelSpec      channels[kMaxChannelsPerProgram]                = {};
    uint8_t             numChannels                                    = 0;
    LandingLightBinding landings[kMaxLandingBindingsPerProgram]        = {};
    uint8_t             numLandings                                    = 0;
};

}  // namespace hubfx::effects::lightfx

#endif  // HUBFX_LED_PROGRAM_H
