/**
 * @file command_handler.h
 * @brief Base class for command handlers
 * 
 * Provides a generic interface for handling serial commands.
 * Each subsystem implements its own command handler.
 */

#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include <Arduino.h>

/**
 * Abstract base class for command handlers
 */
class CommandHandler {
public:
    virtual ~CommandHandler() = default;
    
    /**
     * Handle a command
     * @param cmd The command string
     * @return true if command was handled, false otherwise
     */
    virtual bool handleCommand(const String& cmd) = 0;
    
    /**
     * Print help text for this handler's commands
     */
    virtual void printHelp() const = 0;
    
    /**
     * Get handler name for debugging
     */
    virtual const char* getName() const = 0;
};

#endif // COMMAND_HANDLER_H
