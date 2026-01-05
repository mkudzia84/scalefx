/**
 * @file config_cli.cpp
 * @brief Configuration command handler implementation (refactored to use CommandParser)
 */

#include "config_cli.h"
#include "command_parser.h"
#include "../storage/sd_card.h"

// Helper to escape JSON strings
static void printJsonString(const char* str) {
    if (!str) {
        Serial.print("null");
        return;
    }
    Serial.print("\"");
    while (*str) {
        switch (*str) {
            case '"':  Serial.print("\\\""); break;
            case '\\': Serial.print("\\\\"); break;
            case '\n': Serial.print("\\n"); break;
            case '\r': Serial.print("\\r"); break;
            case '\t': Serial.print("\\t"); break;
            default:   Serial.print(*str); break;
        }
        str++;
    }
    Serial.print("\"");
}

void ConfigCli::printConfigJson() const {
    const HubFXSettings& s = config->settings();
    
    Serial.print("{\"engine\":{");
    Serial.printf("\"enabled\":%s,", s.engine.enabled ? "true" : "false");
    Serial.printf("\"togglePin\":%d,", s.engine.togglePin);
    Serial.printf("\"toggleThresholdUs\":%d,", s.engine.toggleThresholdUs);
    Serial.printf("\"channelA\":%d,", s.engine.channelA);
    Serial.printf("\"channelB\":%d,", s.engine.channelB);
    Serial.printf("\"crossfadeMs\":%d,", s.engine.crossfadeMs);
    
    // Sounds
    Serial.print("\"startupSound\":{");
    Serial.print("\"filename\":"); printJsonString(s.engine.startupSound.filename);
    Serial.printf(",\"volume\":%.2f},", s.engine.startupSound.volume);
    
    Serial.print("\"runningSound\":{");
    Serial.print("\"filename\":"); printJsonString(s.engine.runningSound.filename);
    Serial.printf(",\"volume\":%.2f},", s.engine.runningSound.volume);
    
    Serial.print("\"shutdownSound\":{");
    Serial.print("\"filename\":"); printJsonString(s.engine.shutdownSound.filename);
    Serial.printf(",\"volume\":%.2f},", s.engine.shutdownSound.volume);
    
    Serial.printf("\"startingOffsetFromStoppingMs\":%d,", s.engine.startingOffsetFromStoppingMs);
    Serial.printf("\"stoppingOffsetFromStartingMs\":%d", s.engine.stoppingOffsetFromStartingMs);
    Serial.print("},");
    
    // Gun FX
    Serial.print("\"gun\":{");
    Serial.printf("\"enabled\":%s,", s.gun.enabled ? "true" : "false");
    Serial.printf("\"triggerChannel\":%d,", s.gun.triggerChannel);
    Serial.printf("\"audioChannel\":%d,", s.gun.audioChannel);
    
    // Smoke
    Serial.print("\"smoke\":{");
    Serial.printf("\"heaterToggleChannel\":%d,", s.gun.smoke.heaterToggleChannel);
    Serial.printf("\"heaterThresholdUs\":%d,", s.gun.smoke.heaterThresholdUs);
    Serial.printf("\"fanOffDelayMs\":%d},", s.gun.smoke.fanOffDelayMs);
    
    // Rates of fire
    Serial.printf("\"rateCount\":%d,", s.gun.rateCount);
    Serial.print("\"ratesOfFire\":[");
    for (int i = 0; i < s.gun.rateCount; i++) {
        if (i > 0) Serial.print(",");
        const RateOfFireConfig& r = s.gun.ratesOfFire[i];
        Serial.print("{");
        Serial.printf("\"pwmThresholdUs\":%d,", r.pwmThresholdUs);
        Serial.printf("\"rpm\":%d,", r.rpm);
        Serial.print("\"soundFile\":"); printJsonString(r.soundFile);
        Serial.printf(",\"soundVolume\":%.2f}", r.soundVolume);
    }
    Serial.print("],");
    
    // Pitch servo
    Serial.print("\"pitch\":{");
    Serial.printf("\"inputChannel\":%d,", s.gun.pitch.inputChannel);
    Serial.printf("\"servoId\":%d,", s.gun.pitch.servoId);
    Serial.printf("\"inputMinUs\":%d,", s.gun.pitch.inputMinUs);
    Serial.printf("\"inputMaxUs\":%d,", s.gun.pitch.inputMaxUs);
    Serial.printf("\"outputMinUs\":%d,", s.gun.pitch.outputMinUs);
    Serial.printf("\"outputMaxUs\":%d,", s.gun.pitch.outputMaxUs);
    Serial.printf("\"recoilJerkUs\":%d,", s.gun.pitch.recoilJerkUs);
    Serial.printf("\"recoilJerkVarianceUs\":%d},", s.gun.pitch.recoilJerkVarianceUs);
    
    // Yaw servo
    Serial.print("\"yaw\":{");
    Serial.printf("\"inputChannel\":%d,", s.gun.yaw.inputChannel);
    Serial.printf("\"servoId\":%d,", s.gun.yaw.servoId);
    Serial.printf("\"inputMinUs\":%d,", s.gun.yaw.inputMinUs);
    Serial.printf("\"inputMaxUs\":%d,", s.gun.yaw.inputMaxUs);
    Serial.printf("\"outputMinUs\":%d,", s.gun.yaw.outputMinUs);
    Serial.printf("\"outputMaxUs\":%d,", s.gun.yaw.outputMaxUs);
    Serial.printf("\"recoilJerkUs\":%d,", s.gun.yaw.recoilJerkUs);
    Serial.printf("\"recoilJerkVarianceUs\":%d}", s.gun.yaw.recoilJerkVarianceUs);
    
    Serial.print("},");
    Serial.printf("\"loaded\":%s}\n", s.loaded ? "true" : "false");
}

bool ConfigCli::handleCommand(const String& cmd) {
    if (!config) return false;
    
    CommandParser p(cmd);
    bool json = p.jsonRequested();
    
    // Check subcommands FIRST before bare "config" command
    // Note: "config --json" also has prefix "config " so check for flags-only explicitly
    if (p.hasPrefix("config ") && !p.matches("config", "--json") && !p.matches("config", "-j")) {
        // config backup
        if (p.matches("config", "backup")) {
            bool success = config->backup();
            if (json) {
                Serial.printf("{\"command\":\"backup\",\"success\":%s}\n", success ? "true" : "false");
            } else {
                Serial.println("Backing up current config...");
                Serial.println(success ? "✓ Config backed up successfully" : "✗ Failed to backup config");
            }
            return true;
        }
        
        // config restore
        if (p.matches("config", "restore")) {
            bool success = config->restore();
            if (json) {
                Serial.printf("{\"command\":\"restore\",\"success\":%s}\n", success ? "true" : "false");
            } else {
                Serial.println("Restoring config from backup...");
                Serial.println(success ? "✓ Config restored successfully" : "✗ Failed to restore config");
            }
            return true;
        }
        
        // config reload
        if (p.matches("config", "reload")) {
            bool success = config->load("/config.yaml");
            if (json) {
                Serial.printf("{\"command\":\"reload\",\"success\":%s}\n", success ? "true" : "false");
            } else {
                Serial.println("Reloading config from SD card...");
                Serial.println(success ? "✓ Config reloaded successfully" : "✗ Failed to reload config");
            }
            return true;
        }
        
        // config size
        if (p.matches("config", "size")) {
            int size = config->getSize("/config.yaml");
            if (json) {
                Serial.printf("{\"command\":\"size\",\"bytes\":%d}\n", size);
            } else {
                if (size >= 0) {
                    Serial.printf("Config file size: %d bytes\n", size);
                } else {
                    Serial.println("Error: Cannot read config file size");
                }
            }
            return true;
        }
        
        // config upload <size>
        if (p.matches("config", "upload")) {
            if (!sdCard().isInitialized()) {
                if (json) {
                    Serial.println("{\"command\":\"upload\",\"success\":false,\"error\":\"SD card not initialized\"}");
                } else {
                    Serial.println("ERROR: SD card not initialized. Run 'sd init' first.");
                }
                return true;
            }
            
            uint32_t totalSize = p.argInt(0, 0);
            if (totalSize == 0 || totalSize > 102400) {  // Max 100KB for config
                if (json) {
                    Serial.println("{\"command\":\"upload\",\"success\":false,\"error\":\"Size must be 1 to 102400 bytes\"}");
                } else {
                    Serial.println("ERROR: Size must be 1 to 102400 bytes");
                }
                return true;
            }
            
            bool success = sdCard().uploadFile("/config.yaml", totalSize, Serial);
            if (!json && success) {
                Serial.println("Run 'config reload' to apply changes");
            }
            return true;
        }
        
        if (json) {
            Serial.println("{\"error\":\"Unknown config subcommand\"}");
        } else {
            Serial.println("Error: Unknown config subcommand");
            Serial.println("Valid: backup, restore, reload, size, upload");
        }
        return true;
    }
    
    // config - Display current configuration (no subcommand)
    if (p.is("config")) {
        if (json) {
            printConfigJson();
        } else {
            Serial.println("\n=== Current Configuration ===");
            config->print();
            Serial.println();
        }
        return true;
    }
    
    return false;
}

void ConfigCli::printHelp() const {
    Serial.println("=== Config Commands ===");
    Serial.println("  config [--json]          - Display current configuration");
    Serial.println("  config reload [--json]   - Reload config from SD card");
    Serial.println("  config backup [--json]   - Backup current config to SD card");
    Serial.println("  config restore [--json]  - Restore config from backup");
    Serial.println("  config size [--json]     - Show config file size");
    Serial.println("  config upload <size>     - Upload config to SD card via serial");
}
