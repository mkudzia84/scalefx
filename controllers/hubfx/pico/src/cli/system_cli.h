/**
 * @file system_cli.h
 * @brief System-level commands (help, version, etc.)
 */

#ifndef SYSTEM_CLI_H
#define SYSTEM_CLI_H

#include "../cli/command_handler.h"
#include <vector>

class SystemCli : public CommandHandler {
private:
    std::vector<CommandHandler*> allHandlers;
    
public:
    SystemCli() {}
    
    void registerHandlers(const std::vector<CommandHandler*>& handlers) {
        allHandlers = handlers;
    }
    
    bool handleCommand(const String& cmd) override;
    void printHelp() const override;
    const char* getName() const override { return "System"; }
};

#endif // SYSTEM_CLI_H
