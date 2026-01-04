/**
 * @file config_cli.cpp
 * @brief Configuration command handler implementation
 */

#include "config_cli.h"

bool ConfigCli::handleCommand(const String& cmd) {
    if (!config) return false;
    
    // Config display command
    if (cmd == "config") {
        Serial.println("\n=== Current Configuration ===");
        config->print();
        Serial.println();
        return true;
    }
    
    // Config subcommands
    if (cmd.startsWith("config ")) {
        String subCmd = cmd.substring(7);
        subCmd.trim();
        
        // Backup config
        if (subCmd == "backup") {
            Serial.println("Backing up current config...");
            if (config->backup()) {
                Serial.println("✓ Config backed up successfully");
            } else {
                Serial.println("✗ Failed to backup config");
            }
            return true;
        }
        
        // Restore config
        if (subCmd == "restore") {
            Serial.println("Restoring config from backup...");
            if (config->restore()) {
                Serial.println("✓ Config restored successfully");
            } else {
                Serial.println("✗ Failed to restore config");
            }
            return true;
        }
        
        // Reload config
        if (subCmd == "reload") {
            Serial.println("Reloading config from flash...");
            if (config->load("/config.yaml")) {
                Serial.println("✓ Config reloaded successfully");
            } else {
                Serial.println("✗ Failed to reload config");
            }
            return true;
        }
        
        // Restart with config reload
        if (subCmd == "restart") {
            Serial.println("Restarting system...");
            Serial.flush();
            delay(100);
            rp2040.reboot();
            return true;
        }
        
        Serial.println("Error: Unknown config subcommand");
        Serial.println("Valid: backup, restore, reload, restart");
        return true;
    }
    
    return false;
}

void ConfigCli::printHelp() const {
    Serial.println("=== Config Commands ===");
    Serial.println("  config                   - Display current config");
    Serial.println("  config backup            - Backup current config");
    Serial.println("  config restore           - Restore from backup");
    Serial.println("  config reload            - Reload config from flash");
    Serial.println("  config restart           - Restart system");
}
