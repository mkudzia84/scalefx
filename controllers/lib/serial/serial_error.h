/*
 * Serial Error - Generic Error Codes and Command Results
 *
 * Error codes and CommandResult struct for ACK/NACK handling.
 *
 * Error Code Ranges:
 *   0x00-0x0F  General/common errors (OK, UNKNOWN, INVALID_COMMAND, etc.)
 *   0x10-0x1F  Parameter validation errors (INVALID_PARAM, OUT_OF_RANGE, etc.)
 *   0x20-0x4F  GunFX-specific errors (SERVO_*, SMOKE_*, TRIGGER_*)
 *   0x50-0x5F  LightFX-specific errors (LED_*, SERVO_*)
 *   0x60-0x6F  GearControl-specific errors (GEAR_*, MOTOR_*, SERVO_*, YAW_*)
 *   0x70-0x8F  Reserved for future modules
 *   0xF0-0xFF  System/transport errors (TIMEOUT, CRC_ERROR, etc.)
 *
 * CommandResult:
 *   Encapsulates command outcome for blocking operations:
 *   - Ack()        - Command succeeded
 *   - Nack(code)   - Command rejected with error code
 *   - Timeout()    - No response within timeout
 *   - SendFailed() - Failed to send command
 *
 * Usage:
 *   CommandResult result = master.triggerOn(600);
 *   if (result.isAck()) { ... }
 *   else if (result.isNack()) { handleError(result.errorCode()); }
 */

#ifndef SERIAL_ERROR_H
#define SERIAL_ERROR_H

#include <cstdint>
#include <cstring>

// ============================================================================
// Generic Error Codes
// ============================================================================

/**
 * @brief Generic serial error codes for ACK/NACK responses
 * 
 * These codes are protocol-agnostic and can be used by any module.
 * Domain-specific modules (GunFX, EngineFX) can define additional
 * error codes in the 0x20-0x7F range.
 */
namespace SerialError {
    // General errors (0x00-0x0F)
    constexpr uint8_t OK                    = 0x00;  // No error (ACK)
    constexpr uint8_t UNKNOWN               = 0x01;  // Unknown error
    constexpr uint8_t NOT_INITIALIZED       = 0x02;  // Module not initialized
    constexpr uint8_t INVALID_COMMAND       = 0x03;  // Unknown command
    constexpr uint8_t MISSING_PARAMETER     = 0x04;  // Required parameter missing
    constexpr uint8_t BUSY                  = 0x05;  // Module busy, try again
    constexpr uint8_t NOT_SUPPORTED         = 0x06;  // Command not supported
    constexpr uint8_t PERMISSION_DENIED     = 0x07;  // Operation not permitted
    
    // Parameter validation errors (0x10-0x1F)
    constexpr uint8_t INVALID_PARAM         = 0x10;  // Generic invalid parameter
    constexpr uint8_t PARAM_OUT_OF_RANGE    = 0x11;  // Parameter value out of range
    constexpr uint8_t INVALID_ID            = 0x12;  // Invalid device/channel ID
    constexpr uint8_t INVALID_VALUE         = 0x13;  // Invalid parameter value
    constexpr uint8_t PARAM_TOO_LONG        = 0x14;  // Parameter string too long
    
    // Domain-specific errors: 0x20-0x7F (defined in respective type headers)
    // - GunFX: 0x20-0x4F (servo, smoke, trigger)
    // - LightFX: 0x50-0x5F (LED, servo)
    // - GearControl: 0x60-0x6F (gear, motor, servo, yaw)
    // - Reserved: 0x70-0x7F
    
    // System/transport errors (0xF0-0xFF)
    constexpr uint8_t INTERNAL_ERROR        = 0xF0;  // Internal error
    constexpr uint8_t TIMEOUT               = 0xF1;  // Operation timed out
    constexpr uint8_t COMM_ERROR            = 0xF2;  // Communication error
    constexpr uint8_t BUFFER_OVERFLOW       = 0xF3;  // Buffer overflow
    constexpr uint8_t CRC_ERROR             = 0xF4;  // CRC check failed
    constexpr uint8_t FRAMING_ERROR         = 0xF5;  // Framing/decode error
    
    /**
     * @brief Get human-readable error message for generic errors
     * @param code Error code
     * @return Error message string
     * 
     * Note: Domain-specific errors (0x20-0x7F) return "Domain-specific error".
     * Use the domain's getMessage() function for those codes.
     */
    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case OK:                    return "OK";
            case UNKNOWN:               return "Unknown error";
            case NOT_INITIALIZED:       return "Not initialized";
            case INVALID_COMMAND:       return "Invalid command";
            case MISSING_PARAMETER:     return "Missing parameter";
            case BUSY:                  return "Busy";
            case NOT_SUPPORTED:         return "Not supported";
            case PERMISSION_DENIED:     return "Permission denied";
            case INVALID_PARAM:         return "Invalid parameter";
            case PARAM_OUT_OF_RANGE:    return "Parameter out of range";
            case INVALID_ID:            return "Invalid ID";
            case INVALID_VALUE:         return "Invalid value";
            case PARAM_TOO_LONG:        return "Parameter too long";
            case INTERNAL_ERROR:        return "Internal error";
            case TIMEOUT:               return "Timeout";
            case COMM_ERROR:            return "Communication error";
            case BUFFER_OVERFLOW:       return "Buffer overflow";
            case CRC_ERROR:             return "CRC error";
            case FRAMING_ERROR:         return "Framing error";
            default:
                // Check if it's in the domain-specific range
                if (code >= 0x20 && code <= 0x7F) {
                    return "Domain-specific error";
                }
                return "Unknown error code";
        }
    }
    
    /**
     * @brief Check if error code is in generic range (handled by SerialError)
     */
    inline bool isGenericError(uint8_t code) {
        return (code <= 0x1F) || (code >= 0xF0);
    }
    
    /**
     * @brief Check if error code is in domain-specific range
     */
    inline bool isDomainError(uint8_t code) {
        return (code >= 0x20 && code <= 0x7F);
    }
}

// ============================================================================
// Command Result
// ============================================================================

/**
 * @brief Result of a command sent to slave
 * 
 * Encapsulates the result of a blocking command, including:
 * - Success/failure status
 * - Error code (0 = OK)
 * - Human-readable error message
 */
struct CommandResult {
    bool success = false;           // True if ACK received
    uint8_t errorCode = 0;          // Error code (0 = OK)
    char errorMessage[64] = "";     // Error message from NACK
    
    // Convenience constructors
    CommandResult() = default;
    CommandResult(bool ok) : success(ok), errorCode(ok ? SerialError::OK : SerialError::UNKNOWN) {}
    CommandResult(uint8_t code) : success(code == SerialError::OK), errorCode(code) {
        if (code != SerialError::OK) {
            strncpy(errorMessage, SerialError::getMessage(code), sizeof(errorMessage) - 1);
        }
    }
    CommandResult(uint8_t code, const char* msg) : success(code == SerialError::OK), errorCode(code) {
        if (msg) strncpy(errorMessage, msg, sizeof(errorMessage) - 1);
    }
    
    // Convenience accessors
    operator bool() const { return success; }
    bool isTimeout() const { return errorCode == SerialError::TIMEOUT; }
    bool isNack() const { return !success && errorCode != SerialError::TIMEOUT; }
    const char* message() const { 
        return errorMessage[0] ? errorMessage : SerialError::getMessage(errorCode); 
    }
    
    // Static factory methods
    static CommandResult Ack() { return CommandResult(SerialError::OK); }
    static CommandResult Nack(uint8_t code, const char* msg = nullptr) { 
        return CommandResult(code, msg ? msg : SerialError::getMessage(code)); 
    }
    static CommandResult Timeout() { return CommandResult(SerialError::TIMEOUT); }
    static CommandResult SendFailed() { return CommandResult(SerialError::COMM_ERROR, "Send failed"); }
    static CommandResult NotConnected() { return CommandResult(SerialError::NOT_INITIALIZED, "Not connected"); }
};

#endif // SERIAL_ERROR_H
