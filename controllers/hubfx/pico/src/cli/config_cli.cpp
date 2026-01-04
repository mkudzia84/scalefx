/**
 * @file config_cli.cpp
 * @brief Configuration command handler implementation
 */

#include "config_cli.h"
#include "../storage/sd_card.h"

bool ConfigCli::handleCommand(const String& cmd) {
    if (!config) return false;
    
    // config - Display current configuration
    if (cmd == "config") {
        Serial.println("\n=== Current Configuration ===");
        config->print();
        Serial.println();
        return true;
    }
    
    // config <subcommand> - Configuration operations
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
            Serial.println("Reloading config from SD card...");
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
        
        // Upload config to SD card: config upload <size>
        if (subCmd.startsWith("upload ")) {
            if (!sdCard) {
                Serial.println("ERROR: SD card module not available");
                return true;
            }
            
            if (!sdCard->isInitialized()) {
                Serial.println("ERROR: SD card not initialized. Run 'sd init' first.");
                return true;
            }
            
            String sizeStr = subCmd.substring(7);
            sizeStr.trim();
            
            uint32_t totalSize = sizeStr.toInt();
            if (totalSize == 0 || totalSize > 102400) {  // Max 100KB for config
                Serial.println("ERROR: Size must be 1 to 102400 bytes");
                return true;
            }
            
            // Use unified upload method
            if (sdCard->uploadFile("/config.yaml", totalSize, Serial)) {
                Serial.println("Run 'config reload' to apply changes");
            }
            
            return true;
        }
        
        Serial.println("Error: Unknown config subcommand");
        Serial.println("Valid: backup, restore, reload, restart, upload");
        return true;
    }
    
    return false;
}

void ConfigCli::printHelp() const {
    Serial.println("=== Config Commands ===");
    Serial.println("  config                   - Display current configuration");
    Serial.println("  config reload            - Reload config from SD card");
    Serial.println("  config backup            - Backup current config to SD card");
    Serial.println("  config restore           - Restore config from backup");
    Serial.println("  config upload <size>     - Upload config to SD card via serial");
    Serial.println("  config restart           - Restart system");
}
