#include "gpio.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <gpio_pin> [freq_hz] [--verbose]\n", argv[0]);
        return 1;
    }

    int pin = atoi(argv[1]);
    int freq_hz = 0;
    int verbose = 0;
    if (argc >= 3) {
        if (strcmp(argv[2], "--verbose") == 0 || strcmp(argv[2], "-v") == 0) {
            verbose = 1;
        } else {
            freq_hz = atoi(argv[2]);
        }
    }
    if (argc >= 4 && (strcmp(argv[3], "--verbose") == 0 || strcmp(argv[3], "-v") == 0)) {
        verbose = 1;
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    if (logging_init(NULL, 0, 0) != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return 1;
    }

    if (freq_hz > 0) {
        LOG_INFO(LOG_SYSTEM, "PWM emitter test starting on GPIO %d at %d Hz", pin, freq_hz);
    } else {
        LOG_INFO(LOG_SYSTEM, "PWM emitter test starting on GPIO %d (default 50 Hz)", pin);
    }

    int exit_code = 0;
    PWMEmitter *emitter = NULL;

    if (gpio_init() < 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to initialize GPIO");
        exit_code = 1;
        goto cleanup;
    }

    emitter = pwm_emitter_create(pin, "test");
    if (!emitter) {
        LOG_ERROR(LOG_SYSTEM, "Failed to create PWM emitter on pin %d", pin);
        exit_code = 1;
        goto cleanup;
    }
    if (freq_hz > 0) {
        pwm_emitter_set_frequency(emitter, freq_hz);
    }

    // Cycle from 1000us to 2000us and back, full cycle every 5 seconds
    const int min_us = 1000;
    const int max_us = 2000;
    const int step_ms = 25;          // 25ms step -> ~5s full sweep (2.5s each direction)
    int direction = 1;
    int value = min_us;
    int step_count = 0;

    while (running) {
        if (pwm_emitter_set_value(emitter, value) != 0) {
            LOG_ERROR(LOG_SYSTEM, "Failed to set PWM value to %d us", value);
            exit_code = 1;
            break;
        }

        if (verbose && (step_count % 20 == 0)) {
            LOG_INFO(LOG_SYSTEM, "PWM value: %d us", value);
        }
        step_count++;

        // Update value
        value += direction * 10; // 10us per step
        if (value >= max_us) {
            value = max_us;
            direction = -1;
        } else if (value <= min_us) {
            value = min_us;
            direction = 1;
        }

        usleep(step_ms * 1000);
    }

    LOG_INFO(LOG_SYSTEM, "Stopping PWM emitter test");

cleanup:
    if (emitter) {
        pwm_emitter_destroy(emitter);
    }
    gpio_cleanup();
    logging_shutdown();
    return exit_code;
}
