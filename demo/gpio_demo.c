#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include "gpio.h"

static bool running = true;

void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\nReceived interrupt signal, stopping...\n");
        running = false;
    }
}

void pwm_callback(PWMReading reading, void *user_data) {
    int *count = (int *)user_data;
    (*count)++;
    printf("[ASYNC] Pin %d: %d us - Reading #%d\n", 
           reading.pin, reading.duration_us, *count);
}

void print_usage(const char *program_name) {
    printf("Usage: %s --pin <N> [OPTIONS]\n", program_name);
    printf("\nOptions:\n");
    printf("  --pin <N>        GPIO pin number to monitor (required, 0-27)\n");
    printf("  --mode <MODE>    Monitoring mode: 'sync' or 'async' (default: async)\n");
    printf("  --timeout <MS>   Timeout in milliseconds for sync mode (default: 100)\n");
    printf("  --count <N>      Number of readings to capture (default: infinite)\n");
    printf("  --help, -h       Show this help message\n");
    printf("\nExamples:\n");
    printf("  Async monitoring (with callback):\n");
    printf("    %s --pin 17 --mode async\n", program_name);
    printf("\n  Sync monitoring (blocking reads):\n");
    printf("    %s --pin 17 --mode sync --count 10\n", program_name);
    printf("\n  Async with limited readings:\n");
    printf("    %s --pin 17 --mode async --count 100\n", program_name);
    printf("\nPress Ctrl+C to stop monitoring\n");
}

int main(int argc, char *argv[]) {
    int pin = -1;
    char *mode = "async";
    int timeout_ms = 100;
    int max_count = -1; // Infinite by default
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --pin requires a pin number\n");
                return 1;
            }
            pin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mode") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --mode requires 'sync' or 'async'\n");
                return 1;
            }
            mode = argv[++i];
            if (strcmp(mode, "sync") != 0 && strcmp(mode, "async") != 0) {
                fprintf(stderr, "Error: mode must be 'sync' or 'async'\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--timeout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --timeout requires a value in milliseconds\n");
                return 1;
            }
            timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--count") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --count requires a number\n");
                return 1;
            }
            max_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Validate pin
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "Error: Invalid or missing pin number (must be 0-27)\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    
    // Initialize GPIO
    if (gpio_init() < 0) {
        fprintf(stderr, "Error: Failed to initialize GPIO\n");
        fprintf(stderr, "Note: Try running with sudo for GPIO access\n");
        return 1;
    }
    
    printf("GPIO PWM Monitor Demo\n");
    printf("=====================\n");
    printf("Pin: %d\n", pin);
    printf("Mode: %s\n", mode);
    if (strcmp(mode, "sync") == 0) {
        printf("Timeout: %d ms\n", timeout_ms);
    }
    if (max_count > 0) {
        printf("Max readings: %d\n", max_count);
    } else {
        printf("Max readings: infinite\n");
    }
    printf("\nStarting monitoring... (Press Ctrl+C to stop)\n\n");
    
    int reading_count = 0;
    
    if (strcmp(mode, "async") == 0) {
        // Async mode with callback
        PWMMonitor *monitor = pwm_monitor_create(pin, pwm_callback, &reading_count);
        if (!monitor) {
            fprintf(stderr, "Error: Failed to create PWM monitor\n");
            gpio_cleanup();
            return 1;
        }
        
        if (pwm_monitor_start(monitor) < 0) {
            fprintf(stderr, "Error: Failed to start PWM monitor\n");
            pwm_monitor_destroy(monitor);
            gpio_cleanup();
            return 1;
        }
        
        // Wait until stopped or max count reached
        while (running) {
            if (max_count > 0 && reading_count >= max_count) {
                printf("\nReached maximum reading count (%d)\n", max_count);
                break;
            }
            usleep(100000); // 100ms
        }
        
        // Show statistics
        printf("\nReadings processed: %d\n", reading_count);
        
        pwm_monitor_stop(monitor);
        pwm_monitor_destroy(monitor);
        
    } else {
        // Sync mode with blocking reads
        PWMMonitor *monitor = pwm_monitor_create(pin, NULL, NULL);
        if (!monitor) {
            fprintf(stderr, "Error: Failed to create PWM monitor\n");
            gpio_cleanup();
            return 1;
        }
        
        if (pwm_monitor_start(monitor) < 0) {
            fprintf(stderr, "Error: Failed to start PWM monitor\n");
            pwm_monitor_destroy(monitor);
            gpio_cleanup();
            return 1;
        }
        
        PWMReading reading;
        while (running) {
            if (max_count > 0 && reading_count >= max_count) {
                printf("\nReached maximum reading count (%d)\n", max_count);
                break;
            }
            
            // Wait for next reading
            if (pwm_monitor_wait_reading(monitor, &reading, timeout_ms)) {
                reading_count++;
                printf("[SYNC] Pin %d: %d us - Reading #%d\n",
                       reading.pin, reading.duration_us, reading_count);
            } else {
                if (!running) break;
                printf("[SYNC] No reading (timeout after %d ms)\n", timeout_ms);
            }
        }
        
        // Show statistics
        printf("\nReadings processed: %d\n", reading_count);
        
        pwm_monitor_stop(monitor);
        pwm_monitor_destroy(monitor);
    }
    
    // Cleanup
    gpio_cleanup();
    
    printf("\nGPIO monitor stopped.\n");
    return 0;
}
