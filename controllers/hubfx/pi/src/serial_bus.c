#include "serial_bus.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stdint.h>

struct SerialBus {
    int fd;                     // File descriptor
    char device_path[256];      // Device path
    int baud_rate;              // Baud rate
    int timeout_ms;             // Read timeout
    struct termios orig_termios; // Original terminal settings (for restore)
};

// Convert baud rate integer to termios constant
static speed_t get_baud_constant(int baud_rate) {
    switch (baud_rate) {
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 500000:  return B500000;
        case 576000:  return B576000;
        case 921600:  return B921600;
        case 1000000: return B1000000;
        case 1152000: return B1152000;
        case 1500000: return B1500000;
        case 2000000: return B2000000;
        case 2500000: return B2500000;
        case 3000000: return B3000000;
        case 3500000: return B3500000;
        case 4000000: return B4000000;
        default:
            LOG_ERROR(LOG_SYSTEM, "Unsupported baud rate: %d", baud_rate);
            return B0;
    }
}

SerialBus* serial_bus_open(const SerialBusConfig *config) {
    if (!config || !config->device_path) {
        LOG_ERROR(LOG_SYSTEM, "Invalid serial bus configuration");
        return NULL;
    }

    SerialBus *bus = malloc(sizeof(SerialBus));
    if (!bus) {
        LOG_ERROR(LOG_SYSTEM, "Failed to allocate serial bus");
        return NULL;
    }

    strncpy(bus->device_path, config->device_path, sizeof(bus->device_path) - 1);
    bus->device_path[sizeof(bus->device_path) - 1] = '\0';
    bus->baud_rate = config->baud_rate;
    bus->timeout_ms = config->timeout_ms;

    // Open serial device
    bus->fd = open(config->device_path, O_RDWR | O_NOCTTY | O_SYNC);
    if (bus->fd < 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to open serial device %s: %s", 
                 config->device_path, strerror(errno));
        free(bus);
        return NULL;
    }

    // Save original terminal settings
    if (tcgetattr(bus->fd, &bus->orig_termios) != 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to get terminal attributes: %s", strerror(errno));
        close(bus->fd);
        free(bus);
        return NULL;
    }

    // Configure serial port
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    
    if (tcgetattr(bus->fd, &tty) != 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to get terminal attributes: %s", strerror(errno));
        close(bus->fd);
        free(bus);
        return NULL;
    }

    // Set baud rate
    speed_t baud = get_baud_constant(config->baud_rate);
    if (baud == B0) {
        close(bus->fd);
        free(bus);
        return NULL;
    }
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    // 8N1 mode, no hardware flow control
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
    tty.c_cflag |= (CLOCAL | CREAD);                // Enable receiver, ignore modem controls
    tty.c_cflag &= ~(PARENB | PARODD);              // No parity
    tty.c_cflag &= ~CSTOPB;                         // 1 stop bit
    tty.c_cflag &= ~CRTSCTS;                        // No hardware flow control

    // Raw mode
    tty.c_lflag = 0;                                // No signaling chars, no echo, no canonical processing
    tty.c_oflag = 0;                                // No output processing
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);         // No software flow control
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL); // No special handling

    // Set timeout (deciseconds)
    tty.c_cc[VMIN]  = 0;                            // Non-blocking read
    tty.c_cc[VTIME] = (config->timeout_ms + 99) / 100; // Convert ms to deciseconds (rounded up)

    // Apply settings
    if (tcsetattr(bus->fd, TCSANOW, &tty) != 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to set terminal attributes: %s", strerror(errno));
        close(bus->fd);
        free(bus);
        return NULL;
    }

    // Flush any stale data
    tcflush(bus->fd, TCIOFLUSH);

    LOG_INFO(LOG_SYSTEM, "Serial bus opened: %s @ %d baud (timeout: %d ms)", 
             config->device_path, config->baud_rate, config->timeout_ms);

    return bus;
}

void serial_bus_close(SerialBus *bus) {
    if (!bus) return;

    if (bus->fd >= 0) {
        // Restore original terminal settings
        tcsetattr(bus->fd, TCSANOW, &bus->orig_termios);
        close(bus->fd);
        LOG_INFO(LOG_SYSTEM, "Serial bus closed: %s", bus->device_path);
    }

    free(bus);
}

int serial_bus_write(SerialBus *bus, const void *data, size_t len) {
    if (!bus || bus->fd < 0 || !data) {
        LOG_ERROR(LOG_SYSTEM, "Invalid serial bus or data");
        return -1;
    }

    ssize_t n = write(bus->fd, data, len);
    if (n < 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to write to serial bus: %s", strerror(errno));
        return -1;
    }

    LOG_DEBUG(LOG_SYSTEM, "Serial TX: %zd bytes", n);
    return (int)n;
}

int serial_bus_write_string(SerialBus *bus, const char *str) {
    if (!str) return -1;
    return serial_bus_write(bus, str, strlen(str));
}

int serial_bus_write_command(SerialBus *bus, const char *format, ...) {
    if (!bus || !format) return -1;

    char buffer[512];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);

    if (len < 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to format command");
        return -1;
    }

    if ((size_t)len >= sizeof(buffer) - 1) {
        LOG_WARN(LOG_SYSTEM, "Command truncated (too long)");
        len = sizeof(buffer) - 2;
    }

    // Add newline
    buffer[len] = '\n';
    buffer[len + 1] = '\0';

    LOG_DEBUG(LOG_SYSTEM, "Serial CMD: %s", buffer);
    return serial_bus_write(bus, buffer, len + 1);
}

int serial_bus_read(SerialBus *bus, void *buffer, size_t max_len) {
    if (!bus || bus->fd < 0 || !buffer) {
        LOG_ERROR(LOG_SYSTEM, "Invalid serial bus or buffer");
        return -1;
    }

    ssize_t n = read(bus->fd, buffer, max_len);
    if (n < 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to read from serial bus: %s", strerror(errno));
        return -1;
    }

    if (n > 0) {
        LOG_DEBUG(LOG_SYSTEM, "Serial RX: %zd bytes", n);
    }

    return (int)n;
}

int serial_bus_read_line(SerialBus *bus, char *buffer, size_t max_len) {
    if (!bus || !buffer || max_len == 0) {
        LOG_ERROR(LOG_SYSTEM, "Invalid arguments");
        return -1;
    }

    size_t pos = 0;
    while (pos < max_len - 1) {
        char c;
        int n = serial_bus_read(bus, &c, 1);
        
        if (n < 0) {
            return -1;  // Error
        } else if (n == 0) {
            // Timeout
            if (pos > 0) {
                // Return partial line
                buffer[pos] = '\0';
                LOG_DEBUG(LOG_SYSTEM, "Serial RX line (partial): %s", buffer);
                return (int)pos;
            }
            return 0;  // No data
        }

        if (c == '\n' || c == '\r') {
            // End of line
            buffer[pos] = '\0';
            if (pos > 0) {
                LOG_DEBUG(LOG_SYSTEM, "Serial RX line: %s", buffer);
            }
            return (int)pos;
        }

        buffer[pos++] = c;
    }

    // Buffer full
    buffer[max_len - 1] = '\0';
    LOG_WARN(LOG_SYSTEM, "Serial line buffer full (truncated)");
    return (int)(max_len - 1);
}

bool serial_bus_is_ready(SerialBus *bus) {
    return bus && bus->fd >= 0;
}

int serial_bus_flush(SerialBus *bus) {
    if (!bus || bus->fd < 0) {
        return -1;
    }

    if (tcflush(bus->fd, TCIOFLUSH) != 0) {
        LOG_ERROR(LOG_SYSTEM, "Failed to flush serial bus: %s", strerror(errno));
        return -1;
    }

    return 0;
}

// ---------------------- Binary framing helpers (COBS + CRC8) ----------------------

uint8_t serial_bus_crc8_poly_07(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x07U) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

size_t serial_bus_cobs_encode(const uint8_t *input, size_t length, uint8_t *output, size_t output_cap) {
    if (!input || !output || output_cap == 0) return 0;
    size_t read_index = 0;
    size_t write_index = 1; // first code byte reserved
    size_t code_index = 0;
    uint8_t code = 1;

    while (read_index < length) {
        if (write_index >= output_cap) return 0;
        if (input[read_index] == 0) {
            output[code_index] = code;
            code_index = write_index++;
            code = 1;
            read_index++;
        } else {
            output[write_index++] = input[read_index++];
            code++;
            if (code == 0xFF) {
                if (write_index >= output_cap) return 0;
                output[code_index] = code;
                code_index = write_index++;
                code = 1;
            }
        }
    }
    if (code_index >= output_cap) return 0;
    output[code_index] = code;
    return write_index;
}

size_t serial_bus_cobs_decode(const uint8_t *input, size_t length, uint8_t *output, size_t output_cap) {
    if (!input || !output) return 0;
    size_t read_index = 0;
    size_t write_index = 0;

    while (read_index < length) {
        uint8_t code = input[read_index++];
        if (code == 0 || read_index + code - 1 > length) {
            return 0; // invalid
        }
        for (uint8_t i = 1; i < code; i++) {
            if (write_index >= output_cap || read_index >= length) return 0;
            output[write_index++] = input[read_index++];
        }
        if (code < 0xFF && read_index < length) {
            if (write_index >= output_cap) return 0;
            output[write_index++] = 0x00;
        }
    }
    return write_index;
}

int serial_bus_send_packet(SerialBus *bus, uint8_t type, const uint8_t *payload, size_t payload_len) {
    if (!bus) return -1;
    if (payload_len > 255) return -1;

    // Conservative buffer sizes
    const size_t max_packet = 2 /*type+len*/ + payload_len + 1 /*crc*/;
    uint8_t packet[2 + 255 + 1];
    size_t idx = 0;
    packet[idx++] = type;
    packet[idx++] = (uint8_t)payload_len;
    if (payload_len && payload) {
        memcpy(packet + idx, payload, payload_len);
        idx += payload_len;
    }
    packet[idx++] = serial_bus_crc8_poly_07(packet, idx);

    // Worst-case COBS expansion is +length/254 + 1
    uint8_t encoded[2 + 255 + 1 + 2];
    size_t enc_len = serial_bus_cobs_encode(packet, idx, encoded, sizeof(encoded));
    if (enc_len == 0) {
        LOG_ERROR(LOG_SYSTEM, "COBS encode failed (type=0x%02X)", type);
        return -1;
    }
    if (enc_len >= sizeof(encoded)) {
        LOG_ERROR(LOG_SYSTEM, "Encoded packet too large");
        return -1;
    }
    encoded[enc_len++] = 0x00; // delimiter

    int written = serial_bus_write(bus, encoded, enc_len);
    if (written < 0) {
        LOG_ERROR(LOG_SYSTEM, "Serial write failed (type=0x%02X)", type);
        return -1;
    }
    return 0;
}

// ---------------------- USB Device Detection ----------------------

/**
 * Find and open a USB serial device by VID/PID
 * Searches /sys/bus/usb/devices for matching device and opens its ttyACM port
 */
SerialBus* serial_bus_open_by_vid_pid(uint16_t vendor_id, uint16_t product_id, SerialBusConfig *config) {
    if (!config) {
        LOG_ERROR(LOG_SYSTEM, "Invalid serial bus configuration");
        return NULL;
    }
    
    char vid_pid_str[32];
    snprintf(vid_pid_str, sizeof(vid_pid_str), "%04x:%04x", vendor_id, product_id);
    
    LOG_DEBUG(LOG_SYSTEM, "Searching for USB device %s", vid_pid_str);
    
    // Search /sys/bus/usb-serial/devices for matching device
    FILE *lsusb = popen("lsusb | grep -i \"" XSTR(vendor_id) ":" XSTR(product_id) "\"", "r");
    if (!lsusb) {
        // Fallback: manually search /dev for ttyACM devices and check their VID/PID
        // This is more reliable on some systems
        for (int i = 0; i < 10; i++) {
            char device_path[64];
            snprintf(device_path, sizeof(device_path), "/dev/ttyACM%d", i);
            
            // Try to open and see if it's the device we want
            // For now, just try the first available ttyACM device
            if (access(device_path, F_OK) == 0) {
                LOG_INFO(LOG_SYSTEM, "Found potential device: %s", device_path);
                strncpy((char *)config->device_path, device_path, 255);
                
                SerialBus *bus = serial_bus_open(config);
                if (bus) {
                    LOG_INFO(LOG_SYSTEM, "Opened USB device %s (VID=%04x, PID=%04x)", 
                            device_path, vendor_id, product_id);
                    return bus;
                }
            }
        }
        
        LOG_ERROR(LOG_SYSTEM, "USB device %s not found", vid_pid_str);
        return NULL;
    }
    
    char device_info[256];
    if (fgets(device_info, sizeof(device_info), lsusb)) {
        pclose(lsusb);
        
        // Extract bus and device number from lsusb output
        // Format: Bus 001 Device 003: ID 2e8a:0180 ...
        int bus_num, dev_num;
        if (sscanf(device_info, "Bus %d Device %d:", &bus_num, &dev_num) == 2) {
            // Find the corresponding ttyACM device
            // Try common numbering (usually sequential)
            for (int i = 0; i < 10; i++) {
                char device_path[64];
                snprintf(device_path, sizeof(device_path), "/dev/ttyACM%d", i);
                
                if (access(device_path, F_OK) == 0) {
                    strncpy((char *)config->device_path, device_path, 255);
                    
                    SerialBus *bus = serial_bus_open(config);
                    if (bus) {
                        LOG_INFO(LOG_SYSTEM, "Opened USB device %s (VID=%04x, PID=%04x)", 
                                device_path, vendor_id, product_id);
                        return bus;
                    }
                }
            }
        }
    } else {
        pclose(lsusb);
    }
    
    LOG_ERROR(LOG_SYSTEM, "USB device %s not found", vid_pid_str);
    return NULL;
}
