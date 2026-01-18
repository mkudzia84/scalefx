#ifndef SERIAL_BUS_H
#define SERIAL_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declaration
typedef struct SerialBus SerialBus;

// Serial bus configuration
typedef struct {
    const char *device_path;  // e.g., "/dev/ttyACM0" or "/dev/ttyUSB0"
    int baud_rate;            // e.g., 115200, 9600
    int timeout_ms;           // Read timeout in milliseconds
} SerialBusConfig;

/**
 * Open a serial bus connection
 * @param config Serial bus configuration
 * @return SerialBus handle, or NULL on error
 */
SerialBus* serial_bus_open(const SerialBusConfig *config);

/**
 * Close serial bus connection
 * @param bus SerialBus handle
 */
void serial_bus_close(SerialBus *bus);

/**
 * Write data to serial bus
 * @param bus SerialBus handle
 * @param data Data buffer to write
 * @param len Length of data in bytes
 * @return Number of bytes written, or -1 on error
 */
int serial_bus_write(SerialBus *bus, const void *data, size_t len);

/**
 * Write a null-terminated string to serial bus
 * @param bus SerialBus handle
 * @param str String to write (without newline)
 * @return Number of bytes written, or -1 on error
 */
int serial_bus_write_string(SerialBus *bus, const char *str);

/**
 * Write a formatted command to serial bus (adds newline)
 * @param bus SerialBus handle
 * @param format Printf-style format string
 * @param ... Format arguments
 * @return Number of bytes written, or -1 on error
 */
int serial_bus_write_command(SerialBus *bus, const char *format, ...);

/**
 * Read data from serial bus
 * @param bus SerialBus handle
 * @param buffer Buffer to read into
 * @param max_len Maximum number of bytes to read
 * @return Number of bytes read, 0 on timeout, -1 on error
 */
int serial_bus_read(SerialBus *bus, void *buffer, size_t max_len);

/**
 * Read a line from serial bus (until newline or buffer full)
 * @param bus SerialBus handle
 * @param buffer Buffer to read into
 * @param max_len Maximum number of bytes to read (including null terminator)
 * @return Number of bytes read (excluding null terminator), 0 on timeout, -1 on error
 */
int serial_bus_read_line(SerialBus *bus, char *buffer, size_t max_len);

/**
 * Check if serial bus is open and ready
 * @param bus SerialBus handle
 * @return true if ready, false otherwise
 */
bool serial_bus_is_ready(SerialBus *bus);

/**
 * Flush any pending data in serial buffers
 * @param bus SerialBus handle
 * @return 0 on success, -1 on error
 */
int serial_bus_flush(SerialBus *bus);

// ---------------------- Binary framing helpers (COBS + CRC8) ----------------------
// CRC-8 polynomial 0x07 over the provided buffer
uint8_t serial_bus_crc8_poly_07(const uint8_t *data, size_t len);

// COBS encode/decode; return encoded/decoded length, or 0 on error
size_t serial_bus_cobs_encode(const uint8_t *input, size_t length, uint8_t *output, size_t output_cap);
size_t serial_bus_cobs_decode(const uint8_t *input, size_t length, uint8_t *output, size_t output_cap);

// Send packet framed as: [type:u8][len:u8][payload...][crc] then COBS-encoded and terminated with 0x00
// payload_len must fit in uint8_t; returns 0 on success, -1 on failure
int serial_bus_send_packet(SerialBus *bus, uint8_t type, const uint8_t *payload, size_t payload_len);

// ---------------------- USB Device Detection ----------------------

/**
 * Find and open a USB serial device by Vendor ID and Product ID
 * @param vendor_id USB Vendor ID (e.g., 0x2e8a for Raspberry Pi Foundation)
 * @param product_id USB Product ID (e.g., 0x0180 for gunfx_pico)
 * @param config Serial bus configuration (device_path will be updated with found device)
 * @return SerialBus handle on success, NULL on error or device not found
 */
SerialBus* serial_bus_open_by_vid_pid(uint16_t vendor_id, uint16_t product_id, SerialBusConfig *config);

#endif // SERIAL_BUS_H
