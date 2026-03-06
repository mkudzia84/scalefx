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
 *
 * Protocol Implementations:
 *   serial_gunfx.h           - GunFX client/server (muzzle flash, servo, smoke)
 *   serial_lightfx.h         - LightFX client/server (LED, servo, power)
 *   serial_gearcontrol.h     - GearControl client/server (landing gear, servo, yaw)
 *
 * Client vs Server:
 *   Client (HubFX): Uses UsbHost + BusClient for USB Host CDC communication
 *   Server (Pico):  Uses Serial (USB Device) + BusServer + CoreCommandServer + CommandRouter
 *
 * Usage:
 *   #include <serial.h>              // Everything
 *   #include <serial_gunfx.h>        // GunFX only
 *   #include <serial_lightfx.h>      // LightFX only
 *   #include <serial_gearcontrol.h>  // GearControl only
 *   #include <serial_core.h>         // Just protocol utilities
 */

#ifndef SERIAL_H
#define SERIAL_H

// Core protocol, error codes, CommandResult, interfaces, command routing
#include "serial_core.h"

// Result queue for tag-correlated command/response matching
#include "serial_result_queue.h"

// USB Host manager for PIO-USB CDC devices (client-side)
#include "serial_usb_host.h"

// Binary protocol implementation (client-side, low-level COBS framing)
#include "serial_bus.h"

// Bus client base class (client-side, extends SerialBus with tag queue + INIT)
#include "serial_bus_client.h"

// Bus server base class (server-side, extends ICommandHandler with ACK/NACK)
#include "serial_bus_server.h"

// GunFX binary implementation
#include "serial_gunfx.h"

// LightFX binary implementation
#include "serial_lightfx.h"

// GearControl binary implementation
#include "serial_gearcontrol.h"

#endif // SERIAL_H
