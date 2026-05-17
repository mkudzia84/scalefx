/*
 * Serial Library — Unified Serial Communication for ScaleFX
 *
 * Master include file for the complete ScaleFX serial communication library.
 * Include this file for full functionality, or include specific headers for
 * minimal dependencies.
 *
 * Architecture:
 *   core/core.h               - Protocol, error codes, CommandResult, SFX macros
 *   core/system_service.h     - SystemServicePolicy concept + BoardServer<...>
 *   core/board_service.h      - BoardServicePolicy (lifecycle/identify/status)
 *   core/packet_reader.h      - PacketReader<TDispatch> (COBS framer)
 *   core/stream.h             - StreamWriter + StreamProtocol (chunked streaming, CRC-16)
 *   client/bus.h              - SerialBus (client-side, COBS-framed protocol)
 *   client/bus_client.h       - BusClient base class (client-side, extends SerialBus)
 *   client/result_queue.h     - ResultQueue (tag-correlated command/response matching)
 *
 * Protocol Implementations:
 *   gunfx/gunfx.h             - GunFX client/server (muzzle flash, servo, smoke)
 *   lightfx/lightfx.h         - LightFX client/server (LED, servo, power)
 *   gearcontrol/gearcontrol.h - GearControl client/server (landing gear, servo, yaw)
 *   hubfx/hubfx.h             - HubFX audio/storage client/server (NOT auto-included — heavy deps)
 *
 * Client vs Server:
 *   Client (HubFX): UsbHost (sfx_usb) + BusClient for USB Host CDC communication
 *   Server side:    `BoardServer<...Policies>` + `PacketReader` wrapped by
 *                   `SfxServer<...UserPolicies>` (sfx_server library).
 *
 * Usage:
 *   #include <serial/serial.h>                    // Everything (except hubfx)
 *   #include <serial/gunfx/gunfx.h>               // GunFX only
 *   #include <serial/lightfx/lightfx.h>           // LightFX only
 *   #include <serial/gearcontrol/gearcontrol.h>   // GearControl only
 *   #include <serial/hubfx/hubfx.h>               // HubFX audio/storage (heavy deps)
 *   #include <serial/core/core.h>                 // Just protocol utilities
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

// Server-side framework: variadic policy composer + COBS frame reader.
#include "core/system_service.h"
#include "core/board_service.h"
#include "core/packet_reader.h"

// Chunked data streaming over COBS (server-side, writes via ServiceContext)
#include "core/stream.h"

// Diagnostic log output (ring-buffered, COBS-encoded log packets)
#include "platform/diag_log.h"

// GunFX binary implementation
#include "gunfx/gunfx.h"

// LightFX binary implementation
#include "lightfx/lightfx.h"

// GearControl binary implementation
#include "gearcontrol/gearcontrol.h"

// HubFX audio/storage: NOT auto-included — heavy SdFat/LittleFS dependencies.
// Include explicitly with: #include <serial/hubfx/hubfx.h>

#endif // SERIAL_H
