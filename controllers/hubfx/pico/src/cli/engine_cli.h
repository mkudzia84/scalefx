/**
 * @file engine_cli.h
 * @brief Engine effects command handler
 */

#ifndef ENGINE_CLI_H
#define ENGINE_CLI_H

#include "../cli/command_handler.h"
#include "../effects/engine_fx.h"

class EngineCli : public CommandHandler {
private:
    EngineFX* engine;
    
public:
    EngineCli(EngineFX* engine_ptr) : engine(engine_ptr) {}
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "Engine"; }
};

#endif // ENGINE_CLI_H
