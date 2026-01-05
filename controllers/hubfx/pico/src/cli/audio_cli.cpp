/**
 * @file audio_cli.cpp
 * @brief Audio command handler implementation
 */

#include "audio_cli.h"
#include "../audio/audio_codec.h"
#include <Wire.h>

bool AudioCli::handleCommand(const String& cmd) {
    if (!mixer) return false;
    
    // Codec debug commands - work with any codec type
    if (cmd.startsWith("codec ")) {
        AudioCodec* activeCodec = this->codec;
        
        if (!activeCodec) {
            Serial.println("Error: No codec configured");
            return true;
        }
        
        // codec status
        if (cmd == "codec status") {
            activeCodec->printStatus();
            return true;
        }
        
        // codec test
        if (cmd == "codec test") {
            activeCodec->testCommunication();
            return true;
        }
        
        // codec dump
        if (cmd == "codec dump") {
            activeCodec->dumpRegisters();
            return true;
        }
        
        // codec reset
        if (cmd == "codec reset") {
            Serial.printf("Resetting %s codec...\n", activeCodec->getModelName());
            activeCodec->reset();
            Serial.println("Codec reset complete");
            return true;
        }
        
        // codec reinit
        if (cmd == "codec reinit" || cmd == "codec init") {
            activeCodec->reinitialize();
            return true;
        }
        
        // codec read <reg>
        if (cmd.startsWith("codec read ")) {
            int reg = cmd.substring(11).toInt();
            if (reg >= 0 && reg < 256) {
                uint16_t value = activeCodec->readRegisterCache(reg);
                if (value != 0xFFFF) {
                    Serial.printf("R%d (0x%02X) = 0x%04X (%d)\n", reg, reg, value, value);
                } else {
                    Serial.println("Error: Register read not supported or failed");
                }
            } else {
                Serial.println("Error: Register must be 0-255");
            }
            return true;
        }
        
        // codec write <reg> <value>
        if (cmd.startsWith("codec write ")) {
            int firstSpace = cmd.indexOf(' ', 12);
            if (firstSpace > 0) {
                int reg = cmd.substring(12, firstSpace).toInt();
                String valueStr = cmd.substring(firstSpace + 1);
                valueStr.trim();
                
                // Support hex (0x) or decimal
                uint16_t value;
                if (valueStr.startsWith("0x") || valueStr.startsWith("0X")) {
                    value = strtol(valueStr.c_str(), NULL, 16);
                } else {
                    value = valueStr.toInt();
                }
                
                if (reg >= 0 && reg < 256 && value <= 0xFFFF) {
                    activeCodec->writeRegisterDebug(reg, value);
                } else {
                    Serial.println("Error: Register must be 0-255, value must be 0-0xFFFF");
                }
            } else {
                Serial.println("Usage: codec write <reg> <value>");
            }
            return true;
        }
        
        // codec scan - scan I2C bus
        if (cmd == "codec scan") {
            Serial.println("Scanning I2C bus...");
            TwoWire* wire = static_cast<TwoWire*>(activeCodec->getCommunicationInterface());
            if (wire) {
                int found = 0;
                for (uint8_t addr = 1; addr < 127; addr++) {
                    wire->beginTransmission(addr);
                    uint8_t error = wire->endTransmission();
                    if (error == 0) {
                        Serial.printf("  Device found at 0x%02X\n", addr);
                        found++;
                    }
                }
                if (found == 0) {
                    Serial.println("  No I2C devices found");
                } else {
                    Serial.printf("\nFound %d device(s)\n", found);
                }
            } else {
                Serial.println("Error: Communication interface not available");
            }
            return true;
        }
        
        Serial.println("Unknown codec command. Type 'help' for usage.");
        return true;
    }
    
    // Play command - format: play channel filename [loop] [vol X.X] [left|right]
    if (cmd.startsWith("play ")) {
        int firstSpace = cmd.indexOf(' ');
        int secondSpace = cmd.indexOf(' ', firstSpace + 1);
        
        if (secondSpace > 0) {
            int ch = cmd.substring(firstSpace + 1, secondSpace).toInt();
            String remaining = cmd.substring(secondSpace + 1);
            
            // Extract filename (first part before any options)
            String filename;
            int nextSpace = remaining.indexOf(' ');
            if (nextSpace > 0) {
                filename = remaining.substring(0, nextSpace);
            } else {
                filename = remaining;
            }
            filename.trim();
            
            // Check for valid channel
            if (ch < 0 || ch >= 8) {  // AUDIO_MAX_CHANNELS = 8
                Serial.println("Error: Invalid channel number (0-7)");
                return true;
            }
            
            // Parse options
            AudioPlaybackOptions opts;
            opts.loop = cmd.indexOf("loop") > 0;
            opts.volume = 1.0f;
            opts.output = AudioOutput::Stereo;
            
            // Check for output routing
            if (cmd.indexOf("left") > 0) opts.output = AudioOutput::Left;
            else if (cmd.indexOf("right") > 0) opts.output = AudioOutput::Right;
            
            // Check for volume
            int vol_idx = cmd.indexOf("vol ");
            if (vol_idx > 0) {
                opts.volume = cmd.substring(vol_idx + 4).toFloat();
            }
            
            Serial.print("Playing ");
            Serial.print(filename);
            Serial.print(" on channel ");
            Serial.print(ch);
            if (opts.loop) Serial.print(" (looped)");
            Serial.println();
            
            // Use async API for dual-core safety
            if (!mixer->playAsync(ch, filename.c_str(), opts)) {
                Serial.println("Error: Failed to queue play command");
            }
            return true;
        } else {
            Serial.println("Error: Format is 'play channel filename [loop] [vol X.X] [left|right]'");
            return true;
        }
    }
    
    // Stop command - format: stop channel|all
    if (cmd.startsWith("stop ")) {
        String arg = cmd.substring(5);
        arg.trim();
        
        if (arg == "all") {
            Serial.println("Stopping all channels");
            mixer->stopAsync(-1, AudioStopMode::Immediate);
            return true;
        }
        
        int ch = arg.toInt();
        if (ch >= 0 && ch < 8) {
            Serial.print("Stopping channel ");
            Serial.println(ch);
            mixer->stopAsync(ch, AudioStopMode::Immediate);
            return true;
        } else {
            Serial.println("Error: Invalid channel number");
            return true;
        }
    }
    
    // Fade command - format: fade channel
    if (cmd.startsWith("fade ")) {
        int ch = cmd.substring(5).toInt();
        if (ch >= 0 && ch < 8) {
            Serial.print("Fading channel ");
            Serial.println(ch);
            mixer->stopAsync(ch, AudioStopMode::Fade);
            return true;
        } else {
            Serial.println("Error: Invalid channel number");
            return true;
        }
    }
    
    // Volume command - format: volume channel level  OR  volume level (for master)
    if (cmd.startsWith("volume ")) {
        String args = cmd.substring(7);
        args.trim();
        
        int space = args.indexOf(' ');
        if (space > 0) {
            // Channel volume: volume ch vol
            int ch = args.substring(0, space).toInt();
            float vol = args.substring(space + 1).toFloat();
            
            if (ch >= 0 && ch < 8) {
                Serial.print("Setting channel ");
                Serial.print(ch);
                Serial.print(" volume to ");
                Serial.println(vol);
                mixer->setVolumeAsync(ch, vol);
                return true;
            } else {
                Serial.println("Error: Invalid channel number");
                return true;
            }
        } else {
            // Master volume: volume vol
            float vol = args.toFloat();
            Serial.print("Setting master volume to ");
            Serial.println(vol);
            mixer->setMasterVolumeAsync(vol);
            return true;
        }
    }
    
    // Master volume command - format: master volume
    if (cmd.startsWith("master ")) {
        float vol = cmd.substring(7).toFloat();
        Serial.print("Setting master volume to ");
        Serial.println(vol);
        mixer->setMasterVolumeAsync(vol);
        return true;
    }
    
    // Status command [--json|-j]
    if (cmd == "status" || cmd.startsWith("status ")) {
        bool jsonOutput = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        
        if (jsonOutput) {
            Serial.print("{\"channels\":[");
            bool firstChannel = true;
            for (int i = 0; i < 8; i++) {  // AUDIO_MAX_CHANNELS
                if (mixer->isPlaying(i)) {
                    if (!firstChannel) Serial.print(",");
                    firstChannel = false;
                    
                    int remaining = mixer->remainingMs(i);
                    Serial.printf("{\"channel\":%d,\"status\":\"playing\",\"remainingMs\":%d,\"looping\":%s}",
                                 i, remaining, (remaining < 0) ? "true" : "false");
                }
            }
            Serial.printf("],\"masterVolume\":%.2f}\n", mixer->masterVolume());
        } else {
            Serial.println("\n=== Audio Status ===");
            for (int i = 0; i < 8; i++) {  // AUDIO_MAX_CHANNELS
                if (mixer->isPlaying(i)) {
                    int remaining = mixer->remainingMs(i);
                    Serial.print("Channel ");
                    Serial.print(i);
                    Serial.print(": PLAYING");
                    
                    if (remaining >= 0) {
                        Serial.print(" (");
                        Serial.print(remaining);
                        Serial.print(" ms remaining)");
                    } else {
                        Serial.print(" (looping)");
                    }
                    Serial.println();
                }
            }
            
            Serial.print("\nMaster Volume: ");
            Serial.println(mixer->masterVolume());
            Serial.println();
        }
        return true;
    }
    
    return false;
}

void AudioCli::printHelp() const {
    Serial.println("=== Audio Commands ===");
    Serial.println("  play <ch> <file> [loop] [vol X.X] [left|right]");
    Serial.println("                           - Play audio file on channel");
    Serial.println("  stop <ch|all>            - Stop channel or all channels");
    Serial.println("  fade <ch>                - Fade out channel");
    Serial.println("  volume <vol>             - Set master volume (0.0-1.0)");
    Serial.println("  volume <ch> <vol>        - Set channel volume (0.0-1.0)");
    Serial.println("  master <vol>             - Set master volume (0.0-1.0)");
    Serial.println("  status [--json]          - Show all channel status");
    Serial.println("");
    Serial.println("=== Codec Debug Commands ===");
    Serial.println("  codec status             - Show codec initialization status");
    Serial.println("  codec test               - Test I2C communication");
    Serial.println("  codec scan               - Scan I2C bus for devices");
    Serial.println("  codec dump               - Dump all codec registers");
    Serial.println("  codec read <reg>         - Read register (0-55)");
    Serial.println("  codec write <reg> <val>  - Write register (hex: 0xXXXX)");
    Serial.println("  codec reset              - Software reset codec");
    Serial.println("  codec reinit             - Full codec reinitialization");
}
