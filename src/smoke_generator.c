#include "smoke_generator.h"
#include "gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

struct SmokeGenerator {
    int heater_pin;
    int fan_pin;
    bool heater_on;
    bool fan_on;
    pthread_mutex_t mutex;
};

SmokeGenerator* smoke_generator_create(int heater_pin, int fan_pin) {
    if (heater_pin < 0 || fan_pin < 0) {
        fprintf(stderr, "[SMOKE] Error: Invalid pin numbers\n");
        return NULL;
    }
    
    SmokeGenerator *smoke = (SmokeGenerator *)calloc(1, sizeof(SmokeGenerator));
    if (!smoke) {
        fprintf(stderr, "[SMOKE] Error: Cannot allocate memory for smoke generator\n");
        return NULL;
    }
    
    smoke->heater_pin = heater_pin;
    smoke->fan_pin = fan_pin;
    smoke->heater_on = false;
    smoke->fan_on = false;
    pthread_mutex_init(&smoke->mutex, NULL);
    
    // Set pins as outputs and initialize to LOW
    if (gpio_set_mode(heater_pin, GPIO_MODE_OUTPUT) < 0) {
        fprintf(stderr, "[SMOKE] Error: Failed to set heater pin %d as output\n", heater_pin);
        pthread_mutex_destroy(&smoke->mutex);
        free(smoke);
        return NULL;
    }
    
    if (gpio_set_mode(fan_pin, GPIO_MODE_OUTPUT) < 0) {
        fprintf(stderr, "[SMOKE] Error: Failed to set fan pin %d as output\n", fan_pin);
        pthread_mutex_destroy(&smoke->mutex);
        free(smoke);
        return NULL;
    }
    
    gpio_write(heater_pin, 0);
    gpio_write(fan_pin, 0);
    
    printf("[SMOKE] Smoke generator created (heater: pin %d, fan: pin %d)\n", heater_pin, fan_pin);
    return smoke;
}

void smoke_generator_destroy(SmokeGenerator *smoke) {
    if (!smoke) return;
    
    // Turn off both heater and fan
    gpio_write(smoke->heater_pin, 0);
    gpio_write(smoke->fan_pin, 0);
    
    pthread_mutex_destroy(&smoke->mutex);
    free(smoke);
    
    printf("[SMOKE] Smoke generator destroyed\n");
}

int smoke_generator_heater_on(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    pthread_mutex_lock(&smoke->mutex);
    smoke->heater_on = true;
    pthread_mutex_unlock(&smoke->mutex);
    
    gpio_write(smoke->heater_pin, 1);
    printf("[SMOKE] Heater ON\n");
    
    return 0;
}

int smoke_generator_heater_off(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    pthread_mutex_lock(&smoke->mutex);
    smoke->heater_on = false;
    pthread_mutex_unlock(&smoke->mutex);
    
    gpio_write(smoke->heater_pin, 0);
    printf("[SMOKE] Heater OFF\n");
    
    return 0;
}

int smoke_generator_fan_on(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    pthread_mutex_lock(&smoke->mutex);
    smoke->fan_on = true;
    pthread_mutex_unlock(&smoke->mutex);
    
    gpio_write(smoke->fan_pin, 1);
    printf("[SMOKE] Fan ON\n");
    
    return 0;
}

int smoke_generator_fan_off(SmokeGenerator *smoke) {
    if (!smoke) return -1;
    
    pthread_mutex_lock(&smoke->mutex);
    smoke->fan_on = false;
    pthread_mutex_unlock(&smoke->mutex);
    
    gpio_write(smoke->fan_pin, 0);
    printf("[SMOKE] Fan OFF\n");
    
    return 0;
}

bool smoke_generator_is_heater_on(SmokeGenerator *smoke) {
    if (!smoke) return false;
    
    pthread_mutex_lock(&smoke->mutex);
    bool on = smoke->heater_on;
    pthread_mutex_unlock(&smoke->mutex);
    
    return on;
}

bool smoke_generator_is_fan_on(SmokeGenerator *smoke) {
    if (!smoke) return false;
    
    pthread_mutex_lock(&smoke->mutex);
    bool on = smoke->fan_on;
    pthread_mutex_unlock(&smoke->mutex);
    
    return on;
}
