/**
 * @file command_router.h
 * @brief Routes commands to appropriate handlers
 */

#ifndef COMMAND_ROUTER_H
#define COMMAND_ROUTER_H

#include "command_handler.h"
#include <vector>

class CommandRouter {
private:
    std::vector<CommandHandler*> handlers;
    
public:
    CommandRouter() {}
    
    /**
     * @brief Register a command handler
     */
    void addHandler(CommandHandler* handler) {
        if (handler) {
            handlers.push_back(handler);
        }
    }
    
    /**
     * @brief Route command to appropriate handler
     * @return true if command was handled, false if unknown
     */
    bool routeCommand(const String& cmd) {
        // Try each handler in order
        for (auto* handler : handlers) {
            if (handler->handleCommand(cmd)) {
                return true;
            }
        }
        return false;
    }
    
    /**
     * @brief Print help from all handlers
     */
    void printHelp() {
        for (auto* handler : handlers) {
            handler->printHelp();
            Serial.println();
        }
    }
};

#endif // COMMAND_ROUTER_H
