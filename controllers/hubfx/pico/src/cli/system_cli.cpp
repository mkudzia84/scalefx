/**
 * @file system_cli.cpp
 * @brief System command handler implementation
 */

#include "system_cli.h"
#include <Arduino.h>

bool SystemCli::handleCommand(const String& cmd) {
    // Help command
    if (cmd == "help" || cmd == "?") {
        Serial.println("\n╔════════════════════════════════════════════════╗");
        Serial.println("║          HubFX Pico - Command Help             ║");
        Serial.println("╚════════════════════════════════════════════════╝\n");
        
        // Print help from all registered handlers
        for (auto* handler : allHandlers) {
            handler->printHelp();
            Serial.println();
        }
        
        Serial.println("Tip: Commands are case-insensitive");
        Serial.println();
        return true;
    }
    
    // Version command [--json|-j]
    if (cmd == "version" || cmd == "ver" || cmd.startsWith("version ") || cmd.startsWith("ver ")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        
        if (jsonOutput) {
            Serial.printf("{\"firmware\":\"v0.1.0\",\"platform\":\"RP2040\",\"board\":\"Raspberry Pi Pico\",\"cpuFrequencyMHz\":%lu,\"freeRamBytes\":%lu}\n",
                         F_CPU / 1000000, rp2040.getFreeHeap());
        } else {
            Serial.println("\n=== HubFX Pico Version Info ===");
            Serial.println("Firmware: v0.1.0");
            Serial.println("Platform: RP2040 (Raspberry Pi Pico)");
            Serial.print("CPU Frequency: ");
            Serial.print(F_CPU / 1000000);
            Serial.println(" MHz");
            Serial.print("Free RAM: ");
            Serial.print(rp2040.getFreeHeap());
            Serial.println(" bytes");
            Serial.println();
        }
        return true;
    }
    
    // System status command [--json|-j]
    if (cmd == "status" || cmd.startsWith("status ") || cmd.startsWith("sysinfo")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        
        if (jsonOutput) {
            Serial.printf("{\"system\":{\"firmware\":\"v0.1.0\",\"platform\":\"RP2040\",\"cpuFrequencyMHz\":%lu,\"freeRamBytes\":%lu},\"slaves\":[]}\n",
                         F_CPU / 1000000, rp2040.getFreeHeap());
        } else {
            Serial.println("\n=== System Status ===");
            Serial.println("Firmware: v0.1.0");
            Serial.println("Platform: RP2040 (Raspberry Pi Pico)");
            Serial.print("CPU: ");
            Serial.print(F_CPU / 1000000);
            Serial.println(" MHz");
            Serial.print("Free RAM: ");
            Serial.print(rp2040.getFreeHeap());
            Serial.println(" bytes");
            Serial.println();
            Serial.println("=== Connected Slaves ===");
            Serial.println("(No slaves configured)");
            Serial.println();
        }
        return true;
    }
    
    // Reboot command
    if (cmd == "reboot" || cmd == "restart") {
        Serial.println("Rebooting system...");
        Serial.flush();
        delay(500);
        rp2040.reboot();
        return true;
    }
    
    // Enter BOOTSEL mode for firmware upload
    if (cmd == "bootsel" || cmd == "dfu") {
        Serial.println("Entering BOOTSEL mode for firmware upload...");
        Serial.println("Device will appear as USB drive 'RPI-RP2'");
        Serial.flush();
        delay(500);
        rp2040.rebootToBootloader();
        return true;
    }
    
    // Clear screen
    if (cmd == "clear" || cmd == "cls") {
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
    Serial.println("  help                     - Show this help");
    Serial.println("  version [--json]         - Show version info");
    Serial.println("  status [--json]          - Show system and slave status");
    Serial.println("  reboot                   - Restart system");
    Serial.println("  bootsel                  - Enter BOOTSEL mode for upload");
    Serial.println("  clear                    - Clear screen");
}
