/**
 * @file config_cli.cpp
 * @brief Configuration command handler implementation
 */

#include "config_cli.h"

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
        
        // Upload config to flash: config upload <size>
        if (subCmd.startsWith("upload ")) {
            String sizeStr = subCmd.substring(7);
            sizeStr.trim();
            
            uint32_t totalSize = sizeStr.toInt();
            if (totalSize == 0 || totalSize > 102400) {  // Max 100KB for config
                Serial.println("Error: Size must be 1 to 102400 bytes");
                return true;
            }
            
            // Ensure flash is initialized
            if (!config->isFlashStorage()) {
                Serial.println("ERROR: Config not initialized with flash storage");
                Serial.println("This should not happen. Try rebooting.");
                return true;
            }
            
            Serial.println("READY");  // Signal ready to receive
            Serial.flush();
            
            // Open file for writing directly (stream, don't buffer entire file)
            File file = LittleFS.open("/config.yaml", "w");
            if (!file) {
                Serial.println("ERROR: Cannot open flash file for writing");
                return true;
            }
            
            uint32_t bytesReceived = 0;
            uint8_t buffer[256];  // Smaller buffer for streaming
            uint32_t lastReport = 0;
            
            while (bytesReceived < totalSize) {
                // Kick watchdog
                rp2040.wdt_reset();
                
                // Wait for data with timeout
                uint32_t startWait = millis();
                while (!Serial.available() && (millis() - startWait) < 5000) {
                    rp2040.wdt_reset();
                    delay(10);
                }
                
                if (!Serial.available()) {
                    Serial.println("ERROR: Timeout waiting for data");
                    file.close();
                    return true;
                }
                
                // Read available data
                size_t toRead = min((size_t)(totalSize - bytesReceived), sizeof(buffer));
                size_t available = Serial.available();
                toRead = min(toRead, available);
                
                size_t bytesRead = Serial.readBytes(buffer, toRead);
                
                // Write directly to flash
                size_t written = file.write(buffer, bytesRead);
                if (written != bytesRead) {
                    Serial.println("ERROR: Flash write failed");
                    file.close();
                    return true;
                }
                
                bytesReceived += bytesRead;
                
                // Progress report every 1KB or at completion
                if (bytesReceived - lastReport >= 1024 || bytesReceived >= totalSize) {
                    Serial.print("PROGRESS: ");
                    Serial.print(bytesReceived);
                    Serial.print("/");
                    Serial.println(totalSize);
                    Serial.flush();
                    lastReport = bytesReceived;
                }
            }
            
            // Close and sync file
            file.flush();
            file.close();
            
            Serial.println("SUCCESS: Config uploaded to flash");
            Serial.println("Run 'config reload' to apply changes");
            Serial.flush();
            
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
    Serial.println("  config reload            - Reload config from flash");
    Serial.println("  config backup            - Backup current config to flash");
    Serial.println("  config restore           - Restore config from backup");
    Serial.println("  config upload <size>     - Upload config to flash via serial");
    Serial.println("  config restart           - Restart system");
}
