#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <threads.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>

// Logging state
static struct {
    FILE *log_file;
    char *log_file_path;
    mtx_t mutex;
    long max_size_bytes;
    int keep_old_logs;
    bool initialized;
} logging_state = {
    .log_file = NULL,
    .log_file_path = NULL,
    .max_size_bytes = 10 * 1024 * 1024,  // 10MB default
    .keep_old_logs = 5,
    .initialized = false
};

// Get file size
static long get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

// Rotate log files: log.txt -> log.1.txt, log.1.txt -> log.2.txt, etc.
static void rotate_log_files(void) {
    if (!logging_state.log_file_path) return;
    
    // Close current log file
    if (logging_state.log_file) {
        fclose(logging_state.log_file);
        logging_state.log_file = NULL;
    }
    
    // Delete oldest log if it exists
    char oldest_log[512];
    snprintf(oldest_log, sizeof(oldest_log), "%s.%d", 
             logging_state.log_file_path, logging_state.keep_old_logs);
    remove(oldest_log);  // Ignore errors if file doesn't exist
    
    // Rotate existing logs: N-1 -> N, N-2 -> N-1, ..., 1 -> 2
    for (int i = logging_state.keep_old_logs - 1; i >= 1; i--) {
        char old_name[512], new_name[512];
        snprintf(old_name, sizeof(old_name), "%s.%d", logging_state.log_file_path, i);
        snprintf(new_name, sizeof(new_name), "%s.%d", logging_state.log_file_path, i + 1);
        rename(old_name, new_name);  // Ignore errors
    }
    
    // Rotate current log to .1
    char rotated_name[512];
    snprintf(rotated_name, sizeof(rotated_name), "%s.1", logging_state.log_file_path);
    rename(logging_state.log_file_path, rotated_name);
    
    // Open new log file
    logging_state.log_file = fopen(logging_state.log_file_path, "a");
    if (!logging_state.log_file) {
        fprintf(stderr, "[LOGGING] Error: Failed to open log file after rotation: %s\n", 
                logging_state.log_file_path);
    }
}

// Check if log rotation is needed
static void check_rotation(void) {
    if (!logging_state.log_file || !logging_state.log_file_path) return;
    
    long current_size = get_file_size(logging_state.log_file_path);
    if (current_size >= logging_state.max_size_bytes) {
        fprintf(stdout, "[LOGGING] Rotating log file (size: %ld bytes, max: %ld bytes)\n",
                current_size, logging_state.max_size_bytes);
        rotate_log_files();
    }
}

int logging_init(const char *log_file, int max_size_mb, int keep_old_logs) {
    if (logging_state.initialized) {
        logging_shutdown();
    }
    
    if (mtx_init(&logging_state.mutex, mtx_plain) != thrd_success) {
        fprintf(stderr, "[LOGGING] Error: Failed to initialize mutex\n");
        return -1;
    }
    
    logging_state.max_size_bytes = (max_size_mb > 0 ? max_size_mb : 10) * 1024 * 1024;
    logging_state.keep_old_logs = (keep_old_logs > 0 ? keep_old_logs : 5);
    logging_state.initialized = true;
    
    if (log_file) {
        logging_state.log_file_path = strdup(log_file);
        if (!logging_state.log_file_path) {
            fprintf(stderr, "[LOGGING] Error: Failed to allocate memory for log path\n");
            mtx_destroy(&logging_state.mutex);
            logging_state.initialized = false;
            return -1;
        }
        
        logging_state.log_file = fopen(log_file, "a");
        if (!logging_state.log_file) {
            fprintf(stderr, "[LOGGING] Error: Failed to open log file: %s (%s)\n", 
                    log_file, strerror(errno));
            free(logging_state.log_file_path);
            logging_state.log_file_path = NULL;
            mtx_destroy(&logging_state.mutex);
            logging_state.initialized = false;
            return -1;
        }
        
        // Make log file line-buffered for real-time updates
        setvbuf(logging_state.log_file, NULL, _IOLBF, 0);
        
        fprintf(stdout, "[LOGGING] Initialized: log_file=%s, max_size=%dMB, keep=%d\n",
                log_file, max_size_mb, keep_old_logs);
    } else {
        fprintf(stdout, "[LOGGING] Initialized: console output only\n");
    }
    
    return 0;
}

void logging_shutdown(void) {
    if (!logging_state.initialized) return;
    
    mtx_lock(&logging_state.mutex);
    
    if (logging_state.log_file) {
        fclose(logging_state.log_file);
        logging_state.log_file = NULL;
    }
    
    if (logging_state.log_file_path) {
        free(logging_state.log_file_path);
        logging_state.log_file_path = NULL;
    }
    
    mtx_unlock(&logging_state.mutex);
    mtx_destroy(&logging_state.mutex);
    
    logging_state.initialized = false;
    
    fprintf(stdout, "[LOGGING] Shutdown complete\n");
}

void logging_write(const char *level, const char *component, const char *fmt, ...) {
    if (!logging_state.initialized) {
        // Fallback to stderr if not initialized (stdout is reserved for status display)
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "%s", component);
        if (strcmp(level, "ERROR") == 0) {
            fprintf(stderr, "Error: ");
        } else if (strcmp(level, "WARN") == 0) {
            fprintf(stderr, "Warning: ");
        }
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
        return;
    }
    
    mtx_lock(&logging_state.mutex);
    
    // Get current timestamp
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Format the log message
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    
    // Write to console only if no file logging (console mode)
    // In interactive mode (file logging enabled): skip console output entirely (logs only to file)
    // In console mode (no file): use stdout for INFO/DEBUG, stderr for WARN/ERROR
    if (!logging_state.log_file) {
        bool use_stderr = (strcmp(level, "ERROR") == 0 || strcmp(level, "WARN") == 0);
        FILE *console = use_stderr ? stderr : stdout;
        fprintf(console, "%s", component);
        if (strcmp(level, "ERROR") == 0) {
            fprintf(console, "Error: ");
        } else if (strcmp(level, "WARN") == 0) {
            fprintf(console, "Warning: ");
        }
        fprintf(console, "%s\n", message);
    }
    
    // Write to log file if enabled
    if (logging_state.log_file) {
        fprintf(logging_state.log_file, "[%s] %s %s%s\n", 
                timestamp, level, component, message);
        fflush(logging_state.log_file);
        
        // Check if rotation is needed
        check_rotation();
    }
    
    mtx_unlock(&logging_state.mutex);
}
