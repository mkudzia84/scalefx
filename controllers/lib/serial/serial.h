/*
 * Serial Library - Unified Serial Communication for ScaleFX
 *
 * Master include file for the complete ScaleFX serial communication library.
 * Include this file for full functionality, or include specific headers for
 * minimal dependencies.
 *
 * Architecture:
 *   serial_error.h           - Error codes and CommandResult struct
 *   serial_core.h            - Protocol encoding, ISerialCore interface, CoreCommandServer
 *   serial_usb_host.h        - UsbHost (client-side, USB Host PIO-USB manager)
 *   serial_bus.h             - SerialBus (client-side, COBS-framed protocol)
 *   serial_command_handler.h - Chain of Responsibility command routing (server-side)
 *
 * Protocol Implementations:
 *   serial_gunfx.h           - GunFX client/server (muzzle flash, servo, smoke)
 *   serial_lightfx.h         - LightFX client/server (LED, servo, power)
 *
 * Client vs Server:
 *   Client (HubFX): Uses UsbHost + SerialBus for USB Host CDC communication
 *   Server (Pico):  Uses Serial (USB Device) + CoreCommandServer + CommandRouter
 *
 * Usage:
 *   #include <serial.h>              // Everything
 *   #include <serial_gunfx.h>        // GunFX only
 *   #include <serial_lightfx.h>      // LightFX only
 *   #include <serial_core.h>         // Just protocol utilities
 */

#ifndef SERIAL_H
#define SERIAL_H

// Generic error codes and CommandResult (ACK/NACK support)
#include "serial_error.h"

// Core protocol definitions, interface and command handling
#include "serial_core.h"

// Command handler interface and router (Chain of Responsibility pattern)
#include "serial_command_handler.h"

// USB Host manager for PIO-USB CDC devices (client-side)
#include "serial_usb_host.h"

// Binary protocol implementation (client-side)
#include "serial_bus.h"

// GunFX binary implementation
#include "serial_gunfx.h"

// LightFX binary implementation
#include "serial_lightfx.h"

#endif // SERIAL_H
