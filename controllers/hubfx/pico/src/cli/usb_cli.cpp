/**
 * @file usb_cli.cpp
 * @brief USB Host CLI command implementation
 * 
 * Uses the UsbHost class from serial_common library
 */

#include "usb_cli.h"
#include "command_parser.h"
#include "../tusb_config.h"
#include <Arduino.h>

bool UsbCli::handleCommand(const String& cmd) {
    CommandParser p(cmd);
    
    // Only handle 'usb' commands
    if (!p.is("usb") && !p.hasPrefix("usb ")) {
        return false;
    }
    
    String sub = p.subcommand();
    
    // ---- USB LIST - List connected devices ----
    if (sub == "" || sub == "list" || sub == "ls" || sub == "devices") {
        if (!_usb || !_usb->isInitialized()) {
            if (p.jsonRequested()) {
                Serial.println("{\"error\":\"USB host not initialized\"}");
            } else {
                Serial.println("USB host not initialized");
            }
            return true;
        }
        
        // Use printStatus from serial_common UsbHost
        if (p.jsonRequested()) {
            int count = _usb->cdcDeviceCount();
            Serial.print("{\"cdcDeviceCount\":");
            Serial.print(count);
            Serial.print(",\"ready\":");
            Serial.print(_usb->isReady() ? "true" : "false");
            Serial.println("}");
        } else {
            _usb->printStatus();
        }
        return true;
    }
    
    // ---- USB STATUS - Show USB host status ----
    if (sub == "status") {
        if (p.jsonRequested()) {
            Serial.print("{\"initialized\":");
            Serial.print(_usb && _usb->isInitialized() ? "true" : "false");
            Serial.print(",\"taskRunning\":");
            Serial.print(_usb && _usb->isTaskRunning() ? "true" : "false");
            Serial.print(",\"ready\":");
            Serial.print(_usb && _usb->isReady() ? "true" : "false");
            Serial.print(",\"cdcDeviceCount\":");
            Serial.print(_usb ? _usb->cdcDeviceCount() : 0);
            Serial.print(",\"dpPin\":");
            Serial.print(PIO_USB_DP_PIN_DEFAULT);
            Serial.print(",\"dmPin\":");
            Serial.print(PIO_USB_DP_PIN_DEFAULT + 1);
            Serial.println("}");
        } else {
            Serial.println("\n=== USB Host Status ===");
            Serial.printf("  Initialized: %s\n", _usb && _usb->isInitialized() ? "Yes" : "No");
            Serial.printf("  Task Running: %s\n", _usb && _usb->isTaskRunning() ? "Yes" : "No");
            Serial.printf("  Ready: %s\n", _usb && _usb->isReady() ? "Yes" : "No");
            Serial.printf("  CDC Devices: %d\n", _usb ? _usb->cdcDeviceCount() : 0);
            Serial.printf("  D+ Pin: GP%d\n", PIO_USB_DP_PIN_DEFAULT);
            Serial.printf("  D- Pin: GP%d\n", PIO_USB_DP_PIN_DEFAULT + 1);
            Serial.printf("  PIO: %d\n", PIO_USB_TX_DEFAULT);
            Serial.println();
        }
        return true;
    }
    
    // ---- USB INFO <index> - Detailed CDC device info ----
    if (sub == "info") {
        if (!_usb || !_usb->isInitialized()) {
            if (p.jsonRequested()) {
                Serial.println("{\"error\":\"USB host not initialized\"}");
            } else {
                Serial.println("USB host not initialized");
            }
            return true;
        }
        
        String idxStr = p.arg(0);
        if (idxStr == "") {
            if (p.jsonRequested()) {
                Serial.println("{\"error\":\"Usage: usb info <index>\"}");
            } else {
                Serial.println("Usage: usb info <index>");
                Serial.printf("  Index range: 0-%d\n", _usb->cdcDeviceCount() - 1);
            }
            return true;
        }
        
        int index = idxStr.toInt();
        const CdcDeviceInfo* info = _usb->getCdcDevice(index);
        
        if (!info || !info->connected) {
            if (p.jsonRequested()) {
                Serial.printf("{\"error\":\"No CDC device at index %d\"}\n", index);
            } else {
                Serial.printf("No CDC device at index %d\n", index);
            }
            return true;
        }
        
        if (p.jsonRequested()) {
            Serial.printf("{\"index\":%d,\"devAddr\":%d,\"itfNum\":%d,"
                         "\"vid\":\"0x%04X\",\"pid\":\"0x%04X\",\"connected\":%s}\n",
                index, info->dev_addr, info->itf_num,
                info->vid, info->pid,
                info->connected ? "true" : "false");
        } else {
            Serial.println("\n=== USB CDC Device Info ===");
            Serial.printf("  Index: %d\n", index);
            Serial.printf("  Device Address: %d\n", info->dev_addr);
            Serial.printf("  Interface: %d\n", info->itf_num);
            Serial.printf("  VID: 0x%04X\n", info->vid);
            Serial.printf("  PID: 0x%04X\n", info->pid);
            Serial.printf("  Connected: %s\n", info->connected ? "Yes" : "No");
            Serial.println();
        }
        return true;
    }
    
    // ---- USB STATS - Show statistics ----
    if (sub == "stats") {
        if (!_usb) {
            Serial.println("USB host not available");
            return true;
        }
        
        const UsbHostStats& stats = _usb->stats();
        
        if (p.jsonRequested()) {
            Serial.printf("{\"devicesMounted\":%lu,\"devicesUnmounted\":%lu,"
                         "\"bytesSent\":%lu,\"bytesReceived\":%lu}\n",
                stats.devices_mounted, stats.devices_unmounted,
                stats.bytes_sent, stats.bytes_received);
        } else {
            Serial.println("\n=== USB Host Statistics ===");
            Serial.printf("  Devices mounted: %lu\n", stats.devices_mounted);
            Serial.printf("  Devices unmounted: %lu\n", stats.devices_unmounted);
            Serial.printf("  Bytes sent: %lu\n", stats.bytes_sent);
            Serial.printf("  Bytes received: %lu\n", stats.bytes_received);
            Serial.println();
        }
        return true;
    }
    
    // Unknown subcommand
    if (p.jsonRequested()) {
        Serial.printf("{\"error\":\"Unknown usb command: %s\"}\n", sub.c_str());
    } else {
        Serial.printf("Unknown usb command: %s\n", sub.c_str());
        Serial.println("Use 'help' for available commands");
    }
    return true;
}

void UsbCli::printHelp() const {
    Serial.println("=== USB Host Commands ===");
    Serial.println("  usb [--json]            - List connected USB CDC devices");
    Serial.println("  usb status [--json]     - Show USB host status");
    Serial.println("  usb info <idx> [--json] - Show CDC device info by index");
    Serial.println("  usb stats [--json]      - Show USB host statistics");
}
