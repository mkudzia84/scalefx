/**
 * @file config_cli.cpp
 * @brief Configuration command handler implementation (refactored to use CommandParser)
 */

#include "config_cli.h"
#include "command_parser.h"
#include "../storage/sd_card.h"

bool ConfigCli::handleCommand(const String& cmd) {
    if (!config) return false;
    
    CommandParser p(cmd);
    
    // config - Display current configuration
    if (p.is("config")) {
        Serial.println("\n=== Current Configuration ===");
        config->print();
        Serial.println();
        return true;
    }
    
    // config <subcommand> - Configuration operations
    if (p.hasPrefix("config ")) {
        // config backup
        if (p.matches("config", "backup")) {
            Serial.println("Backing up current config...");
            Serial.println(config->backup() ? "✓ Config backed up successfully" : "✗ Failed to backup config");
            return true;
        }
        
        // config restore
        if (p.matches("config", "restore")) {
            Serial.println("Restoring config from backup...");
            Serial.println(config->restore() ? "✓ Config restored successfully" : "✗ Failed to restore config");
            return true;
        }
        
        // config reload
        if (p.matches("config", "reload")) {
            Serial.println("Reloading config from SD card...");
            Serial.println(config->load("/config.yaml") ? "✓ Config reloaded successfully" : "✗ Failed to reload config");
            return true;
        }
        
        // config restart
        if (p.matches("config", "restart")) {
            Serial.println("Restarting system...");
            Serial.flush();
            delay(100);
            rp2040.reboot();
            return true;
        }
        
        // config upload <size>
        if (p.matches("config", "upload")) {
            if (!sdCard) {
                Serial.println("ERROR: SD card module not available");
                return true;
            }
            
            if (!sdCard->isInitialized()) {
                Serial.println("ERROR: SD card not initialized. Run 'sd init' first.");
                return true;
            }
            
            uint32_t totalSize = p.argInt(0, 0);
            if (totalSize == 0 || totalSize > 102400) {  // Max 100KB for config
                Serial.println("ERROR: Size must be 1 to 102400 bytes");
                return true;
            }
            
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
