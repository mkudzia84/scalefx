/*
 * role_command.h — shared builders for the common role-command payloads.
 *
 * Effects drive role ports through their topology-forwarder callback with a
 * tiny payload, almost always `[portIdx][value]`. That shape was hand-rolled
 * (build the buffer, set buf[0]=portIdx, putU16LE, call _send) at ~a dozen
 * sites across GunFx / LandingFx / LightFx — the same `[portIdx]+endianness`
 * marshalling that bred the servo-profile drift bug. These helpers own the
 * layout once so a call site is one line and can't get the offset/endianness
 * wrong.
 *
 * Templated on the send callback so each effect's own `SendRoleCmdFn` typedef
 * binds without a type-identity dance; the signature is
 *   bool send(void* ctx, const PortRef& addr, uint8_t innerType,
 *             const uint8_t* payload, size_t len)
 *
 * Variable-length payloads (LED queue load, servo recoil = [i16][u16]) stay
 * hand-coded at their call site — these helpers cover only the fixed
 * single-value shapes.
 */

#ifndef HUBFX_ROLE_COMMAND_H
#define HUBFX_ROLE_COMMAND_H

#include <cstddef>
#include <cstdint>

#include <serial/core/core.h>     // SfxWire::putU16LE / putI16LE
#include "effect_id.h"            // hubfx::effects::PortRef

namespace hubfx::effects::rolecmd {

/// `[portIdx][value:u16LE]` — the most common role command (servo target /
/// pos-norm, heater target, …).
template <typename SendFn>
inline bool u16(SendFn send, void* ctx, const PortRef& port,
                uint8_t inner, uint16_t value) {
    if (!send) return false;
    uint8_t p[3] = { port.portIdx, 0, 0 };
    SfxWire::putU16LE(&p[1], value);
    return send(ctx, port, inner, p, sizeof(p));
}

/// `[portIdx][value:i16LE]` — signed single value.
template <typename SendFn>
inline bool i16(SendFn send, void* ctx, const PortRef& port,
                uint8_t inner, int16_t value) {
    if (!send) return false;
    uint8_t p[3] = { port.portIdx, 0, 0 };
    SfxWire::putI16LE(&p[1], value);
    return send(ctx, port, inner, p, sizeof(p));
}

/// `[portIdx][value:u8]` — motor pct, LED brightness, …
template <typename SendFn>
inline bool u8(SendFn send, void* ctx, const PortRef& port,
               uint8_t inner, uint8_t value) {
    if (!send) return false;
    const uint8_t p[2] = { port.portIdx, value };
    return send(ctx, port, inner, p, sizeof(p));
}

/// `[portIdx]` — no-arg role command (LED start / stop, …).
template <typename SendFn>
inline bool none(SendFn send, void* ctx, const PortRef& port, uint8_t inner) {
    if (!send) return false;
    const uint8_t p[1] = { port.portIdx };
    return send(ctx, port, inner, p, sizeof(p));
}

}  // namespace hubfx::effects::rolecmd

#endif  // HUBFX_ROLE_COMMAND_H
