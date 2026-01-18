#include "smoke_generator.h"
#include "gpio.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

struct SmokeGenerator {
    int heater_pin;
    int fan_pin;
    atomic_bool heater_on;
    atomic_bool fan_on;
};

SmokeGenerator* smoke_generator_create(int heater_pin, int fan_pin) {
    if (heater_pin < 0 || fan_pin < 0) {
        LOG_ERROR(LOG_SMOKE, "Invalid pin numbers");
        return nullptr;
    }
    
    SmokeGenerator *smoke = calloc(1, sizeof(SmokeGenerator));
    if (!smoke) {
        LOG_ERROR(LOG_SMOKE, "Cannot allocate memory for smoke generator");
        return nullptr;
    }
    
    smoke->heater_pin = heater_pin;
    smoke->fan_pin = fan_pin;
    atomic_init(&smoke->heater_on, false);
    atomic_init(&smoke->fan_on, false);
    
    // Set pins as outputs and initialize to LOW
    if (gpio_set_mode(heater_pin, GPIO_MODE_OUTPUT) < 0) {
        LOG_ERROR(LOG_SMOKE, "Failed to set heater pin %d as output", heater_pin);
        free(smoke);
        return nullptr;
    }
    
    if (gpio_set_mode(fan_pin, GPIO_MODE_OUTPUT) < 0) {
        LOG_ERROR(LOG_SMOKE, "Failed to set fan pin %d as output", fan_pin);
        free(smoke);
        return nullptr;
    }
    
    gpio_write(heater_pin, 0);
    gpio_write(fan_pin, 0);
    
    LOG_INFO(LOG_SMOKE, "Created (heater: pin %d, fan: pin %d)", heater_pin, fan_pin);
    return smoke;
}

void smoke_generator_destroy(SmokeGenerator *smoke) {
    if (!smoke) return;
    
    // Turn off both heater and fan
    gpio_write(smoke->heater_pin, 0);
    gpio_write(smoke->fan_pin, 0);
    
    free(smoke);
    
    LOG_INFO(LOG_SMOKE, "Smoke generator destroyed");
}

int smoke_generator_heater_on(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    atomic_store(&smoke->heater_on, true);
    
    gpio_write(smoke->heater_pin, 1);
    LOG_INFO(LOG_SMOKE, "Heater ON");
    
    return 0;
}

int smoke_generator_heater_off(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    atomic_store(&smoke->heater_on, false);
    
    gpio_write(smoke->heater_pin, 0);
    LOG_INFO(LOG_SMOKE, "Heater OFF");
    
    return 0;
}

int smoke_generator_fan_on(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    atomic_store(&smoke->fan_on, true);
    
    gpio_write(smoke->fan_pin, 1);
    LOG_INFO(LOG_SMOKE, "Fan ON");
    
    return 0;
}

int smoke_generator_fan_off(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    atomic_store(&smoke->fan_on, false);
    
    gpio_write(smoke->fan_pin, 0);
    LOG_INFO(LOG_SMOKE, "Fan OFF");
    
    return 0;
}

bool smoke_generator_is_heater_on(SmokeGenerator *smoke) {
    if (!smoke) return false;
    
    return atomic_load(&smoke->heater_on);
}

bool smoke_generator_is_fan_on(SmokeGenerator *smoke) {
    if (!smoke) return false;
    
    return atomic_load(&smoke->fan_on);
}

