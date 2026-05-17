/*
 * Serial Library — wire protocol + client side.
 *
 * sfx_serial owns the wire primitives (CRC-8, COBS, packet layout),
 * the client-side framework (SerialBus / BusClient / ResultQueue), and
 * the generic-expander wire protocol (ComponentPacket).  Domain protocol
 * headers live with their owning firmware now — HubFX's master wire
 * format moves into the HubFX controller (the only consumer is the Go
 * SDK / Studio anyway).
 *
 * Server-side framework (BoardServer<...Policies>, BoardServicePolicy,
 * PacketReader<TDispatch>, StreamWriter) lives in the sfx_board
 * library — `<server/sfx_server.h>` and friends.  This file deliberately
 * does NOT pull those in (sfx_board depends on sfx_serial, not the
 * other way around).
 *
 * Architecture:
 *   core/core.h               - Protocol, error codes, CommandResult, SFX macros
 *   wire.{h,cpp}              - Stateless wire encoding (CRC-8, COBS, encode/parse)
 *   diag_log.{h,cpp}          - DiagLog ring buffer + COBS-encoded log packets
 *   client/bus.h              - SerialBus (client-side, COBS-framed protocol)
 *   client/bus_client.h       - BusClient base class (client-side, extends SerialBus)
 *   client/result_queue.h     - ResultQueue (tag-correlated command/response matching)
 *   components/components.h   - Generic-expander wire protocol (ComponentPacket / kind /
 *                               led_status — slave-board redesign per
 *                               instructions/15-GENERIC-EXPANDER-REFACTOR.md)
 *
 * Archived domain protocols (controllers/archive/sfx_serial_legacy/):
 *   gunfx/        — GunFX wire (0x01-0x2F)
 *   lightfx/      — LightFX wire (0x40-0x5F)
 *   gearcontrol/  — GearControl wire (0x60-0x7F)
 *   hubfx/        — HubFX master wire (0x80-0xAF) — Go-side mirror is the
 *                   live source of truth at app/go/protocol/hubfx/
 *
 * Usage:
 *   #include <serial/serial.h>          // wire + client + components
 *   #include <serial/core/core.h>       // just protocol utilities
 *   #include <server/sfx_server.h>      // server-side composer (sfx_board lib)
 */

#ifndef SERIAL_H
#define SERIAL_H

// Core protocol, error codes, CommandResult, SFX macros
#include "core/core.h"

// Result queue for tag-correlated command/response matching
#include "client/result_queue.h"

// NOTE: USB Host (usb/sfx_usb_host.h) and USB Registry (usb/usb_registry.h)
// are in the sfx_usb library. Include them separately if needed.

// Binary protocol implementation (client-side, low-level COBS framing)
#include "client/bus.h"

// Bus client base class (client-side, extends SerialBus with tag queue + INIT)
#include "client/bus_client.h"

// Server-side framework (BoardServer<...Policies>, BoardServicePolicy,
// PacketReader, StreamWriter) lives in the sfx_server library — include
// `<server/sfx_server.h>` (or the individual `<server/...>` headers)
// from server-side firmware.  sfx_serial intentionally does NOT pull
// those in (would invert the dep direction).

// Diagnostic log output (ring-buffered, COBS-encoded log packets)
#include "diag_log.h"

// Generic-expander wire protocol (component kinds, LED status,
// ComponentPacket commands).  Replaces the per-board protocol headers
// (gunfx/lightfx/gearcontrol) that were archived 2026-05-17.
#include "components/components.h"
#include "components/component_kind.h"
#include "components/led_status.h"

#endif // SERIAL_H
