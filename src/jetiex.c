#include "jetiex.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

/* Internal Structure */
struct JetiEX {
    JetiEXConfig config;
    int serial_fd;
    
    JetiEXSensor sensors[JETIEX_MAX_SENSORS];
    int sensor_count;
    
    JetiEXParameter parameters[JETIEX_MAX_PARAMS];
    int parameter_count;
    
    pthread_t thread;
    pthread_t rx_thread;
    pthread_mutex_t mutex;
    bool running;
    
    char text_message[JETIEX_MAX_TEXT_LENGTH + 1];
    bool text_pending;
    
    uint8_t rx_buffer[256];
    int rx_buffer_len;
};

/* CRC-16 CCITT calculation */
static uint16_t calculate_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    
    return crc;
}

/* Build telemetry packet */
static int build_telemetry_packet(JetiEX *jetiex, uint8_t *packet) {
    int pos = 0;
    
    // Header
    packet[pos++] = JETIEX_PACKET_DATA;  // Packet type
    packet[pos++] = jetiex->config.manufacturer_id & 0xFF;
    packet[pos++] = (jetiex->config.manufacturer_id >> 8) & 0xFF;
    packet[pos++] = jetiex->config.device_id & 0xFF;
    packet[pos++] = (jetiex->config.device_id >> 8) & 0xFF;
    
    // Reserved byte
    packet[pos++] = 0x00;
    
    // Data section start marker
    packet[pos++] = 0x3A;
    
    // Add sensor data
    for (int i = 0; i < jetiex->sensor_count; i++) {
        JetiEXSensor *sensor = &jetiex->sensors[i];
        if (!sensor->enabled) continue;
        
        // Encode sensor ID and data type
        uint8_t id_type = (sensor->id & 0x0F) | ((sensor->type & 0x0F) << 4);
        packet[pos++] = id_type;
        
        // Encode value based on type
        switch (sensor->type) {
            case JETIEX_TYPE_6b:
                packet[pos++] = sensor->value & 0x3F;
                break;
                
            case JETIEX_TYPE_14b:
                packet[pos++] = sensor->value & 0xFF;
                packet[pos++] = (sensor->value >> 8) & 0x3F;
                break;
                
            case JETIEX_TYPE_22b:
                packet[pos++] = sensor->value & 0xFF;
                packet[pos++] = (sensor->value >> 8) & 0xFF;
                packet[pos++] = (sensor->value >> 16) & 0x3F;
                break;
                
            case JETIEX_TYPE_30b:
                packet[pos++] = sensor->value & 0xFF;
                packet[pos++] = (sensor->value >> 8) & 0xFF;
                packet[pos++] = (sensor->value >> 16) & 0xFF;
                packet[pos++] = (sensor->value >> 24) & 0x3F;
                break;
                
            default:
                break;
        }
        
        // Check packet size limit
        if (pos >= JETIEX_MAX_PACKET_SIZE - 3) {
            break;
        }
    }
    
    // Calculate and add CRC
    uint16_t crc = calculate_crc16(packet, pos);
    packet[pos++] = crc & 0xFF;
    packet[pos++] = (crc >> 8) & 0xFF;
    
    return pos;
}

/* Build text message packet */
static int build_text_packet(JetiEX *jetiex, uint8_t *packet) {
    int pos = 0;
    
    // Header
    packet[pos++] = JETIEX_PACKET_MSG;
    packet[pos++] = jetiex->config.manufacturer_id & 0xFF;
    packet[pos++] = (jetiex->config.manufacturer_id >> 8) & 0xFF;
    packet[pos++] = jetiex->config.device_id & 0xFF;
    packet[pos++] = (jetiex->config.device_id >> 8) & 0xFF;
    
    // Text message
    size_t len = strlen(jetiex->text_message);
    if (len > JETIEX_MAX_TEXT_LENGTH) {
        len = JETIEX_MAX_TEXT_LENGTH;
    }
    
    packet[pos++] = len;
    memcpy(&packet[pos], jetiex->text_message, len);
    pos += len;
    
    // Calculate and add CRC
    uint16_t crc = calculate_crc16(packet, pos);
    packet[pos++] = crc & 0xFF;
    packet[pos++] = (crc >> 8) & 0xFF;
    
    return pos;
}

/* Build configuration response packet */
static int build_config_response(JetiEX *jetiex, uint8_t cmd, uint8_t param_id, uint8_t *packet) {
    int pos = 0;
    
    // Header
    packet[pos++] = JETIEX_PACKET_CONFIG;
    packet[pos++] = jetiex->config.manufacturer_id & 0xFF;
    packet[pos++] = (jetiex->config.manufacturer_id >> 8) & 0xFF;
    packet[pos++] = jetiex->config.device_id & 0xFF;
    packet[pos++] = (jetiex->config.device_id >> 8) & 0xFF;
    
    packet[pos++] = cmd;  // Command
    packet[pos++] = param_id;  // Parameter ID
    
    // Find parameter
    JetiEXParameter *param = NULL;
    for (int i = 0; i < jetiex->parameter_count; i++) {
        if (jetiex->parameters[i].id == param_id) {
            param = &jetiex->parameters[i];
            break;
        }
    }
    
    if (!param) {
        packet[pos++] = 0xFF;  // Error: parameter not found
    } else {
        packet[pos++] = 0x00;  // Status: OK
        packet[pos++] = param->type;  // Parameter type
        
        // Add parameter name
        uint8_t name_len = strlen(param->name);
        if (name_len > JETIEX_PARAM_NAME_LEN) name_len = JETIEX_PARAM_NAME_LEN;
        packet[pos++] = name_len;
        memcpy(&packet[pos], param->name, name_len);
        pos += name_len;
        
        // Add parameter value based on type
        switch (param->type) {
            case JETIEX_PARAM_UINT8:
            case JETIEX_PARAM_INT8:
            case JETIEX_PARAM_BOOL:
                packet[pos++] = *(uint8_t*)param->value_ptr;
                break;
                
            case JETIEX_PARAM_UINT16:
            case JETIEX_PARAM_INT16: {
                uint16_t val = *(uint16_t*)param->value_ptr;
                packet[pos++] = val & 0xFF;
                packet[pos++] = (val >> 8) & 0xFF;
                break;
            }
                
            case JETIEX_PARAM_UINT32:
            case JETIEX_PARAM_INT32:
            case JETIEX_PARAM_FLOAT: {
                uint32_t val = *(uint32_t*)param->value_ptr;
                packet[pos++] = val & 0xFF;
                packet[pos++] = (val >> 8) & 0xFF;
                packet[pos++] = (val >> 16) & 0xFF;
                packet[pos++] = (val >> 24) & 0xFF;
                break;
            }
                
            case JETIEX_PARAM_STRING: {
                const char *str = (const char*)param->value_ptr;
                uint8_t str_len = strlen(str);
                if (str_len > 32) str_len = 32;
                packet[pos++] = str_len;
                memcpy(&packet[pos], str, str_len);
                pos += str_len;
                break;
            }
        }
        
        // Add min/max values
        packet[pos++] = param->min_value & 0xFF;
        packet[pos++] = (param->min_value >> 8) & 0xFF;
        packet[pos++] = (param->min_value >> 16) & 0xFF;
        packet[pos++] = (param->min_value >> 24) & 0xFF;
        
        packet[pos++] = param->max_value & 0xFF;
        packet[pos++] = (param->max_value >> 8) & 0xFF;
        packet[pos++] = (param->max_value >> 16) & 0xFF;
        packet[pos++] = (param->max_value >> 24) & 0xFF;
        
        packet[pos++] = param->flags;
    }
    
    // Calculate and add CRC
    uint16_t crc = calculate_crc16(packet, pos);
    packet[pos++] = crc & 0xFF;
    packet[pos++] = (crc >> 8) & 0xFF;
    
    return pos;
}

/* Handle incoming configuration command */
static void handle_config_command(JetiEX *jetiex, const uint8_t *data, size_t len) {
    if (len < 8) {
        LOG_WARN(LOG_JETIEX, "Config packet too short: %zu bytes", len);
        return;
    }
    
    // Verify CRC
    uint16_t received_crc = data[len - 2] | (data[len - 1] << 8);
    uint16_t calculated_crc = calculate_crc16(data, len - 2);
    if (received_crc != calculated_crc) {
        LOG_WARN(LOG_JETIEX, "Config packet CRC mismatch");
        return;
    }
    
    uint8_t cmd = data[5];
    uint8_t param_id = data[6];
    
    LOG_DEBUG(LOG_JETIEX, "Received config command: cmd=0x%02X param_id=%d", cmd, param_id);
    
    pthread_mutex_lock(&jetiex->mutex);
    
    switch (cmd) {
        case JETIEX_CMD_READ: {
            // Build and send response
            uint8_t response[JETIEX_MAX_PACKET_SIZE];
            int response_len = build_config_response(jetiex, cmd, param_id, response);
            write(jetiex->serial_fd, response, response_len);
            LOG_DEBUG(LOG_JETIEX, "Sent parameter read response for ID %d", param_id);
            break;
        }
            
        case JETIEX_CMD_WRITE: {
            // Find parameter
            JetiEXParameter *param = NULL;
            for (int i = 0; i < jetiex->parameter_count; i++) {
                if (jetiex->parameters[i].id == param_id) {
                    param = &jetiex->parameters[i];
                    break;
                }
            }
            
            if (!param) {
                LOG_WARN(LOG_JETIEX, "Write to unknown parameter ID %d", param_id);
                break;
            }
            
            if (param->flags & JETIEX_PARAM_READONLY) {
                LOG_WARN(LOG_JETIEX, "Attempt to write read-only parameter ID %d", param_id);
                break;
            }
            
            // Extract and validate new value
            size_t value_offset = 7;
            int32_t new_value = 0;
            
            switch (param->type) {
                case JETIEX_PARAM_UINT8:
                case JETIEX_PARAM_INT8:
                case JETIEX_PARAM_BOOL:
                    new_value = data[value_offset];
                    break;
                    
                case JETIEX_PARAM_UINT16:
                case JETIEX_PARAM_INT16:
                    new_value = data[value_offset] | (data[value_offset + 1] << 8);
                    break;
                    
                case JETIEX_PARAM_UINT32:
                case JETIEX_PARAM_INT32:
                case JETIEX_PARAM_FLOAT:
                    new_value = data[value_offset] | (data[value_offset + 1] << 8) |
                               (data[value_offset + 2] << 16) | (data[value_offset + 3] << 24);
                    break;
            }
            
            // Validate range
            if (new_value < param->min_value || new_value > param->max_value) {
                LOG_WARN(LOG_JETIEX, "Parameter %d value %d out of range [%d, %d]",
                        param_id, new_value, param->min_value, param->max_value);
                break;
            }
            
            // Update value
            switch (param->type) {
                case JETIEX_PARAM_UINT8:
                case JETIEX_PARAM_INT8:
                case JETIEX_PARAM_BOOL:
                    *(uint8_t*)param->value_ptr = (uint8_t)new_value;
                    break;
                    
                case JETIEX_PARAM_UINT16:
                case JETIEX_PARAM_INT16:
                    *(uint16_t*)param->value_ptr = (uint16_t)new_value;
                    break;
                    
                case JETIEX_PARAM_UINT32:
                case JETIEX_PARAM_INT32:
                case JETIEX_PARAM_FLOAT:
                    *(uint32_t*)param->value_ptr = (uint32_t)new_value;
                    break;
            }
            
            LOG_INFO(LOG_JETIEX, "Parameter %d (%s) updated to %d", 
                    param_id, param->name, new_value);
            
            // Call callback if registered
            if (jetiex->config.config_changed_callback) {
                jetiex->config.config_changed_callback(param_id, jetiex->config.user_data);
            }
            
            break;
        }
            
        case JETIEX_CMD_LIST: {
            // Send list of all parameters
            for (int i = 0; i < jetiex->parameter_count; i++) {
                uint8_t response[JETIEX_MAX_PACKET_SIZE];
                int response_len = build_config_response(jetiex, JETIEX_CMD_READ, 
                                                         jetiex->parameters[i].id, response);
                write(jetiex->serial_fd, response, response_len);
                usleep(10000);  // Small delay between packets
            }
            LOG_INFO(LOG_JETIEX, "Sent parameter list (%d parameters)", jetiex->parameter_count);
            break;
        }
            
        case JETIEX_CMD_SAVE: {
            // Call save callback if registered
            if (jetiex->config.config_save_callback) {
                bool success = jetiex->config.config_save_callback(jetiex->config.user_data);
                LOG_INFO(LOG_JETIEX, "Configuration save %s", success ? "successful" : "failed");
                jetiex_send_text(jetiex, success ? "Config Saved" : "Save Failed");
            } else {
                LOG_WARN(LOG_JETIEX, "Save requested but no callback registered");
            }
            break;
        }
            
        default:
            LOG_WARN(LOG_JETIEX, "Unknown config command: 0x%02X", cmd);
            break;
    }
    
    pthread_mutex_unlock(&jetiex->mutex);
}

/* Receive thread for bidirectional communication */
static void *receive_thread(void *arg) {
    JetiEX *jetiex = (JetiEX *)arg;
    
    LOG_INIT(LOG_JETIEX, "Receive thread started");
    
    while (jetiex->running) {
        uint8_t byte;
        ssize_t n = read(jetiex->serial_fd, &byte, 1);
        
        if (n > 0) {
            pthread_mutex_lock(&jetiex->mutex);
            
            // Add to buffer
            if (jetiex->rx_buffer_len < (int)sizeof(jetiex->rx_buffer)) {
                jetiex->rx_buffer[jetiex->rx_buffer_len++] = byte;
                
                // Check if we have a complete packet
                if (jetiex->rx_buffer_len >= 8) {  // Minimum packet size
                    if (jetiex->rx_buffer[0] == JETIEX_PACKET_CONFIG) {
                        // Wait for complete packet based on length field
                        // For simplicity, process when we have reasonable data
                        if (jetiex->rx_buffer_len >= 12) {
                            handle_config_command(jetiex, jetiex->rx_buffer, jetiex->rx_buffer_len);
                            jetiex->rx_buffer_len = 0;  // Reset buffer
                        }
                    } else {
                        // Unknown packet type, reset
                        jetiex->rx_buffer_len = 0;
                    }
                }
            } else {
                // Buffer overflow, reset
                LOG_WARN(LOG_JETIEX, "RX buffer overflow, resetting");
                jetiex->rx_buffer_len = 0;
            }
            
            pthread_mutex_unlock(&jetiex->mutex);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR(LOG_JETIEX, "Serial read error: %s", strerror(errno));
            break;
        }
        
        usleep(1000);  // 1ms sleep
    }
    
    LOG_SHUTDOWN(LOG_JETIEX, "Receive thread");
    return NULL;
}

/* Telemetry transmission thread */
static void *telemetry_thread(void *arg) {
    JetiEX *jetiex = (JetiEX *)arg;
    uint8_t packet[JETIEX_MAX_PACKET_SIZE];
    
    LOG_INIT(LOG_JETIEX, "Telemetry transmission thread started");
    
    long long update_interval_ms = 1000 / jetiex->config.update_rate_hz;
    struct timeval last_update;
    gettimeofday(&last_update, NULL);
    
    while (jetiex->running) {
        struct timeval now;
        gettimeofday(&now, NULL);
        
        long long elapsed_ms = (now.tv_sec - last_update.tv_sec) * 1000 +
                              (now.tv_usec - last_update.tv_usec) / 1000;
        
        if (elapsed_ms >= update_interval_ms) {
            pthread_mutex_lock(&jetiex->mutex);
            
            int packet_len;
            
            // Send text message if pending
            if (jetiex->text_pending && jetiex->config.text_messages) {
                packet_len = build_text_packet(jetiex, packet);
                jetiex->text_pending = false;
                LOG_DEBUG(LOG_JETIEX, "Sending text message: %s", jetiex->text_message);
            } else {
                // Send telemetry data
                packet_len = build_telemetry_packet(jetiex, packet);
                LOG_DEBUG(LOG_JETIEX, "Sending telemetry packet (%d bytes, %d sensors)", 
                         packet_len, jetiex->sensor_count);
            }
            
            pthread_mutex_unlock(&jetiex->mutex);
            
            // Transmit packet
            ssize_t written = write(jetiex->serial_fd, packet, packet_len);
            if (written != packet_len) {
                LOG_WARN(LOG_JETIEX, "Partial write: %zd/%d bytes", written, packet_len);
            }
            
            last_update = now;
        }
        
        // Sleep for a short interval
        usleep(1000); // 1ms
    }
    
    LOG_SHUTDOWN(LOG_JETIEX, "Telemetry transmission");
    return NULL;
}

/* Public API Implementation */

JetiEX *jetiex_create(const JetiEXConfig *config) {
    if (!config || !config->serial_port) {
        LOG_ERROR(LOG_JETIEX, "Invalid configuration");
        return NULL;
    }
    
    JetiEX *jetiex = (JetiEX *)calloc(1, sizeof(JetiEX));
    if (!jetiex) {
        LOG_ERROR(LOG_JETIEX, "Memory allocation failed");
        return NULL;
    }
    
    memcpy(&jetiex->config, config, sizeof(JetiEXConfig));
    
    // Open serial port
    jetiex->serial_fd = open(config->serial_port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (jetiex->serial_fd < 0) {
        LOG_ERROR(LOG_JETIEX, "Cannot open serial port %s: %s", 
                 config->serial_port, strerror(errno));
        free(jetiex);
        return NULL;
    }
    
    // Configure serial port
    struct termios tty;
    if (tcgetattr(jetiex->serial_fd, &tty) != 0) {
        LOG_ERROR(LOG_JETIEX, "tcgetattr failed: %s", strerror(errno));
        close(jetiex->serial_fd);
        free(jetiex);
        return NULL;
    }
    
    // Set baud rate
    speed_t baud;
    switch (config->baud_rate) {
        case 9600:   baud = B9600; break;
        case 19200:  baud = B19200; break;
        case 38400:  baud = B38400; break;
        case 57600:  baud = B57600; break;
        case 115200: baud = B115200; break;
        case 230400: baud = B230400; break;
        default:
            LOG_ERROR(LOG_JETIEX, "Unsupported baud rate: %u", config->baud_rate);
            close(jetiex->serial_fd);
            free(jetiex);
            return NULL;
    }
    
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);
    
    // 8N1 mode
    tty.c_cflag &= ~PARENB;        // No parity
    tty.c_cflag &= ~CSTOPB;        // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;            // 8 data bits
    tty.c_cflag &= ~CRTSCTS;       // No hardware flow control
    tty.c_cflag |= CREAD | CLOCAL; // Enable receiver, ignore modem control
    
    // Raw mode
    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ECHOE;
    tty.c_lflag &= ~ECHONL;
    tty.c_lflag &= ~ISIG;
    
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    
    // Non-blocking read
    tty.c_cc[VTIME] = 0;
    tty.c_cc[VMIN] = 0;
    
    if (tcsetattr(jetiex->serial_fd, TCSANOW, &tty) != 0) {
        LOG_ERROR(LOG_JETIEX, "tcsetattr failed: %s", strerror(errno));
        close(jetiex->serial_fd);
        free(jetiex);
        return NULL;
    }
    
    // Initialize mutex
    pthread_mutex_init(&jetiex->mutex, NULL);
    
    LOG_INFO(LOG_JETIEX, "Initialized on %s at %u baud (Mfr:0x%04X Dev:0x%04X)",
             config->serial_port, config->baud_rate,
             config->manufacturer_id, config->device_id);
    
    return jetiex;
}

void jetiex_destroy(JetiEX *jetiex) {
    if (!jetiex) return;
    
    if (jetiex->running) {
        jetiex_stop(jetiex);
    }
    
    if (jetiex->serial_fd >= 0) {
        close(jetiex->serial_fd);
    }
    
    pthread_mutex_destroy(&jetiex->mutex);
    
    LOG_SHUTDOWN(LOG_JETIEX, "JetiEX telemetry");
    free(jetiex);
}

bool jetiex_add_sensor(JetiEX *jetiex, const JetiEXSensor *sensor) {
    if (!jetiex || !sensor) {
        LOG_ERROR(LOG_JETIEX, "Invalid parameters");
        return false;
    }
    
    pthread_mutex_lock(&jetiex->mutex);
    
    if (jetiex->sensor_count >= JETIEX_MAX_SENSORS) {
        LOG_ERROR(LOG_JETIEX, "Maximum sensors reached (%d)", JETIEX_MAX_SENSORS);
        pthread_mutex_unlock(&jetiex->mutex);
        return false;
    }
    
    // Check for duplicate ID
    for (int i = 0; i < jetiex->sensor_count; i++) {
        if (jetiex->sensors[i].id == sensor->id) {
            LOG_WARN(LOG_JETIEX, "Sensor ID %d already exists, replacing", sensor->id);
            memcpy(&jetiex->sensors[i], sensor, sizeof(JetiEXSensor));
            pthread_mutex_unlock(&jetiex->mutex);
            return true;
        }
    }
    
    memcpy(&jetiex->sensors[jetiex->sensor_count], sensor, sizeof(JetiEXSensor));
    jetiex->sensor_count++;
    
    LOG_INFO(LOG_JETIEX, "Added sensor #%d: %s (%s)", 
             sensor->id, sensor->label, sensor->unit_label);
    
    pthread_mutex_unlock(&jetiex->mutex);
    return true;
}

bool jetiex_update_sensor(JetiEX *jetiex, uint8_t sensor_id, int32_t value) {
    if (!jetiex) {
        return false;
    }
    
    pthread_mutex_lock(&jetiex->mutex);
    
    for (int i = 0; i < jetiex->sensor_count; i++) {
        if (jetiex->sensors[i].id == sensor_id) {
            jetiex->sensors[i].value = value;
            LOG_DEBUG(LOG_JETIEX, "Sensor #%d updated: %d %s", 
                     sensor_id, value, jetiex->sensors[i].unit_label);
            pthread_mutex_unlock(&jetiex->mutex);
            return true;
        }
    }
    
    pthread_mutex_unlock(&jetiex->mutex);
    LOG_WARN(LOG_JETIEX, "Sensor ID %d not found", sensor_id);
    return false;
}

bool jetiex_send_text(JetiEX *jetiex, const char *message) {
    if (!jetiex || !message) {
        return false;
    }
    
    pthread_mutex_lock(&jetiex->mutex);
    
    strncpy(jetiex->text_message, message, JETIEX_MAX_TEXT_LENGTH);
    jetiex->text_message[JETIEX_MAX_TEXT_LENGTH] = '\0';
    jetiex->text_pending = true;
    
    pthread_mutex_unlock(&jetiex->mutex);
    
    LOG_INFO(LOG_JETIEX, "Text message queued: %s", message);
    return true;
}

int jetiex_get_sensor_count(const JetiEX *jetiex) {
    return jetiex ? jetiex->sensor_count : 0;
}

bool jetiex_enable_sensor(JetiEX *jetiex, uint8_t sensor_id, bool enabled) {
    if (!jetiex) {
        return false;
    }
    
    pthread_mutex_lock(&jetiex->mutex);
    
    for (int i = 0; i < jetiex->sensor_count; i++) {
        if (jetiex->sensors[i].id == sensor_id) {
            jetiex->sensors[i].enabled = enabled;
            LOG_INFO(LOG_JETIEX, "Sensor #%d %s", sensor_id, enabled ? "enabled" : "disabled");
            pthread_mutex_unlock(&jetiex->mutex);
            return true;
        }
    }
    
    pthread_mutex_unlock(&jetiex->mutex);
    return false;
}

bool jetiex_start(JetiEX *jetiex) {
    if (!jetiex) {
        return false;
    }
    
    if (jetiex->running) {
        LOG_WARN(LOG_JETIEX, "Already running");
        return true;
    }
    
    jetiex->running = true;
    
    // Start telemetry transmission thread
    if (pthread_create(&jetiex->thread, NULL, telemetry_thread, jetiex) != 0) {
        LOG_ERROR(LOG_JETIEX, "Failed to create telemetry thread");
        jetiex->running = false;
        return false;
    }
    
    // Start receive thread if remote configuration is enabled
    if (jetiex->config.remote_config) {
        if (pthread_create(&jetiex->rx_thread, NULL, receive_thread, jetiex) != 0) {
            LOG_ERROR(LOG_JETIEX, "Failed to create receive thread");
            jetiex->running = false;
            pthread_join(jetiex->thread, NULL);
            return false;
        }
        LOG_INFO(LOG_JETIEX, "Remote configuration enabled");
    }
    
    LOG_INFO(LOG_JETIEX, "Telemetry started at %d Hz", jetiex->config.update_rate_hz);
    return true;
}

void jetiex_stop(JetiEX *jetiex) {
    if (!jetiex || !jetiex->running) {
        return;
    }
    
    LOG_INFO(LOG_JETIEX, "Stopping telemetry...");
    jetiex->running = false;
    pthread_join(jetiex->thread, NULL);
    
    // Stop receive thread if it was started
    if (jetiex->config.remote_config) {
        pthread_join(jetiex->rx_thread, NULL);
    }
}

bool jetiex_is_running(const JetiEX *jetiex) {
    return jetiex && jetiex->running;
}

/* Helper functions */

JetiEXSensor jetiex_sensor_rpm(uint8_t id, const char *label) {
    JetiEXSensor sensor = {
        .id = id,
        .type = JETIEX_TYPE_22b,
        .unit = JETIEX_UNIT_RPM,
        .precision = 0,
        .value = 0,
        .enabled = true
    };
    strncpy(sensor.label, label, sizeof(sensor.label) - 1);
    sensor.label[sizeof(sensor.label) - 1] = '\0';
    strncpy(sensor.unit_label, "rpm", sizeof(sensor.unit_label) - 1);
    sensor.unit_label[sizeof(sensor.unit_label) - 1] = '\0';
    return sensor;
}

JetiEXSensor jetiex_sensor_voltage(uint8_t id, const char *label, uint8_t precision) {
    JetiEXSensor sensor = {
        .id = id,
        .type = JETIEX_TYPE_14b,
        .unit = JETIEX_UNIT_VOLTS,
        .precision = precision > 2 ? 2 : precision,
        .value = 0,
        .enabled = true
    };
    strncpy(sensor.label, label, sizeof(sensor.label) - 1);
    sensor.label[sizeof(sensor.label) - 1] = '\0';
    strncpy(sensor.unit_label, "V", sizeof(sensor.unit_label) - 1);
    sensor.unit_label[sizeof(sensor.unit_label) - 1] = '\0';
    return sensor;
}

JetiEXSensor jetiex_sensor_current(uint8_t id, const char *label, uint8_t precision) {
    JetiEXSensor sensor = {
        .id = id,
        .type = JETIEX_TYPE_14b,
        .unit = JETIEX_UNIT_AMPS,
        .precision = precision > 2 ? 2 : precision,
        .value = 0,
        .enabled = true
    };
    strncpy(sensor.label, label, sizeof(sensor.label) - 1);
    sensor.label[sizeof(sensor.label) - 1] = '\0';
    strncpy(sensor.unit_label, "A", sizeof(sensor.unit_label) - 1);
    sensor.unit_label[sizeof(sensor.unit_label) - 1] = '\0';
    return sensor;
}

JetiEXSensor jetiex_sensor_temperature(uint8_t id, const char *label, uint8_t precision) {
    JetiEXSensor sensor = {
        .id = id,
        .type = JETIEX_TYPE_14b,
        .unit = JETIEX_UNIT_CELSIUS,
        .precision = precision > 1 ? 1 : precision,
        .value = 0,
        .enabled = true
    };
    strncpy(sensor.label, label, sizeof(sensor.label) - 1);
    sensor.label[sizeof(sensor.label) - 1] = '\0';
    strncpy(sensor.unit_label, "°C", sizeof(sensor.unit_label) - 1);
    sensor.unit_label[sizeof(sensor.unit_label) - 1] = '\0';
    return sensor;
}

JetiEXSensor jetiex_sensor_percentage(uint8_t id, const char *label) {
    JetiEXSensor sensor = {
        .id = id,
        .type = JETIEX_TYPE_14b,
        .unit = JETIEX_UNIT_PERCENT,
        .precision = 0,
        .value = 0,
        .enabled = true
    };
    strncpy(sensor.label, label, sizeof(sensor.label) - 1);
    sensor.label[sizeof(sensor.label) - 1] = '\0';
    strncpy(sensor.unit_label, "%", sizeof(sensor.unit_label) - 1);
    sensor.unit_label[sizeof(sensor.unit_label) - 1] = '\0';
    return sensor;
}

JetiEXSensor jetiex_sensor_index(uint8_t id, const char *label) {
    JetiEXSensor sensor = {
        .id = id,
        .type = JETIEX_TYPE_6b,
        .unit = JETIEX_UNIT_NONE,
        .precision = 0,
        .value = 0,
        .enabled = true
    };
    strncpy(sensor.label, label, sizeof(sensor.label) - 1);
    sensor.label[sizeof(sensor.label) - 1] = '\0';
    sensor.unit_label[0] = '\0';
    return sensor;
}

/* Parameter Management Functions */

bool jetiex_add_parameter(JetiEX *jetiex, const JetiEXParameter *param) {
    if (!jetiex || !param) {
        LOG_ERROR(LOG_JETIEX, "Invalid parameters");
        return false;
    }
    
    pthread_mutex_lock(&jetiex->mutex);
    
    if (jetiex->parameter_count >= JETIEX_MAX_PARAMS) {
        LOG_ERROR(LOG_JETIEX, "Maximum parameters reached (%d)", JETIEX_MAX_PARAMS);
        pthread_mutex_unlock(&jetiex->mutex);
        return false;
    }
    
    // Check for duplicate ID
    for (int i = 0; i < jetiex->parameter_count; i++) {
        if (jetiex->parameters[i].id == param->id) {
            LOG_WARN(LOG_JETIEX, "Parameter ID %d already exists, replacing", param->id);
            memcpy(&jetiex->parameters[i], param, sizeof(JetiEXParameter));
            pthread_mutex_unlock(&jetiex->mutex);
            return true;
        }
    }
    
    memcpy(&jetiex->parameters[jetiex->parameter_count], param, sizeof(JetiEXParameter));
    jetiex->parameter_count++;
    
    LOG_INFO(LOG_JETIEX, "Added parameter #%d: %s (type %d)", 
             param->id, param->name, param->type);
    
    pthread_mutex_unlock(&jetiex->mutex);
    return true;
}

bool jetiex_remove_parameter(JetiEX *jetiex, uint8_t param_id) {
    if (!jetiex) {
        return false;
    }
    
    pthread_mutex_lock(&jetiex->mutex);
    
    for (int i = 0; i < jetiex->parameter_count; i++) {
        if (jetiex->parameters[i].id == param_id) {
            // Shift remaining parameters down
            for (int j = i; j < jetiex->parameter_count - 1; j++) {
                memcpy(&jetiex->parameters[j], &jetiex->parameters[j + 1], sizeof(JetiEXParameter));
            }
            jetiex->parameter_count--;
            LOG_INFO(LOG_JETIEX, "Removed parameter #%d", param_id);
            pthread_mutex_unlock(&jetiex->mutex);
            return true;
        }
    }
    
    pthread_mutex_unlock(&jetiex->mutex);
    return false;
}

int jetiex_get_parameter_count(const JetiEX *jetiex) {
    return jetiex ? jetiex->parameter_count : 0;
}

const JetiEXParameter *jetiex_get_parameter(const JetiEX *jetiex, uint8_t param_id) {
    if (!jetiex) {
        return NULL;
    }
    
    for (int i = 0; i < jetiex->parameter_count; i++) {
        if (jetiex->parameters[i].id == param_id) {
            return &jetiex->parameters[i];
        }
    }
    
    return NULL;
}

bool jetiex_update_parameter(JetiEX *jetiex, uint8_t param_id, const void *value) {
    if (!jetiex || !value) {
        return false;
    }
    
    pthread_mutex_lock(&jetiex->mutex);
    
    for (int i = 0; i < jetiex->parameter_count; i++) {
        if (jetiex->parameters[i].id == param_id) {
            JetiEXParameter *param = &jetiex->parameters[i];
            
            // Copy value based on type
            switch (param->type) {
                case JETIEX_PARAM_UINT8:
                case JETIEX_PARAM_INT8:
                case JETIEX_PARAM_BOOL:
                    *(uint8_t*)param->value_ptr = *(const uint8_t*)value;
                    break;
                    
                case JETIEX_PARAM_UINT16:
                case JETIEX_PARAM_INT16:
                    *(uint16_t*)param->value_ptr = *(const uint16_t*)value;
                    break;
                    
                case JETIEX_PARAM_UINT32:
                case JETIEX_PARAM_INT32:
                case JETIEX_PARAM_FLOAT:
                    *(uint32_t*)param->value_ptr = *(const uint32_t*)value;
                    break;
                    
                case JETIEX_PARAM_STRING:
                    strncpy((char*)param->value_ptr, (const char*)value, 32);
                    ((char*)param->value_ptr)[31] = '\0';
                    break;
            }
            
            pthread_mutex_unlock(&jetiex->mutex);
            return true;
        }
    }
    
    pthread_mutex_unlock(&jetiex->mutex);
    return false;
}
