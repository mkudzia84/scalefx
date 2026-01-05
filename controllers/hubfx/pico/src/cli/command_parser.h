/**
 * @file command_parser.h
 * @brief Utility class for parsing CLI commands
 * 
 * Reduces boilerplate in command handlers by providing:
 * - Command matching with prefix detection
 * - Automatic flag extraction (--json, -j, etc.)
 * - Argument parsing with type conversion
 * - Subcommand extraction
 */

#ifndef COMMAND_PARSER_H
#define COMMAND_PARSER_H

#include <Arduino.h>
#include <vector>

/**
 * Parsed command result with easy access to parts
 */
class CommandParser {
public:
    CommandParser(const String& cmd) : _original(cmd) {
        parse();
    }
    
    // ---- Command Matching ----
    
    /** Check if command matches exactly */
    bool is(const char* command) const {
        return _base.equalsIgnoreCase(command);
    }
    
    /** Check if command starts with prefix (e.g., "audio") */
    bool hasPrefix(const char* prefix) const {
        return _original.startsWith(prefix);
    }
    
    /** Check if command is "prefix subcommand" (e.g., "audio play") */
    bool matches(const char* prefix, const char* subcommand = nullptr) const {
        if (!_original.startsWith(prefix)) return false;
        if (!subcommand) return true;
        
        String expected = String(prefix) + " " + subcommand;
        return _original.startsWith(expected) || _original.equalsIgnoreCase(expected);
    }
    
    // ---- Flag Detection ----
    
    /** Check if --json or -j flag is present */
    bool jsonRequested() const { return _jsonFlag; }
    
    /** Check for any flag (e.g., "--verbose", "-v") */
    bool hasFlag(const char* longForm, const char* shortForm = nullptr) const {
        if (_original.indexOf(longForm) >= 0) return true;
        if (shortForm && _original.indexOf(shortForm) >= 0) return true;
        return false;
    }
    
    // ---- Argument Access ----
    
    /** Get number of arguments (excluding flags) */
    size_t argCount() const { return _args.size(); }
    
    /** Get argument by index (0-based, after command/subcommand) */
    String arg(size_t index) const {
        return (index < _args.size()) ? _args[index] : "";
    }
    
    /** Get argument as integer */
    int argInt(size_t index, int defaultVal = 0) const {
        if (index >= _args.size()) return defaultVal;
        return _args[index].toInt();
    }
    
    /** Get argument as float */
    float argFloat(size_t index, float defaultVal = 0.0f) const {
        if (index >= _args.size()) return defaultVal;
        return _args[index].toFloat();
    }
    
    /** Get all remaining text after command (without flags) */
    String remainder() const { return _remainder; }
    
    /** Get the base command (first word) */
    String base() const { return _base; }
    
    /** Get subcommand (second word, if present) */
    String subcommand() const { return _subcommand; }
    
    /** Get original command string */
    String original() const { return _original; }
    
    // ---- Named Arguments ----
    
    /** Get value after a keyword (e.g., "vol 0.5" -> 0.5) */
    String valueAfter(const char* keyword) const {
        int idx = _original.indexOf(keyword);
        if (idx < 0) return "";
        
        String after = _original.substring(idx + strlen(keyword));
        after.trim();
        
        // Get the next word
        int space = after.indexOf(' ');
        return (space > 0) ? after.substring(0, space) : after;
    }
    
    float valueAfterFloat(const char* keyword, float defaultVal = 0.0f) const {
        String val = valueAfter(keyword);
        return val.length() > 0 ? val.toFloat() : defaultVal;
    }
    
    int valueAfterInt(const char* keyword, int defaultVal = 0) const {
        String val = valueAfter(keyword);
        return val.length() > 0 ? val.toInt() : defaultVal;
    }

private:
    void parse() {
        String cmd = _original;
        cmd.trim();
        
        // Extract common flags
        _jsonFlag = (cmd.indexOf("--json") >= 0 || cmd.indexOf("-j") >= 0);
        
        // Remove known flags for cleaner parsing
        String clean = cmd;
        clean.replace("--json", "");
        clean.replace("-j", "");
        clean.trim();
        
        // Split into parts
        std::vector<String> parts;
        int start = 0;
        for (int i = 0; i <= (int)clean.length(); i++) {
            if (i == (int)clean.length() || clean[i] == ' ') {
                if (i > start) {
                    String part = clean.substring(start, i);
                    part.trim();
                    if (part.length() > 0) {
                        parts.push_back(part);
                    }
                }
                start = i + 1;
            }
        }
        
        // Extract base command and subcommand
        if (parts.size() > 0) {
            _base = parts[0];
        }
        if (parts.size() > 1) {
            _subcommand = parts[1];
        }
        
        // Arguments are everything after base and subcommand (excluding flags)
        for (size_t i = 2; i < parts.size(); i++) {
            if (!parts[i].startsWith("-")) {
                _args.push_back(parts[i]);
            }
        }
        
        // Remainder is everything after base command
        int firstSpace = clean.indexOf(' ');
        if (firstSpace > 0) {
            _remainder = clean.substring(firstSpace + 1);
            _remainder.trim();
        }
    }
    
    String _original;
    String _base;
    String _subcommand;
    String _remainder;
    std::vector<String> _args;
    bool _jsonFlag = false;
};

// ============================================================================
//  CONVENIENCE MACROS
// ============================================================================

// Quick command check macros for common patterns
#define CMD_IS(cmd, name) (cmd).is(name)
#define CMD_MATCHES(cmd, prefix, sub) (cmd).matches(prefix, sub)
#define CMD_JSON(cmd) (cmd).jsonRequested()

#endif // COMMAND_PARSER_H
