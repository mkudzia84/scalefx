/**
 * @file audio_cli.h
 * @brief Audio command handler
 */

#ifndef AUDIO_CLI_H
#define AUDIO_CLI_H

#include "../cli/command_handler.h"
#include "../audio/audio_mixer.h"

class AudioCli : public CommandHandler {
private:
    AudioMixer* mixer;
    
public:
    AudioCli(AudioMixer* mixer_ptr) : mixer(mixer_ptr) {}
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "Audio"; }
};

#endif // AUDIO_CLI_H
