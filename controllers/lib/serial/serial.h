/*
 * Serial Library - Unified Serial Communication for ScaleFX
 * 
 * Combined library providing:
 *   - Protocol utilities (COBS framing, CRC-8, packet types) in SerialProtocol namespace
 *   - Generic error codes (SerialError) and CommandResult for ACK/NACK handling
 *   - SerialInitHandler for protocol negotiation (always text-based)
 *   - UsbHost class for USB HOST functionality (PIO-USB for CDC devices)
 *   - SerialBusBase abstract interface
 *   - SerialBus class for binary COBS packet protocol (default)
 *   - SerialBusText class for human-readable text protocol (testing)
 *   - IGunFxMaster/IGunFxSlave abstract interfaces for protocol-agnostic code
 *   - GunFxSerialMaster/Slave classes for GunFX binary communication
 *   - GunFxSerialMasterText/SlaveText for GunFX text communication
 * 
 * Usage:
 *   #include <serial.h>       // Include everything
 *   
 * Or include specific components:
 *   #include <serial_protocol.h>     // Protocol constants only
 *   #include <serial_error.h>        // Generic error codes and CommandResult
 *   #include <serial_init.h>         // Protocol negotiation
 *   #include <serial_bus.h>          // Binary serial bus
 *   #include <serial_bus_text.h>     // Text serial bus
 *   #include <serial_gunfx_types.h>  // GunFX types and interfaces
 *   #include <serial_gunfx.h>        // GunFX binary master/slave
 *   #include <serial_gunfx_text.h>   // GunFX text master/slave
 */

#ifndef SERIAL_H
#define SERIAL_H

// Core protocol definitions
#include "serial_protocol.h"

// Generic error codes and CommandResult (ACK/NACK support)
#include "serial_error.h"

// Command handler interface and router (Chain of Responsibility pattern)
#include "serial_command_handler.h"

// Protocol negotiation (INIT/INIT_READY handshake)
#include "serial_init.h"

// Abstract base class
#include "serial_bus_base.h"

// Binary protocol implementation (default)
#include "serial_bus.h"

// Text protocol implementation (for testing)
#include "serial_bus_text.h"

// GunFX types and interfaces (protocol-agnostic)
#include "serial_gunfx_types.h"

// GunFX binary and text implementations
#include "serial_gunfx.h"
#include "serial_gunfx_text.h"

#endif // SERIAL_H
