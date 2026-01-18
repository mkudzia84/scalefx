/*
 * Serial Library - Unified Serial Communication for ScaleFX
 *
 * Master include file for the complete ScaleFX serial communication library.
 * Include this file for full functionality, or include specific headers for
 * minimal dependencies.
 *
 * Architecture:
 *   serial_error.h           - Error codes and CommandResult struct
 *   serial_core.h            - Protocol encoding, ISerialCore interface, CoreCommandHandler
 *   serial_bus.h             - UsbHost and SerialBus (master-side, USB Host)
 *   serial_command_handler.h - Chain of Responsibility command routing (slave-side)
 *
 * Protocol Implementations:
 *   serial_gunfx.h           - GunFX master/slave (muzzle flash, servo, smoke)
 *   serial_lightfx.h         - LightFX master/slave (LED, servo, power)
 *
 * Master vs Slave:
 *   Master (HubFX): Uses UsbHost + SerialBus for USB Host CDC communication
 *   Slave (Pico):   Uses Serial (USB Device) + CoreCommandHandler + CommandRouter
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

// Binary protocol implementation
#include "serial_bus.h"

// GunFX binary implementation
#include "serial_gunfx.h"

// LightFX binary implementation
#include "serial_lightfx.h"

#endif // SERIAL_H
