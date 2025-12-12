#include "status.h"
#include "gun_fx.h"
#include "engine_fx.h"
#include "servo.h"
#include "gpio.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

// ANSI color codes
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_BLUE    "\033[34m"

struct StatusDisplay {
    GunFX *gun;
    EngineFX *engine;
    int interval_ms;
    
    // Threading
    thrd_t display_thread;
    atomic_bool running;
    
    struct timespec start_time;
};

// Helper to format PWM value
static const char* format_pwm(int pwm_us, char *buffer, size_t buf_size) {
    if (pwm_us < 0) {
        snprintf(buffer, buf_size, "n/a");
    } else {
        snprintf(buffer, buf_size, "%4d", pwm_us);
    }
    return buffer;
}

// Helper to get color for PWM value
static const char* pwm_color(int pwm_us) {
    return pwm_us >= 0 ? COLOR_YELLOW : COLOR_RED;
}

// Helper to get color for boolean state
static const char* bool_color(bool state) {
    return state ? COLOR_GREEN : COLOR_RED;
}

// Print engine status
static void print_engine_status(EngineFX *engine) {
    if (!engine) return;
    
    EngineState state = engine_fx_get_state(engine);
    const char *state_str = "UNKNOWN";
    const char *state_color = COLOR_RED;
    
    switch (state) {
        case ENGINE_STOPPED:
            state_str = "STOPPED";
            state_color = COLOR_RED;
            break;
        case ENGINE_STARTING:
            state_str = "STARTING";
            state_color = COLOR_YELLOW;
            break;
        case ENGINE_RUNNING:
            state_str = "RUNNING";
            state_color = COLOR_GREEN;
            break;
        case ENGINE_STOPPING:
            state_str = "STOPPING";
            state_color = COLOR_YELLOW;
            break;
    }
    
    printf(COLOR_CYAN COLOR_BOLD "🚁 ENGINE STATUS\n" COLOR_RESET);
    printf(COLOR_CYAN "═══════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf(COLOR_BOLD "State:" COLOR_RESET " %s%-10s" COLOR_RESET "\n", state_color, state_str);
}

// Print gun status
static void print_gun_status(GunFX *gun) {
    if (!gun) return;
    
    bool is_firing = gun_fx_is_firing(gun);
    int rate_index = gun_fx_get_current_rate_index(gun);
    int rpm = gun_fx_get_current_rpm(gun);
    
    printf(COLOR_CYAN "═══════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf(COLOR_CYAN COLOR_BOLD "🎯 GUN STATUS\n" COLOR_RESET);
    printf(COLOR_CYAN "═══════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf(COLOR_BOLD "Firing:" COLOR_RESET " %s%-4s" COLOR_RESET "  │  ", 
           bool_color(is_firing), is_firing ? "YES" : "NO");
    printf(COLOR_BOLD "Rate:" COLOR_RESET " %d  │  ", rate_index >= 0 ? rate_index + 1 : 0);
    printf(COLOR_BOLD "RPM:" COLOR_RESET " %-4d\n", rpm);
}

// Print PWM inputs
static void print_pwm_inputs(GunFX *gun, EngineFX *engine) {
    char pwm_buf[16];
    
    printf(COLOR_CYAN "═══════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf(COLOR_MAGENTA COLOR_BOLD "📡 PWM INPUTS\n" COLOR_RESET);
    
    // Engine toggle
    if (engine) {
        int engine_pwm = engine_fx_get_toggle_pwm(engine);
        int engine_pin = engine_fx_get_toggle_pin(engine);
        printf("  • Engine Toggle:     GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs\n",
               engine_pin,
               pwm_color(engine_pwm),
               format_pwm(engine_pwm, pwm_buf, sizeof(pwm_buf)));
    }
    
    // Gun trigger
    if (gun) {
        int trigger_pwm = gun_fx_get_trigger_pwm(gun);
        int trigger_pin = gun_fx_get_trigger_pin(gun);
        printf("  • Gun Trigger:       GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs\n",
               trigger_pin,
               pwm_color(trigger_pwm),
               format_pwm(trigger_pwm, pwm_buf, sizeof(pwm_buf)));
    }
    
    // Smoke heater toggle
    if (gun) {
        int heater_pwm = gun_fx_get_heater_toggle_pwm(gun);
        int heater_pin = gun_fx_get_heater_toggle_pin(gun);
        bool heater_on = gun_fx_get_heater_state(gun);
        printf("  • Smoke Heater Tog:  GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs  [%s%-4s" COLOR_RESET "]\n",
               heater_pin,
               pwm_color(heater_pwm),
               format_pwm(heater_pwm, pwm_buf, sizeof(pwm_buf)),
               bool_color(heater_on),
               heater_on ? "ON" : "OFF");
    }
    
    // Servo inputs
    if (gun) {
        Servo *pitch_servo = gun_fx_get_pitch_servo(gun);
        Servo *yaw_servo = gun_fx_get_yaw_servo(gun);
        
        if (pitch_servo) {
            int pitch_pwm = gun_fx_get_pitch_pwm(gun);
            int pitch_pin = gun_fx_get_pitch_pin(gun);
            printf("  • Pitch Servo Input: GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs\n",
                   pitch_pin,
                   pwm_color(pitch_pwm),
                   format_pwm(pitch_pwm, pwm_buf, sizeof(pwm_buf)));
        }
        
        if (yaw_servo) {
            int yaw_pwm = gun_fx_get_yaw_pwm(gun);
            int yaw_pin = gun_fx_get_yaw_pin(gun);
            printf("  • Yaw Servo Input:   GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs\n",
                   yaw_pin,
                   pwm_color(yaw_pwm),
                   format_pwm(yaw_pwm, pwm_buf, sizeof(pwm_buf)));
        }
    }
}

// Print servo outputs
static void print_servo_outputs(GunFX *gun) {
    if (!gun) return;
    
    Servo *pitch_servo = gun_fx_get_pitch_servo(gun);
    Servo *yaw_servo = gun_fx_get_yaw_servo(gun);
    
    if (!pitch_servo && !yaw_servo) return;
    
    printf(COLOR_CYAN "═══════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf(COLOR_MAGENTA COLOR_BOLD "🎮 SERVO OUTPUTS\n" COLOR_RESET);
    
    if (pitch_servo) {
        int pitch_output = servo_get_output(pitch_servo);
        int pitch_output_pin = gun_fx_get_pitch_output_pin(gun);
        printf("  • Pitch Servo:       GPIO %2d  " COLOR_BOLD "←" COLOR_RESET "  %s%-6d" COLOR_RESET " µs\n",
               pitch_output_pin,
               COLOR_GREEN,
               pitch_output);
    }
    
    if (yaw_servo) {
        int yaw_output = servo_get_output(yaw_servo);
        int yaw_output_pin = gun_fx_get_yaw_output_pin(gun);
        printf("  • Yaw Servo:         GPIO %2d  " COLOR_BOLD "←" COLOR_RESET "  %s%-6d" COLOR_RESET " µs\n",
               yaw_output_pin,
               COLOR_GREEN,
               yaw_output);
    }
}

// Print output features status
static void print_output_features(GunFX *gun, EngineFX *engine) {
    printf(COLOR_CYAN "═══════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf(COLOR_MAGENTA COLOR_BOLD "🔧 OUTPUT FEATURES\n" COLOR_RESET);
    
    // Engine audio status
    if (engine) {
        EngineState state = engine_fx_get_state(engine);
        const char *audio_status = "None";
        switch (state) {
            case ENGINE_STARTING: audio_status = "Starting Sound"; break;
            case ENGINE_RUNNING: audio_status = "Running Sound (Loop)"; break;
            case ENGINE_STOPPING: audio_status = "Stopping Sound"; break;
            default: audio_status = "None"; break;
        }
        printf("  • Engine Audio:      %s%-20s" COLOR_RESET "\n", 
               state != ENGINE_STOPPED ? COLOR_GREEN : COLOR_RED, audio_status);
    }
    
    // Gun status
    if (gun) {
        bool is_firing = gun_fx_is_firing(gun);
        int rate_index = gun_fx_get_current_rate_index(gun);
        int rpm = gun_fx_get_current_rpm(gun);
        
        // Gun audio
        const char *gun_audio = is_firing ? "Firing Sound (Loop)" : "None";
        printf("  • Gun Audio:         %s%-20s" COLOR_RESET, 
               is_firing ? COLOR_GREEN : COLOR_RED, gun_audio);
        if (is_firing) {
            printf(" [Rate %d @ %d RPM]", rate_index + 1, rpm);
        }
        printf("\n");
        
        // Nozzle flash
        int nozzle_pin = gun_fx_get_nozzle_flash_pin(gun);
        printf("  • Nozzle Flash:      GPIO %2d  %s%-10s" COLOR_RESET "\n",
               nozzle_pin,
               is_firing ? COLOR_GREEN : COLOR_RED,
               is_firing ? "BLINKING" : "OFF");
        
        // Smoke heater
        bool heater_on = gun_fx_get_heater_state(gun);
        int smoke_heater_pin = gun_fx_get_smoke_heater_pin(gun);
        printf("  • Smoke Heater:      GPIO %2d  %s%-10s" COLOR_RESET "\n",
               smoke_heater_pin,
               bool_color(heater_on),
               heater_on ? "ON" : "OFF");
        
        // Smoke fan with detailed status
        int smoke_fan_pin = gun_fx_get_smoke_fan_pin(gun);
        bool fan_pending_off = gun_fx_get_smoke_fan_pending_off(gun);
        const char *fan_status;
        const char *fan_color;
        
        if (is_firing) {
            fan_status = "ON";
            fan_color = COLOR_GREEN;
        } else if (fan_pending_off) {
            fan_status = "TURNING OFF";
            fan_color = COLOR_YELLOW;
        } else {
            fan_status = "OFF";
            fan_color = COLOR_RED;
        }
        
        printf("  • Smoke Fan:         GPIO %2d  %s%-12s" COLOR_RESET "\n",
               smoke_fan_pin, fan_color, fan_status);
    }
}

// Main display function
static void display_status(StatusDisplay *status) {
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    double elapsed = (current_time.tv_sec - status->start_time.tv_sec) + 
                    (current_time.tv_nsec - status->start_time.tv_nsec) / 1e9;
    
    // Clear screen and print header
    printf("\033[2J\033[H");  // Clear screen and move cursor to top
    printf(COLOR_CYAN COLOR_BOLD "════════════════════════════════════════════════════════════════════════════\n");
    printf("                         HELIFX SYSTEM STATUS @ %.1fs\n", elapsed);
    printf("════════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf("\n");
    
    // Print engine status
    if (status->engine) {
        print_engine_status(status->engine);
    }
    
    // Print gun status
    if (status->gun) {
        print_gun_status(status->gun);
    }
    
    // Print PWM inputs
    print_pwm_inputs(status->gun, status->engine);
    
    // Print servo outputs
    if (status->gun) {
        print_servo_outputs(status->gun);
    }
    
    // Print output features status
    print_output_features(status->gun, status->engine);
    
    printf(COLOR_CYAN "═══════════════════════════════════════════════════════════════════════════\n" COLOR_RESET);
    printf("\n");
}

// Display thread
static int status_display_thread(void *arg) {
    StatusDisplay *status = (StatusDisplay *)arg;
    
    LOG_INFO(LOG_SYSTEM, "Status display thread started (interval: %dms)", status->interval_ms);
    
    while (atomic_load(&status->running)) {
        display_status(status);
        
        // Sleep for interval in microseconds
        usleep(status->interval_ms * 1000);
    }
    
    LOG_INFO(LOG_SYSTEM, "Status display thread stopped");
    return thrd_success;
}

StatusDisplay* status_display_create(GunFX *gun, EngineFX *engine, int interval_ms) {
    if (interval_ms <= 0) {
        interval_ms = 100;  // Default 100 milliseconds
    }
    
    StatusDisplay *status = malloc(sizeof(StatusDisplay));
    if (!status) {
        LOG_ERROR(LOG_SYSTEM, "Cannot allocate memory for status display");
        return nullptr;
    }
    
    status->gun = gun;
    status->engine = engine;
    status->interval_ms = interval_ms;
    atomic_init(&status->running, true);
    
    clock_gettime(CLOCK_MONOTONIC, &status->start_time);
    
    // Start display thread
    if (thrd_create(&status->display_thread, status_display_thread, status) != thrd_success) {
        LOG_ERROR(LOG_SYSTEM, "Failed to create status display thread");
        free(status);
        return nullptr;
    }
    
    LOG_INIT(LOG_SYSTEM, "Status display initialized");
    return status;
}

void status_display_destroy(StatusDisplay *status) {
    if (!status) return;
    
    // Stop display thread
    if (atomic_load(&status->running)) {
        atomic_store(&status->running, false);
        thrd_join(status->display_thread, nullptr);
    }
    
    free(status);
    LOG_SHUTDOWN(LOG_SYSTEM, "Status display");
}

void status_display_print_now(StatusDisplay *status) {
    if (!status) return;
    display_status(status);
}
