/**
 * @file audio_cli.cpp
 * @brief Audio command handler implementation (refactored to use CommandParser)
 */

#include "audio_cli.h"
#include "command_parser.h"
#include "../audio/audio_codec.h"
#include "../audio/wm8960_codec.h"
#include <Wire.h>

// Helper for parsing loop arguments
static void parseLoopArgs(const String& cmd, AudioPlaybackOptions& opts) {
    int loop_idx = cmd.indexOf("loop");
    if (loop_idx > 0) {
        opts.loop = true;
        String afterLoop = cmd.substring(loop_idx + 4);
        afterLoop.trim();
        
        int spaceAfter = afterLoop.indexOf(' ');
        String loopArg = (spaceAfter > 0) ? afterLoop.substring(0, spaceAfter) : afterLoop;
        loopArg.trim();
        
        if (loopArg == "inf" || loopArg == "infinite" || loopArg.length() == 0) {
            opts.loopCount = LOOP_INFINITE;
        } else if (loopArg.toInt() > 0) {
            opts.loopCount = loopArg.toInt();
        } else {
            opts.loopCount = LOOP_INFINITE;
        }
    }
}

bool AudioCli::handleCommand(const String& cmd) {
    if (!mixer) return false;
    
    CommandParser p(cmd);
    
    // ============== CODEC COMMANDS ==============
    if (p.hasPrefix("codec ")) {
        AudioCodec* activeCodec = this->codec;
        
        if (!activeCodec) {
            Serial.println("Error: No codec configured");
            return true;
        }
        
        if (p.matches("codec", "status"))  { activeCodec->printStatus(); return true; }
        if (p.matches("codec", "test"))    { activeCodec->testCommunication(); return true; }
        if (p.matches("codec", "dump"))    { activeCodec->dumpRegisters(); return true; }
        
        if (p.matches("codec", "reset")) {
            Serial.printf("Resetting %s codec...\n", activeCodec->getModelName());
            activeCodec->reset();
            Serial.println("Codec reset complete");
            return true;
        }
        
        if (p.matches("codec", "reinit") || p.matches("codec", "init")) {
            activeCodec->reinitialize();
            return true;
        }
        
        if (p.matches("codec", "recover")) {
            if (strcmp(activeCodec->getModelName(), "WM8960") == 0) {
                static_cast<WM8960Codec*>(activeCodec)->recoverI2C();
            } else {
                Serial.println("Recovery not supported for this codec");
            }
            return true;
        }
        
        if (p.matches("codec", "read")) {
            int reg = p.argInt(0);
            if (reg >= 0 && reg < 256) {
                uint16_t value = activeCodec->readRegisterCache(reg);
                Serial.printf(value != 0xFFFF 
                    ? "R%d (0x%02X) = 0x%04X (%d)\n" 
                    : "Error: Register read not supported or failed\n", reg, reg, value, value);
            } else {
                Serial.println("Error: Register must be 0-255");
            }
            return true;
        }
        
        if (p.matches("codec", "write")) {
            int reg = p.argInt(0);
            String valueStr = p.arg(1);
            if (valueStr.length() > 0) {
                uint16_t value = (valueStr.startsWith("0x") || valueStr.startsWith("0X"))
                    ? strtol(valueStr.c_str(), NULL, 16) 
                    : valueStr.toInt();
                
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
        
        if (p.matches("codec", "scan")) {
            Serial.println("Scanning I2C bus...");
            TwoWire* wire = static_cast<TwoWire*>(activeCodec->getCommunicationInterface());
            if (wire) {
                int found = 0;
                for (uint8_t addr = 1; addr < 127; addr++) {
                    wire->beginTransmission(addr);
                    if (wire->endTransmission() == 0) {
                        Serial.printf("  Device found at 0x%02X\n", addr);
                        found++;
                    }
                }
                Serial.println(found ? "" : "  No I2C devices found");
                if (found) Serial.printf("Found %d device(s)\n", found);
            } else {
                Serial.println("Error: Communication interface not available");
            }
            return true;
        }
        
        Serial.println("Unknown codec command. Type 'help' for usage.");
        return true;
    }
    
    // ============== MOCK I2S STATS ==============
#if AUDIO_MOCK_I2S
    if (p.matches("audio", "stats") || p.matches("audio", "statistics")) {
        mixer->printMockStatistics();
        return true;
    }
    
    if (p.hasFlag("reset") && p.hasPrefix("audio stats")) {
        mixer->resetMockStatistics();
        return true;
    }
#endif

    // ============== AUDIO PLAY ==============
    if (p.matches("audio", "play")) {
        int ch = p.argInt(0, -1);
        String filename = p.arg(1);
        
        if (ch < 0 || ch >= 8) {
            Serial.println("Error: Invalid channel number (0-7)");
            return true;
        }
        if (filename.length() == 0) {
            Serial.println("Error: Format is 'audio play <ch> <file> [loop [N|inf]] [vol X.X] [left|right]'");
            return true;
        }
        
        AudioPlaybackOptions opts;
        opts.volume = p.valueAfterFloat("vol ", 1.0f);
        opts.output = p.hasFlag("left") ? AudioOutput::Left 
                    : p.hasFlag("right") ? AudioOutput::Right 
                    : AudioOutput::Stereo;
        opts.loopCount = 0;
        opts.loop = false;
        
        parseLoopArgs(cmd, opts);
        
        Serial.printf("Playing %s on channel %d", filename.c_str(), ch);
        if (opts.loop) {
            Serial.printf(opts.loopCount == LOOP_INFINITE ? " (loop infinite)" : " (loop %dx)", opts.loopCount);
        }
        Serial.println();
        
        if (!mixer->playAsync(ch, filename.c_str(), opts)) {
            Serial.println("Error: Failed to queue play command");
        }
        return true;
    }
    
    // ============== AUDIO QUEUE ==============
    if (p.matches("audio", "queue")) {
        // Handle "audio queue clear"
        if (p.arg(0) == "clear") {
            String target = p.arg(1).length() > 0 ? p.arg(1) : "all";
            if (target == "all") {
                Serial.println("Clearing all channel queues");
                mixer->clearQueueAsync(-1);
            } else {
                int ch = target.toInt();
                if (ch >= 0 && ch < 8) {
                    Serial.printf("Clearing queue for channel %d\n", ch);
                    mixer->clearQueueAsync(ch);
                } else {
                    Serial.println("Error: Invalid channel number");
                }
            }
            return true;
        }
        
        // Regular queue command
        int ch = p.argInt(0, -1);
        String filename = p.arg(1);
        
        if (ch < 0 || ch >= 8) {
            Serial.println("Error: Invalid channel number (0-7)");
            return true;
        }
        if (filename.length() == 0) {
            Serial.println("Error: Format is 'audio queue <ch> <file> [loop N] [vol X.X] [--finish-loop|--stop-now]'");
            return true;
        }
        
        AudioPlaybackOptions opts;
        opts.volume = p.valueAfterFloat("vol ", 1.0f);
        opts.output = AudioOutput::Stereo;
        opts.loopCount = 0;
        opts.loop = false;
        
        // Check for loop count (must be finite for queue)
        int loop_idx = cmd.indexOf("loop");
        if (loop_idx > 0) {
            String afterLoop = cmd.substring(loop_idx + 4);
            afterLoop.trim();
            int spaceAfter = afterLoop.indexOf(' ');
            String loopArg = (spaceAfter > 0) ? afterLoop.substring(0, spaceAfter) : afterLoop;
            loopArg.trim();
            
            if (loopArg == "inf" || loopArg == "infinite") {
                Serial.println("Error: Cannot queue infinite loop items");
                return true;
            }
            if (loopArg.toInt() > 0) {
                opts.loop = true;
                opts.loopCount = loopArg.toInt();
            }
        }
        
        QueueLoopBehavior behavior = p.hasFlag("--stop-now", "--stop") 
            ? QueueLoopBehavior::StopImmediate 
            : QueueLoopBehavior::FinishLoop;
        
        Serial.printf("Queueing %s on channel %d", filename.c_str(), ch);
        if (opts.loop && opts.loopCount > 0) Serial.printf(" (loop %dx)", opts.loopCount);
        Serial.printf(" [%s]\n", behavior == QueueLoopBehavior::StopImmediate ? "stop current" : "finish loop");
        
        if (!mixer->queueSoundAsync(ch, filename.c_str(), opts, behavior)) {
            Serial.println("Error: Failed to queue sound");
        }
        return true;
    }
    
    // Handle "audio clear queue" variant
    if (p.matches("audio", "clear") && p.arg(0) == "queue") {
        Serial.println("Clearing all channel queues");
        mixer->clearQueueAsync(-1);
        return true;
    }
    
    // ============== AUDIO STOP ==============
    if (p.matches("audio", "stop")) {
        String target = p.arg(0);
        if (target == "all" || target.length() == 0) {
            Serial.println("Stopping all channels");
            mixer->stopAsync(-1, AudioStopMode::Immediate);
        } else {
            int ch = target.toInt();
            if (ch >= 0 && ch < 8) {
                Serial.printf("Stopping channel %d\n", ch);
                mixer->stopAsync(ch, AudioStopMode::Immediate);
            } else {
                Serial.println("Error: Invalid channel number");
            }
        }
        return true;
    }
    
    // ============== AUDIO FADE ==============
    if (p.matches("audio", "fade")) {
        int ch = p.argInt(0, -1);
        if (ch >= 0 && ch < 8) {
            Serial.printf("Fading channel %d\n", ch);
            mixer->stopAsync(ch, AudioStopMode::Fade);
        } else {
            Serial.println("Error: Invalid channel number");
        }
        return true;
    }
    
    // ============== AUDIO VOLUME ==============
    if (p.matches("audio", "volume")) {
        String arg0 = p.arg(0);
        String arg1 = p.arg(1);
        
        if (arg1.length() > 0) {
            // Channel volume: audio volume <ch> <vol>
            int ch = arg0.toInt();
            float vol = arg1.toFloat();
            if (ch >= 0 && ch < 8) {
                Serial.printf("Setting channel %d volume to %.2f\n", ch, vol);
                mixer->setVolumeAsync(ch, vol);
            } else {
                Serial.println("Error: Invalid channel number");
            }
        } else {
            // Master volume: audio volume <vol>
            float vol = arg0.toFloat();
            Serial.printf("Setting master volume to %.2f\n", vol);
            mixer->setMasterVolumeAsync(vol);
        }
        return true;
    }
    
    // ============== AUDIO MASTER ==============
    if (p.matches("audio", "master")) {
        float vol = p.argFloat(0, 0.5f);
        Serial.printf("Setting master volume to %.2f\n", vol);
        mixer->setMasterVolumeAsync(vol);
        return true;
    }
    
    // ============== AUDIO STATUS ==============
    if (p.matches("audio", "status") || p.is("audio")) {
        if (p.is("audio") && !p.jsonRequested()) {
            return false;  // Just "audio" without status - not handled
        }
        
        if (p.jsonRequested()) {
            Serial.print("{\"channels\":[");
            bool firstChannel = true;
            for (int i = 0; i < 8; i++) {
                if (mixer->isPlaying(i) || mixer->queueLength(i) > 0) {
                    if (!firstChannel) Serial.print(",");
                    firstChannel = false;
                    
                    Serial.printf("{\"channel\":%d,\"status\":\"%s\",\"file\":\"%s\",\"volume\":%.2f,"
                                 "\"remainingMs\":%d,\"looping\":%s,\"loopCount\":%d,\"queueLength\":%d,\"output\":\"%s\"}",
                                 i, 
                                 mixer->isPlaying(i) ? "playing" : "idle",
                                 mixer->getFilename(i) ?: "",
                                 mixer->getChannelVolume(i),
                                 mixer->remainingMs(i), 
                                 mixer->isLooping(i) ? "true" : "false",
                                 mixer->getLoopCount(i),
                                 mixer->queueLength(i),
                                 mixer->getOutput(i) == AudioOutput::Left ? "left" : 
                                 (mixer->getOutput(i) == AudioOutput::Right ? "right" : "stereo"));
                }
            }
            Serial.printf("],\"masterVolume\":%.2f}\n", mixer->masterVolume());
        } else {
            Serial.println("\n=== Audio Status ===");
            bool anyPlaying = false;
            
            for (int i = 0; i < 8; i++) {
                if (mixer->isPlaying(i)) {
                    anyPlaying = true;
                    const char* fname = mixer->getFilename(i);
                    AudioOutput out = mixer->getOutput(i);
                    
                    Serial.printf("Channel %d: PLAYING\n", i);
                    if (fname) {
                        const char* shortName = strrchr(fname, '/');
                        Serial.printf("  File:    %s\n", shortName ? shortName + 1 : fname);
                        Serial.printf("  Path:    %s\n", fname);
                    }
                    
                    if (mixer->isLooping(i)) {
                        int loopCount = mixer->getLoopCount(i);
                        int initialLoop = mixer->getInitialLoopCount(i);
                        if (loopCount == LOOP_INFINITE) {
                            Serial.println("  Mode:    LOOP (infinite)");
                        } else if (initialLoop > 0) {
                            Serial.printf("  Mode:    LOOP (%d/%d remaining)\n", loopCount, initialLoop);
                        } else {
                            Serial.println("  Mode:    LOOP");
                        }
                    } else {
                        Serial.printf("  Mode:    ONCE (%d ms remaining)\n", mixer->remainingMs(i));
                    }
                    
                    int qLen = mixer->queueLength(i);
                    if (qLen > 0) Serial.printf("  Queue:   %d item(s) waiting\n", qLen);
                    
                    Serial.printf("  Volume:  %.0f%%\n", mixer->getChannelVolume(i) * 100.0f);
                    Serial.printf("  Output:  %s\n", 
                                 out == AudioOutput::Left ? "Left" : 
                                 (out == AudioOutput::Right ? "Right" : "Stereo"));
                    Serial.printf("  Format:  %d Hz, %d-bit, %s\n", 
                                 mixer->getSampleRate(i), mixer->getBitsPerSample(i),
                                 mixer->getNumChannels(i) == 1 ? "Mono" : "Stereo");
                    Serial.println();
                }
            }
            
            if (!anyPlaying) Serial.println("No channels playing\n");
            
            bool anyQueued = false;
            for (int i = 0; i < 8; i++) {
                if (!mixer->isPlaying(i) && mixer->queueLength(i) > 0) {
                    if (!anyQueued) { Serial.println("Queued sounds:"); anyQueued = true; }
                    Serial.printf("  Channel %d: %d item(s) waiting\n", i, mixer->queueLength(i));
                }
            }
            if (anyQueued) Serial.println();
            
            Serial.printf("Master Volume: %.0f%%\n\n", mixer->masterVolume() * 100.0f);
        }
        return true;
    }
    
    return false;
}

void AudioCli::printHelp() const {
    Serial.println("=== Audio Commands ===");
    Serial.println("  audio play <ch> <file> [loop [N|inf]] [vol X.X] [left|right]");
    Serial.println("                           - Play audio file on channel");
    Serial.println("                           - loop: repeat infinite times");
    Serial.println("                           - loop N: repeat N times");
    Serial.println("  audio queue <ch> <file> [loop N] [vol X.X] [--finish-loop|--stop-now]");
    Serial.println("                           - Queue sound to play after current");
    Serial.println("                           - Cannot queue infinite loops");
    Serial.println("  audio queue clear <ch|all> - Clear channel queue(s)");
    Serial.println("  audio stop <ch|all>      - Stop channel or all channels");
    Serial.println("  audio fade <ch>          - Fade out channel");
    Serial.println("  audio volume <vol>       - Set master volume (0.0-1.0)");
    Serial.println("  audio volume <ch> <vol>  - Set channel volume (0.0-1.0)");
    Serial.println("  audio master <vol>       - Set master volume (0.0-1.0)");
    Serial.println("  audio status [--json]    - Show all channel status");
#if AUDIO_MOCK_I2S
    Serial.println("");
    Serial.println("=== Mock I2S Statistics (AUDIO_MOCK_I2S=1) ===");
    Serial.println("  audio stats              - Show mock I2S statistics");
    Serial.println("  audio stats reset        - Reset statistics counters");
#endif
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
    Serial.println("  codec recover            - Recover I2C bus (if stuck)");
}
