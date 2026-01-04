/**
 * @file audio_cli.cpp
 * @brief Audio command handler implementation
 */

#include "audio_cli.h"

bool AudioCli::handleCommand(const String& cmd) {
    if (!mixer) return false;
    
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
}
