/*
 * Serial Library - Unified Serial Communication for ScaleFX
 *
 * Master include file for the complete ScaleFX serial communication library.
 * Include this file for full functionality, or include specific headers for
 * minimal dependencies.
 *
 * Architecture:
 *   serial_core.h            - Protocol, error codes, CommandResult, ICommandHandler, CommandRouter, SFX macros
 *   serial_usb_host.h        - UsbHost (client-side, USB Host PIO-USB manager)
 *   serial_bus.h             - SerialBus (client-side, COBS-framed protocol)
 *   serial_bus_client.h      - BusClient base class (client-side, extends SerialBus)
 *   serial_bus_server.h      - BusServer + CoreCommandServer (server-side, extends ICommandHandler)
 *   serial_stream.h          - StreamWriter + StreamProtocol (chunked streaming, CRC-16)
 *
 * Protocol Implementations:
 *   serial_gunfx.h           - GunFX client/server (muzzle flash, servo, smoke)
 *   serial_lightfx.h         - LightFX client/server (LED, servo, power)
 *   serial_gearcontrol.h     - GearControl client/server (landing gear, servo, yaw)
 *   serial_hubfx.h           - HubFX audio/storage client/server (NOT auto-included — heavy deps)
 *
 * Client vs Server:
 *   Client (HubFX): Uses UsbHost + BusClient for USB Host CDC communication
 *   Server (Pico):  Uses Serial (USB Device) + BusServer + CoreCommandServer + CommandRouter
 *
 * Usage:
 *   #include <serial/serial.h>              // Everything
 *   #include <serial/gunfx/gunfx.h>        // GunFX only
 *   #include <serial/lightfx/lightfx.h>      // LightFX only
 *   #include <serial/gearcontrol/gearcontrol.h>  // GearControl only
 *   #include <serial/hubfx/hubfx.h>              // HubFX audio/storage only (requires SdFat, LittleFS)
 *   #include <serial/core/core.h>         // Just protocol utilities
 */

#ifndef SERIAL_H
#define SERIAL_H

// Core protocol, error codes, CommandResult, interfaces, command routing
#include "core/core.h"

// Result queue for tag-correlated command/response matching
#include "client/result_queue.h"

// USB Host singleton for CDC devices (client-side, platform-conditional)
#include "usb/usb_host.h"

// USB Device Registry — tracks connected slave controllers
#include "usb/usb_registry.h"

// Binary protocol implementation (client-side, low-level COBS framing)
#include "client/bus.h"

// Bus client base class (client-side, extends SerialBus with tag queue + INIT)
#include "client/bus_client.h"

// Bus server base class (server-side, extends ICommandHandler with ACK/NACK)
#include "core/bus_server.h"

// Chunked data streaming over COBS (server-side, uses BusServer for output)
#include "core/stream.h"

// Diagnostic log output (ring-buffered, COBS-encoded log packets)
#include "core/diag_log.h"

// GunFX binary implementation
#include "gunfx/gunfx.h"

// LightFX binary implementation
#include "lightfx/lightfx.h"

// GearControl binary implementation
#include "gearcontrol/gearcontrol.h"

// HubFX audio/storage: NOT auto-included — heavy SdFat/LittleFS dependencies.
// Include explicitly with: #include <serial/hubfx/hubfx.h>

#endif // SERIAL_H
