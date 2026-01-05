/**
 * @file system_cli.cpp
 * @brief System command handler implementation
 * 
 * Provides system-level commands with JSON output for ScaleFX App integration.
 */

#include "system_cli.h"
#include "command_parser.h"
#include "../effects/gun_fx.h"
#include "../storage/sd_card.h"
#include <Arduino.h>

bool SystemCli::handleCommand(const String& cmd) {
    CommandParser p(cmd);
    
    // ---- HELP ----
    if (p.is("help") || p.is("?")) {
        if (p.jsonRequested()) {
            // JSON help: list all command groups
            Serial.print("{\"commands\":[");
            bool first = true;
            for (auto* handler : allHandlers) {
                if (!first) Serial.print(",");
                first = false;
                Serial.printf("\"%s\"", handler->getName());
            }
            Serial.println("]}");
        } else {
            Serial.println("\n╔════════════════════════════════════════════════╗");
            Serial.println("║          HubFX Pico - Command Help             ║");
            Serial.println("╚════════════════════════════════════════════════╝\n");
            
            for (auto* handler : allHandlers) {
                handler->printHelp();
                Serial.println();
            }
            
            Serial.println("Tip: Commands are case-insensitive\n");
        }
        return true;
    }
    
    // ---- VERSION [--json] ----
    if (p.is("version") || p.is("ver")) {
        if (p.jsonRequested()) {
            Serial.printf("{\"firmware\":\"v%s\",\"build\":%d,\"platform\":\"RP2040\","
                         "\"board\":\"Raspberry Pi Pico\",\"cpuFrequencyMHz\":%lu,"
                         "\"freeRamBytes\":%lu,\"totalRamBytes\":%d}\n",
                         _firmwareVersion, _buildNumber,
                         F_CPU / 1000000, rp2040.getFreeHeap(), 262144);
        } else {
            Serial.println("\n=== HubFX Pico Version Info ===");
            Serial.printf("Firmware: v%s (Build %d)\n", _firmwareVersion, _buildNumber);
            Serial.println("Platform: RP2040 (Raspberry Pi Pico)");
            Serial.printf("CPU Frequency: %lu MHz\n", F_CPU / 1000000);
            Serial.printf("Free RAM: %lu bytes\n\n", rp2040.getFreeHeap());
        }
        return true;
    }
    
    // ---- STATUS [--json] - System and slave status ----
    if (p.is("status") || p.is("sysinfo")) {
        if (p.jsonRequested()) {
            // Comprehensive JSON status for app
            Serial.print("{\"system\":{");
            Serial.printf("\"firmware\":\"v%s\",\"build\":%d,", _firmwareVersion, _buildNumber);
            Serial.printf("\"platform\":\"RP2040\",\"cpuFrequencyMHz\":%lu,", F_CPU / 1000000);
            Serial.printf("\"freeRamBytes\":%lu,\"totalRamBytes\":%d,", rp2040.getFreeHeap(), 262144);
            Serial.printf("\"uptimeMs\":%lu", millis());
            Serial.print("}");
            
            // SD card status
            Serial.print(",\"sdCard\":{");
            if (_sdCard) {
                Serial.printf("\"initialized\":%s", _sdCard->isInitialized() ? "true" : "false");
            } else {
                Serial.print("\"initialized\":false");
            }
            Serial.print("}");
            
            // Slaves array
            Serial.print(",\"slaves\":[");
            bool firstSlave = true;
            
            // GunFX slave
            if (_gunFx) {
                if (!firstSlave) Serial.print(",");
                firstSlave = false;
                Serial.print("{\"type\":\"GunFX\",");
                Serial.printf("\"connected\":%s,", _gunFx->isConnected() ? "true" : "false");
                Serial.printf("\"ready\":%s", _gunFx->isSlaveReady() ? "true" : "false");
                Serial.print("}");
            }
            
            // Future: LightFX, GearCtrl slaves would go here
            
            Serial.println("]}");
        } else {
            Serial.println("\n=== System Status ===");
            Serial.printf("Firmware: v%s (Build %d)\n", _firmwareVersion, _buildNumber);
            Serial.println("Platform: RP2040 (Raspberry Pi Pico)");
            Serial.printf("CPU: %lu MHz\n", F_CPU / 1000000);
            Serial.printf("Free RAM: %lu bytes\n", rp2040.getFreeHeap());
            Serial.printf("Uptime: %lu ms\n", millis());
            
            Serial.println("\n=== Storage ===");
            if (_sdCard) {
                Serial.printf("SD Card: %s\n", _sdCard->isInitialized() ? "Initialized" : "Not initialized");
            } else {
                Serial.println("SD Card: Not configured");
            }
            
            Serial.println("\n=== Connected Slaves ===");
            bool anySlaves = false;
            
            if (_gunFx) {
                anySlaves = true;
                Serial.printf("GunFX: %s\n", _gunFx->isConnected() 
                    ? (_gunFx->isSlaveReady() ? "Connected (Ready)" : "Connected (Initializing)")
                    : "Not connected");
            }
            
            if (!anySlaves) {
                Serial.println("(No slaves configured)");
            }
            Serial.println();
        }
        return true;
    }
    
    // ---- REBOOT [--json] ----
    if (p.is("reboot") || p.is("restart")) {
        if (p.jsonRequested()) {
            Serial.println("{\"command\":\"reboot\",\"status\":\"rebooting\"}");
        } else {
            Serial.println("Rebooting system...");
        }
        Serial.flush();
        delay(500);
        rp2040.reboot();
        return true;
    }
    
    // ---- BOOTSEL [--json] ----
    if (p.is("bootsel") || p.is("dfu")) {
        if (p.jsonRequested()) {
            Serial.println("{\"command\":\"bootsel\",\"status\":\"entering_dfu\"}");
        } else {
            Serial.println("Entering BOOTSEL mode for firmware upload...");
            Serial.println("Device will appear as USB drive 'RPI-RP2'");
        }
        Serial.flush();
        delay(500);
        rp2040.rebootToBootloader();
        return true;
    }
    
    // ---- PING [--json] - Simple connectivity check for app ----
    if (p.is("ping")) {
        if (p.jsonRequested()) {
            Serial.printf("{\"pong\":true,\"uptimeMs\":%lu}\n", millis());
        } else {
            Serial.println("pong");
        }
        return true;
    }
    
    // ---- CLEAR SCREEN ----
    if (p.is("clear") || p.is("cls")) {
        Serial.write(27);       // ESC
        Serial.print("[2J");    // Clear screen
        Serial.write(27);       // ESC
        Serial.print("[H");     // Cursor to home
        return true;
    }
    
    return false;
}

void SystemCli::printHelp() const {
    Serial.println("=== System Commands ===");
    Serial.println("  help [--json]            - Show this help");
    Serial.println("  version [--json]         - Show version info");
    Serial.println("  status [--json]          - Show system and slave status");
    Serial.println("  ping [--json]            - Connectivity check");
    Serial.println("  reboot [--json]          - Restart system");
    Serial.println("  bootsel [--json]         - Enter BOOTSEL mode for upload");
    Serial.println("  clear                    - Clear screen");
}
