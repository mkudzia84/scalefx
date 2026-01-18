#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <stdbool.h>

/**
 * @file logging.h
 * @brief Unified logging macros for consistent output across all components
 * 
 * This header provides standardized logging macros for error, warning, and
 * info messages with component-based tagging for easy filtering and debugging.
 * Supports file logging with automatic rotation at 10MB and cleanup of old logs.
 */

/**
 * Initialize logging system
 * @param log_file Path to log file (nullptr for stdout/stderr only)
 * @param max_size_mb Maximum log file size in MB before rotation (default: 10)
 * @param keep_old_logs Number of old log files to keep (default: 5)
 * @return 0 on success, -1 on failure
 */
int logging_init(const char *log_file, int max_size_mb, int keep_old_logs);

/**
 * Shutdown logging system and close files
 */
void logging_shutdown(void);

/**
 * Internal function to write log message to file and console
 * Do not call directly - use LOG_* macros instead
 */
void logging_write(const char *level, const char *component, const char *fmt, ...);

/* Color codes for terminal output (optional, can be disabled) */
#define LOG_COLOR_RED     "\033[91m"
#define LOG_COLOR_YELLOW  "\033[93m"
#define LOG_COLOR_GREEN   "\033[92m"
#define LOG_COLOR_BLUE    "\033[94m"
#define LOG_COLOR_RESET   "\033[0m"

/* Component prefixes */
#define LOG_SFXHUB   "[SFXHUB] "
#define LOG_CONFIG   "[CONFIG] "
#define LOG_ENGINE   "[ENGINE] "
#define LOG_GUN      "[GUN]    "
#define LOG_SERVO    "[SERVO]  "
#define LOG_AUDIO    "[AUDIO]  "
#define LOG_SMOKE    "[SMOKE]  "
#define LOG_GPIO     "[GPIO]   "
#define LOG_LIGHTS   "[LIGHTS] "

/* System component tag for logging infrastructure itself */
#define LOG_SYSTEM   "[SYSTEM] "

/* Log level macros - Writes to both file and console */

/**
 * @brief Log an error message (stderr, with component prefix)
 * @param component Component prefix (e.g., LOG_GUN)
 * @param fmt Format string
 * @param ... Arguments
 */
#define LOG_ERROR(component, fmt, ...) \
    logging_write("ERROR", component, fmt, ##__VA_ARGS__)

/**
 * @brief Log a warning message (stderr, with component prefix)
 * @param component Component prefix (e.g., LOG_GUN)
 * @param fmt Format string
 * @param ... Arguments
 */
#define LOG_WARN(component, fmt, ...) \
    logging_write("WARN", component, fmt, ##__VA_ARGS__)

/**
 * @brief Log informational message (stdout, with component prefix)
 * @param component Component prefix (e.g., LOG_GUN)
 * @param fmt Format string
 * @param ... Arguments
 */
#define LOG_INFO(component, fmt, ...) \
    logging_write("INFO", component, fmt, ##__VA_ARGS__)

/**
 * @brief Log debug/verbose message (stdout, with component prefix)
 * Only compiled if DEBUG is defined at compile time
 * @param component Component prefix (e.g., LOG_GUN)
 * @param fmt Format string
 * @param ... Arguments
 */
#ifdef DEBUG
#define LOG_DEBUG(component, fmt, ...) \
    logging_write("DEBUG", component, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(component, fmt, ...) ((void)0)
#endif

/**
 * @brief Log a status line (for periodic dumps)
 * Status lines are untagged for compact display (console only, not logged to file)
 * @param fmt Format string
 * @param ... Arguments
 */
#define LOG_STATUS(fmt, ...) \
    printf(fmt "\n", ##__VA_ARGS__)

/**
 * @brief Log initialization message
 * @param component Component prefix
 * @param details Description of what was initialized
 */
#define LOG_INIT(component, details) \
    LOG_INFO(component, "Initialized: %s", details)

/**
 * @brief Log shutdown message
 * @param component Component prefix
 * @param details Description of what was shut down
 */
#define LOG_SHUTDOWN(component, details) \
    LOG_INFO(component, "Shutdown: %s", details)

/**
 * @brief Log state change message
 * @param component Component prefix
 * @param from_state Previous state
 * @param to_state New state
 */
#define LOG_STATE(component, from_state, to_state) \
    LOG_INFO(component, "State: %s → %s", from_state, to_state)

/**
 * @brief Log a configuration item during startup
 * @param component Component prefix
 * @param item Configuration item name
 * @param value Configuration value
 */
#define LOG_CONFIG_ITEM(component, item, value) \
    printf(component "  %s: %s\n", item, value)

#endif // LOGGING_H
