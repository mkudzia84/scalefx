/**
 * @file audio_cli.h
 * @brief Audio command handler
 */

#ifndef AUDIO_CLI_H
#define AUDIO_CLI_H

#include "../cli/command_handler.h"
#include "../audio/audio_mixer.h"

class AudioCodec;  // Forward declaration

class AudioCli : public CommandHandler {
private:
    AudioMixer* mixer;
    AudioCodec* codec;
    
public:
    AudioCli(AudioMixer* mixer_ptr, AudioCodec* codec_ptr = nullptr) 
        : mixer(mixer_ptr), codec(codec_ptr) {}
    
    void setCodec(AudioCodec* codec_ptr) { codec = codec_ptr; }
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "Audio"; }
};

#endif // AUDIO_CLI_H
