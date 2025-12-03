#include "config_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

// Helper function to expand tilde in paths
static void expand_path(const char *input, char *output, size_t output_size) {
    if (input[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = getenv("USERPROFILE");  // Windows fallback
        if (home) {
            snprintf(output, output_size, "%s%s", home, input + 1);
        } else {
            strncpy(output, input, output_size - 1);
        }
    } else {
        strncpy(output, input, output_size - 1);
    }
    output[output_size - 1] = '\0';
}

int config_load(const char *config_file, HeliFXConfig *config) {
    FILE *fh = fopen(config_file, "r");
    if (!fh) {
        fprintf(stderr, "[CONFIG] Error: Cannot open config file '%s'\n", config_file);
        return -1;
    }
    
    yaml_parser_t parser;
    yaml_event_t event;
    
    if (!yaml_parser_initialize(&parser)) {
        fprintf(stderr, "[CONFIG] Error: Failed to initialize YAML parser\n");
        fclose(fh);
        return -1;
    }
    
    yaml_parser_set_input_file(&parser, fh);
    
    // Initialize defaults
    memset(config, 0, sizeof(HeliFXConfig));
    config->engine.enabled = true;
    config->engine.pin = 17;
    config->engine.threshold_us = 1500;
    config->gun.enabled = true;
    config->gun.trigger_pin = 27;
    config->gun.rates = NULL;
    config->gun.rate_count = 0;
    config->gun.nozzle_flash_enabled = true;
    config->gun.nozzle_flash_pin = 23;
    config->gun.smoke_enabled = true;
    config->gun.smoke_fan_pin = 24;
    config->gun.smoke_heater_pin = 25;
    config->gun.smoke_heater_toggle_pin = 22;
    config->gun.smoke_heater_pwm_threshold_us = 1500;
    config->gun.smoke_fan_off_delay_ms = 2000;
    
    char current_section[64] = "";
    char current_key[64] = "";
    char current_subsection[64] = "";
    int in_rate = -1;
    int depth = 0;
    bool in_rate_sequence = false;
    
    bool done = false;
    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) {
            fprintf(stderr, "[CONFIG] Error: YAML parsing error\n");
            yaml_parser_delete(&parser);
            fclose(fh);
            return -1;
        }
        
        switch (event.type) {
            case YAML_MAPPING_START_EVENT:
                depth++;
                break;
                
            case YAML_MAPPING_END_EVENT:
                depth--;
                if (depth == 1) {
                    current_subsection[0] = '\0';
                    in_rate = -1;
                }
                if (depth == 2 && in_rate_sequence) {
                    in_rate = -1;  // Finished one rate entry
                }
                break;
                
            case YAML_SEQUENCE_START_EVENT:
                if (strcmp(current_key, "rate_of_fire") == 0) {
                    config->gun.rate_count = 0;
                    in_rate_sequence = true;
                }
                break;
                
            case YAML_SEQUENCE_END_EVENT:
                if (in_rate_sequence) {
                    in_rate_sequence = false;
                    in_rate = -1;
                }
                break;
                
            case YAML_SCALAR_EVENT: {
                char *value = (char *)event.data.scalar.value;
                
                if (depth == 1) {
                    // Top-level section
                    strncpy(current_section, value, sizeof(current_section) - 1);
                    current_subsection[0] = '\0';
                } else if (depth == 2) {
                    // Section key
                    strncpy(current_key, value, sizeof(current_key) - 1);
                    
                    if (strcmp(value, "engine_toggle") == 0 || 
                        strcmp(value, "sounds") == 0 ||
                        strcmp(value, "trigger") == 0 ||
                        strcmp(value, "nozzle_flash") == 0 ||
                        strcmp(value, "smoke") == 0) {
                        strncpy(current_subsection, value, sizeof(current_subsection) - 1);
                    }
                } else if (depth == 3) {
                    // Value or nested key
                    if (strcmp(current_section, "engine_fx") == 0) {
                        if (strcmp(current_subsection, "engine_toggle") == 0) {
                            if (strcmp(value, "pin") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->engine.pin = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "threshold_us") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->engine.threshold_us = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            }
                        } else if (strcmp(current_subsection, "sounds") == 0) {
                            strncpy(current_key, value, sizeof(current_key) - 1);
                        }
                    } else if (strcmp(current_section, "gun_fx") == 0) {
                        if (in_rate_sequence) {
                            // Rate of fire entry - we're inside the sequence
                            if (strcmp(value, "name") == 0) {
                                in_rate = config->gun.rate_count;
                                config->gun.rate_count++;
                                // Dynamically allocate/reallocate rates array
                                RateOfFireConfig *new_rates = realloc(config->gun.rates, 
                                                                     config->gun.rate_count * sizeof(RateOfFireConfig));
                                if (!new_rates) {
                                    fprintf(stderr, "[CONFIG] Error: Failed to allocate memory for rate of fire\n");
                                    yaml_event_delete(&event);
                                    yaml_parser_delete(&parser);
                                    fclose(fh);
                                    if (config->gun.rates) free(config->gun.rates);
                                    return -1;
                                }
                                config->gun.rates = new_rates;
                                memset(&config->gun.rates[in_rate], 0, sizeof(RateOfFireConfig));
                                yaml_parser_parse(&parser, &event);
                                strncpy(config->gun.rates[in_rate].name, 
                                       (char *)event.data.scalar.value,
                                       sizeof(config->gun.rates[in_rate].name) - 1);
                                yaml_event_delete(&event);
                                continue;
                            } else if (in_rate >= 0) {
                                if (strcmp(value, "rpm") == 0) {
                                    yaml_parser_parse(&parser, &event);
                                    config->gun.rates[in_rate].rpm = atoi((char *)event.data.scalar.value);
                                    yaml_event_delete(&event);
                                    continue;
                                } else if (strcmp(value, "pwm_threshold_us") == 0) {
                                    yaml_parser_parse(&parser, &event);
                                    config->gun.rates[in_rate].pwm_threshold_us = atoi((char *)event.data.scalar.value);
                                    yaml_event_delete(&event);
                                    continue;
                                } else if (strcmp(value, "sound") == 0) {
                                    yaml_parser_parse(&parser, &event);
                                    expand_path((char *)event.data.scalar.value,
                                              config->gun.rates[in_rate].sound_file,
                                              sizeof(config->gun.rates[in_rate].sound_file));
                                    yaml_event_delete(&event);
                                    continue;
                                }
                            }
                        } else if (strcmp(current_subsection, "trigger") == 0) {
                            if (strcmp(value, "pin") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.trigger_pin = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            }
                        } else if (strcmp(current_subsection, "nozzle_flash") == 0) {
                            if (strcmp(value, "enabled") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.nozzle_flash_enabled = (strcmp((char *)event.data.scalar.value, "true") == 0);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "pin") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.nozzle_flash_pin = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            }
                        } else if (strcmp(current_subsection, "smoke") == 0) {
                            if (strcmp(value, "enabled") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.smoke_enabled = (strcmp((char *)event.data.scalar.value, "true") == 0);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "fan_pin") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.smoke_fan_pin = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "heater_pin") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.smoke_heater_pin = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "heater_toggle_pin") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.smoke_heater_toggle_pin = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "heater_pwm_threshold_us") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.smoke_heater_pwm_threshold_us = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "fan_off_delay_ms") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->gun.smoke_fan_off_delay_ms = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            }
                        }
                    }
                } else if (depth == 4) {
                    // Nested values (sounds)
                    if (strcmp(current_section, "engine_fx") == 0 && strcmp(current_subsection, "sounds") == 0) {
                        if (strcmp(current_key, "starting") == 0 && strcmp(value, "file") == 0) {
                            yaml_parser_parse(&parser, &event);
                            expand_path((char *)event.data.scalar.value, config->engine.starting_file, sizeof(config->engine.starting_file));
                            yaml_event_delete(&event);
                            continue;
                        } else if (strcmp(current_key, "running") == 0 && strcmp(value, "file") == 0) {
                            yaml_parser_parse(&parser, &event);
                            expand_path((char *)event.data.scalar.value, config->engine.running_file, sizeof(config->engine.running_file));
                            yaml_event_delete(&event);
                            continue;
                        } else if (strcmp(current_key, "stopping") == 0 && strcmp(value, "file") == 0) {
                            yaml_parser_parse(&parser, &event);
                            expand_path((char *)event.data.scalar.value, config->engine.stopping_file, sizeof(config->engine.stopping_file));
                            yaml_event_delete(&event);
                            continue;
                        } else if (strcmp(current_key, "transitions") == 0) {
                            if (strcmp(value, "starting_offset_ms") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->engine.starting_offset_ms = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            } else if (strcmp(value, "stopping_offset_ms") == 0) {
                                yaml_parser_parse(&parser, &event);
                                config->engine.stopping_offset_ms = atoi((char *)event.data.scalar.value);
                                yaml_event_delete(&event);
                                continue;
                            }
                        }
                    }
                }
                break;
            }
                
            case YAML_STREAM_END_EVENT:
                done = true;
                break;
                
            default:
                break;
        }
        
        yaml_event_delete(&event);
    }
    
    yaml_parser_delete(&parser);
    fclose(fh);
    
    return 0;
}

int config_validate(const HeliFXConfig *config) {
    if (!config) {
        fprintf(stderr, "[CONFIG] Error: NULL configuration\n");
        return -1;
    }
    
    // Validate engine FX
    if (config->engine.enabled) {
        if (config->engine.pin < 0 || config->engine.pin > 27) {
            fprintf(stderr, "[CONFIG] Error: Invalid engine toggle pin %d (must be 0-27)\n", config->engine.pin);
            return -1;
        }
        if (config->engine.threshold_us <= 0) {
            fprintf(stderr, "[CONFIG] Error: Invalid engine threshold %d (must be > 0)\n", config->engine.threshold_us);
            return -1;
        }
    }
    
    // Validate gun FX
    if (config->gun.enabled) {
        if (config->gun.trigger_pin < 0 || config->gun.trigger_pin > 27) {
            fprintf(stderr, "[CONFIG] Error: Invalid trigger pin %d (must be 0-27)\n", config->gun.trigger_pin);
            return -1;
        }
        
        if (config->gun.nozzle_flash_enabled) {
            if (config->gun.nozzle_flash_pin < 0 || config->gun.nozzle_flash_pin > 27) {
                fprintf(stderr, "[CONFIG] Error: Invalid nozzle flash pin %d (must be 0-27)\n", config->gun.nozzle_flash_pin);
                return -1;
            }
        }
        
        if (config->gun.smoke_enabled) {
            if (config->gun.smoke_fan_pin < 0 || config->gun.smoke_fan_pin > 27) {
                fprintf(stderr, "[CONFIG] Error: Invalid smoke fan pin %d (must be 0-27)\n", config->gun.smoke_fan_pin);
                return -1;
            }
            if (config->gun.smoke_heater_pin < 0 || config->gun.smoke_heater_pin > 27) {
                fprintf(stderr, "[CONFIG] Error: Invalid smoke heater pin %d (must be 0-27)\n", config->gun.smoke_heater_pin);
                return -1;
            }
        }
        
        if (config->gun.rate_count <= 0) {
            fprintf(stderr, "[CONFIG] Error: No rates of fire configured\n");
            return -1;
        }
        
        for (int i = 0; i < config->gun.rate_count; i++) {
            if (config->gun.rates[i].rpm <= 0) {
                fprintf(stderr, "[CONFIG] Error: Invalid RPM %d for rate %d\n", config->gun.rates[i].rpm, i + 1);
                return -1;
            }
            if (config->gun.rates[i].pwm_threshold_us <= 0) {
                fprintf(stderr, "[CONFIG] Error: Invalid PWM threshold %d for rate %d\n", 
                       config->gun.rates[i].pwm_threshold_us, i + 1);
                return -1;
            }
        }
    }
    
    return 0;
}

void config_print(const HeliFXConfig *config) {
    printf("\n[CONFIG] Configuration:\n");
    printf("========================================\n");
    
    if (config->engine.enabled) {
        printf("Engine FX: ENABLED\n");
        printf("  Pin: %d\n", config->engine.pin);
        printf("  Threshold: %d µs\n", config->engine.threshold_us);
        printf("  Starting sound: %s\n", config->engine.starting_file);
        printf("  Running sound: %s\n", config->engine.running_file);
        printf("  Stopping sound: %s\n", config->engine.stopping_file);
        printf("  Starting offset: %d ms\n", config->engine.starting_offset_ms);
        printf("  Stopping offset: %d ms\n", config->engine.stopping_offset_ms);
    } else {
        printf("Engine FX: DISABLED\n");
    }
    
    printf("\n");
    
    if (config->gun.enabled) {
        printf("Gun FX: ENABLED\n");
        printf("  Trigger pin: %d\n", config->gun.trigger_pin);
        printf("  Nozzle flash: %s (pin %d)\n", 
               config->gun.nozzle_flash_enabled ? "ENABLED" : "DISABLED",
               config->gun.nozzle_flash_pin);
        printf("  Smoke: %s\n", config->gun.smoke_enabled ? "ENABLED" : "DISABLED");
        if (config->gun.smoke_enabled) {
            printf("    Fan pin: %d\n", config->gun.smoke_fan_pin);
            printf("    Heater pin: %d\n", config->gun.smoke_heater_pin);
            printf("    Heater toggle pin: %d\n", config->gun.smoke_heater_toggle_pin);
            printf("    Fan off delay: %d ms\n", config->gun.smoke_fan_off_delay_ms);
        }
        printf("  Rates of fire: %d\n", config->gun.rate_count);
        for (int i = 0; i < config->gun.rate_count; i++) {
            printf("    %d. %s: %d RPM @ %d µs (%s)\n",
                   i + 1,
                   config->gun.rates[i].name,
                   config->gun.rates[i].rpm,
                   config->gun.rates[i].pwm_threshold_us,
                   config->gun.rates[i].sound_file);
        }
    } else {
        printf("Gun FX: DISABLED\n");
    }
    
    printf("========================================\n\n");
}

void config_free(HeliFXConfig *config) {
    if (config && config->gun.rates) {
        free(config->gun.rates);
        config->gun.rates = NULL;
        config->gun.rate_count = 0;
    }
}
