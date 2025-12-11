#include "gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <threads.h>
#include "logging.h"

// BCM2835/BCM2711 GPIO register offsets
#define BCM2835_PERI_BASE   0x20000000  // BCM2835 (Pi 1, Zero)
#define BCM2711_PERI_BASE   0xFE000000  // BCM2711 (Pi 4)
#define GPIO_OFFSET         0x200000

#define BLOCK_SIZE          (4*1024)

// GPIO register offsets
#define GPFSEL0     0   // Function Select 0
#define GPSET0      7   // Pin Output Set 0
#define GPCLR0      10  // Pin Output Clear 0
#define GPLEV0      13  // Pin Level 0
#define GPPUD       37  // Pull-up/down Enable
#define GPPUDCLK0   38  // Pull-up/down Enable Clock 0

// For BCM2711 (Pi 4)
#define GPPUPPDN0   57  // Pull-up/down Register 0

static volatile unsigned int *gpio_map = nullptr;
static bool initialized = false;
static unsigned int gpio_base = BCM2835_PERI_BASE;

// Detect Raspberry Pi model and return appropriate base address
static unsigned int detect_gpio_base(void) {
    FILE *fp = fopen("/proc/device-tree/model", "r");
    if (fp) {
        char model[256];
        if (fgets(model, sizeof(model), fp)) {
            if (strstr(model, "Raspberry Pi 4") || strstr(model, "Raspberry Pi 400")) {
                fclose(fp);
                return BCM2711_PERI_BASE;
            }
        }
        fclose(fp);
    }
    return BCM2835_PERI_BASE;
}

int gpio_init(void) {
    if (initialized) {
        fprintf(stderr, "[GPIO] Warning: GPIO already initialized\n");
        return 0;
    }
    
    int mem_fd;
    void *gpio_mem;
    
    // Detect the correct base address for this Pi model
    gpio_base = detect_gpio_base();
    printf("[GPIO] Using base address: 0x%08X\n", gpio_base);
    
    // Open /dev/gpiomem for direct memory access
    mem_fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "[GPIO] Error: Cannot open /dev/gpiomem (try running with sudo)\n");
        return -1;
    }
    
    // Map GPIO memory
    gpio_mem = mmap(
        nullptr,
        BLOCK_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        mem_fd,
        0  // /dev/gpiomem already points to GPIO base
    );
    
    close(mem_fd);
    
    if (gpio_mem == MAP_FAILED) {
        fprintf(stderr, "[GPIO] Error: mmap failed: %s\n", strerror(errno));
        return -1;
    }
    
    gpio_map = (volatile unsigned int *)gpio_mem;
    initialized = true;
    
    printf("[GPIO] GPIO subsystem initialized\n");
    return 0;
}

void gpio_cleanup(void) {
    if (!initialized) return;
    
    if (gpio_map != nullptr) {
        munmap((void *)gpio_map, BLOCK_SIZE);
        gpio_map = nullptr;
    }
    
    initialized = false;
    printf("[GPIO] GPIO subsystem cleaned up\n");
}

int gpio_set_mode(int pin, GPIOMode mode) {
    if (!initialized) {
        fprintf(stderr, "[GPIO] Error: GPIO not initialized\n");
        return -1;
    }
    
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "[GPIO] Error: Invalid pin number %d (must be 0-27)\n", pin);
        return -1;
    }
    
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    
    // Read current value
    unsigned int val = gpio_map[GPFSEL0 + reg];
    
    // Clear the 3 bits for this pin
    val &= ~(7 << shift);
    
    // Set mode (000 = input, 001 = output)
    if (mode == GPIO_MODE_OUTPUT) {
        val |= (1 << shift);
    }
    
    gpio_map[GPFSEL0 + reg] = val;
    
    return 0;
}

int gpio_set_pull(int pin, GPIOPull pull) {
    if (!initialized) {
        fprintf(stderr, "[GPIO] Error: GPIO not initialized\n");
        return -1;
    }
    
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "[GPIO] Error: Invalid pin number %d (must be 0-27)\n", pin);
        return -1;
    }
    
    if (gpio_base == BCM2711_PERI_BASE) {
        // BCM2711 (Pi 4) method
        int reg = GPPUPPDN0 + (pin / 16);
        int shift = (pin % 16) * 2;
        
        unsigned int val = gpio_map[reg];
        val &= ~(3 << shift);
        val |= (pull << shift);
        gpio_map[reg] = val;
    } else {
        // BCM2835 (older Pi) method
        gpio_map[GPPUD] = pull;
        usleep(10);
        gpio_map[GPPUDCLK0] = (1 << pin);
        usleep(10);
        gpio_map[GPPUD] = 0;
        gpio_map[GPPUDCLK0] = 0;
    }
    
    return 0;
}

int gpio_write(int pin, bool value) {
    if (!initialized) {
        fprintf(stderr, "[GPIO] Error: GPIO not initialized\n");
        return -1;
    }
    
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "[GPIO] Error: Invalid pin number %d (must be 0-27)\n", pin);
        return -1;
    }
    
    if (value) {
        gpio_map[GPSET0] = (1 << pin);
    } else {
        gpio_map[GPCLR0] = (1 << pin);
    }
    
    return 0;
}

bool gpio_read(int pin) {
    if (!initialized) {
        fprintf(stderr, "[GPIO] Error: GPIO not initialized\n");
        return false;
    }
    
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "[GPIO] Error: Invalid pin number %d (must be 0-27)\n", pin);
        return false;
    }
    
    return (gpio_map[GPLEV0] & (1 << pin)) ? true : false;
}

int gpio_toggle(int pin) {
    bool current = gpio_read(pin);
    return gpio_write(pin, !current);
}

int gpio_set_edge(int pin, GPIOEdge edge) {
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "[GPIO] Error: Invalid pin number %d (must be 0-27)\n", pin);
        return -1;
    }
    
    char path[256];
    char value[16];
    FILE *fp;
    
    // Export pin if needed
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    if (access(path, F_OK) != 0) {
        fp = fopen("/sys/class/gpio/export", "w");
        if (fp) {
            fprintf(fp, "%d", pin);
            fclose(fp);
            usleep(100000); // Wait for sysfs to create files
        }
    }
    
    // Set edge detection
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/edge", pin);
    fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "[GPIO] Error: Cannot set edge for pin %d\n", pin);
        return -1;
    }
    
    switch (edge) {
        case GPIO_EDGE_NONE:
            strcpy(value, "none");
            break;
        case GPIO_EDGE_RISING:
            strcpy(value, "rising");
            break;
        case GPIO_EDGE_FALLING:
            strcpy(value, "falling");
            break;
        case GPIO_EDGE_BOTH:
            strcpy(value, "both");
            break;
    }
    
    fprintf(fp, "%s", value);
    fclose(fp);
    
    return 0;
}

int gpio_wait_for_edge(int pin, int timeout_ms) {
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "[GPIO] Error: Invalid pin number %d (must be 0-27)\n", pin);
        return -1;
    }
    
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[GPIO] Error: Cannot open %s\n", path);
        return -1;
    }
    
    // Read initial value to clear any pending interrupt
    char buf[2];
    read(fd, buf, sizeof(buf));
    
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLPRI | POLLERR;
    
    int ret = poll(&pfd, 1, timeout_ms);
    
    close(fd);
    
    if (ret > 0) {
        return 1; // Edge detected
    } else if (ret == 0) {
        return 0; // Timeout
    } else {
        fprintf(stderr, "[GPIO] Error: poll failed: %s\n", strerror(errno));
        return -1;
    }
}

int gpio_read_pwm_duration(int pin, int timeout_us) {
    if (!initialized) {
        fprintf(stderr, "[GPIO] Error: GPIO not initialized\n");
        return -1;
    }
    
    if (pin < 0 || pin > 27) {
        fprintf(stderr, "[GPIO] Error: Invalid pin number %d (must be 0-27)\n", pin);
        return -1;
    }
    
    struct timespec start, end;
    long elapsed_us;
    int timeout_counter = 0;
    int max_wait = timeout_us;
    
    // Wait for signal to go LOW (start of pulse cycle)
    while (gpio_read(pin)) {
        usleep(1);
        timeout_counter++;
        if (timeout_counter >= max_wait) {
            fprintf(stderr, "[GPIO] Error: Timeout waiting for LOW signal on pin %d\n", pin);
            return -1;
        }
    }
    
    // Wait for signal to go HIGH (start of pulse)
    timeout_counter = 0;
    while (!gpio_read(pin)) {
        usleep(1);
        timeout_counter++;
        if (timeout_counter >= max_wait) {
            fprintf(stderr, "[GPIO] Error: Timeout waiting for HIGH signal on pin %d\n", pin);
            return -1;
        }
    }
    
    // Record start time when signal goes HIGH
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Wait for signal to go LOW (end of pulse)
    timeout_counter = 0;
    while (gpio_read(pin)) {
        usleep(1);
        timeout_counter++;
        if (timeout_counter >= max_wait) {
            fprintf(stderr, "[GPIO] Error: Timeout waiting for pulse to end on pin %d\n", pin);
            return -1;
        }
    }
    
    // Record end time when signal goes LOW
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    // Calculate duration in microseconds
    elapsed_us = (end.tv_sec - start.tv_sec) * 1000000L + 
                 (end.tv_nsec - start.tv_nsec) / 1000L;
    
    return (int)elapsed_us;
}

// ============================================================================
// ASYNC PWM MONITOR IMPLEMENTATION
// ============================================================================

struct PWMMonitor {
    int pin;
    thrd_t thread;
    mtx_t mutex;
    cnd_t cond;
    
    bool running;
    bool has_new_reading;
    
    PWMReading current_reading;
    
    PWMCallback callback;
    void *user_data;
    
    int timeout_us;
};

static int pwm_monitor_thread(void *arg) {
    PWMMonitor *monitor = (PWMMonitor *)arg;
    
    LOG_INFO(LOG_GPIO, "PWM monitor thread started for pin %d (timeout: %d µs)", 
             monitor->pin, monitor->timeout_us);
    
    int timeout_count = 0;
    bool first_signal_received = false;
    
    while (monitor->running) {
        int duration = gpio_read_pwm_duration(monitor->pin, monitor->timeout_us);
        
        if (duration > 0) {
            if (!first_signal_received) {
                LOG_INFO(LOG_GPIO, "First PWM signal received on pin %d: %d µs",
                         monitor->pin, duration);
                first_signal_received = true;
            }
            
            PWMReading reading;
            reading.pin = monitor->pin;
            reading.duration_us = duration;
            
            mtx_lock(&monitor->mutex);
            
            // Update current reading (overwrite previous)
            monitor->current_reading = reading;
            monitor->has_new_reading = true;
            
            // Signal waiting threads
            cnd_broadcast(&monitor->cond);
            
            mtx_unlock(&monitor->mutex);
            
            // Call callback if set
            if (monitor->callback) {
                monitor->callback(reading, monitor->user_data);
            }
            
            timeout_count = 0; // Reset timeout counter on successful read
        } else if (duration == -1) {
            // Timeout or error, continue monitoring
            timeout_count++;
            if (timeout_count == 10 && !first_signal_received) {
                LOG_WARN(LOG_GPIO, "No PWM signal detected on pin %d after 10 attempts", monitor->pin);
            }
            usleep(1000); // Small delay to prevent busy loop
        }
    }
    
    LOG_INFO(LOG_GPIO, "PWM monitor thread stopped for pin %d", monitor->pin);
    return thrd_success;
}

PWMMonitor* pwm_monitor_create(int pin, PWMCallback callback, void *user_data) {
    if (!initialized) {
        LOG_ERROR(LOG_GPIO, "GPIO not initialized");
        return nullptr;
    }
    
    if (pin < 0 || pin > 27) {
        LOG_ERROR(LOG_GPIO, "Invalid pin number %d (must be 0-27)", pin);
        return nullptr;
    }
    
    PWMMonitor *monitor = calloc(1, sizeof(PWMMonitor));
    if (!monitor) {
        LOG_ERROR(LOG_GPIO, "Cannot allocate memory for PWM monitor");
        return nullptr;
    }
    
    monitor->pin = pin;
    monitor->callback = callback;
    monitor->user_data = user_data;
    monitor->running = false;
    monitor->has_new_reading = false;
    monitor->timeout_us = 100000; // 100ms default timeout
    
    mtx_init(&monitor->mutex, mtx_plain);
    cnd_init(&monitor->cond);
    
    // Set pin as input
    if (gpio_set_mode(pin, GPIO_MODE_INPUT) < 0) {
        LOG_ERROR(LOG_GPIO, "Failed to set pin %d as input", pin);
        mtx_destroy(&monitor->mutex);
        cnd_destroy(&monitor->cond);
        free(monitor);
        return nullptr;
    }
    
    LOG_INFO(LOG_GPIO, "PWM monitor created for pin %d", pin);
    return monitor;
}

void pwm_monitor_destroy(PWMMonitor *monitor) {
    if (!monitor) return;
    
    if (monitor->running) {
        pwm_monitor_stop(monitor);
    }
    
    mtx_destroy(&monitor->mutex);
    cnd_destroy(&monitor->cond);
    free(monitor);
    
    LOG_INFO(LOG_GPIO, "PWM monitor destroyed");
}
int pwm_monitor_start(PWMMonitor *monitor) {
    if (!monitor) return -1;
    
    if (monitor->running) {
        LOG_WARN(LOG_GPIO, "PWM monitor already running");
        return 0;
    }
    
    monitor->running = true;
    
    if (thrd_create(&monitor->thread, pwm_monitor_thread, monitor) != thrd_success) {
        LOG_ERROR(LOG_GPIO, "Failed to create PWM monitor thread");
        monitor->running = false;
        return -1;
    }
    
    LOG_INFO(LOG_GPIO, "PWM monitor started");
    return 0;
}

int pwm_monitor_stop(PWMMonitor *monitor) {
    if (!monitor) return -1;
    
    if (!monitor->running) {
        return 0;
    }
    
    monitor->running = false;
    
    thrd_join(monitor->thread, nullptr);
    
    LOG_INFO(LOG_GPIO, "PWM monitor stopped");
    return 0;
}

bool pwm_monitor_get_reading(PWMMonitor *monitor, PWMReading *reading) {
    if (!monitor || !reading) return false;
    
    mtx_lock(&monitor->mutex);
    
    bool has_reading = monitor->has_new_reading;
    if (has_reading) {
        *reading = monitor->current_reading;
        monitor->has_new_reading = false;
    }
    
    mtx_unlock(&monitor->mutex);
    
    return has_reading;
}

bool pwm_monitor_wait_reading(PWMMonitor *monitor, PWMReading *reading, int timeout_ms) {
    if (!monitor || !reading) return false;
    
    mtx_lock(&monitor->mutex);
    
    if (timeout_ms < 0) {
        // Wait indefinitely
        while (!monitor->has_new_reading && monitor->running) {
            cnd_wait(&monitor->cond, &monitor->mutex);
        }
    } else {
        // Wait with timeout
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        
        while (!monitor->has_new_reading && monitor->running) {
            if (cnd_timedwait(&monitor->cond, &monitor->mutex, &ts) != thrd_success) {
                break; // Timeout
            }
        }
    }
    
    bool has_reading = monitor->has_new_reading;
    if (has_reading) {
        *reading = monitor->current_reading;
        monitor->has_new_reading = false;
    }
    
    mtx_unlock(&monitor->mutex);
    
    return has_reading;
}

bool pwm_monitor_is_running(PWMMonitor *monitor) {
    if (!monitor) return false;
    return monitor->running;
}

bool gpio_is_initialized(void) {
    return initialized;
}
