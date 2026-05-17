/*
 * Config Client — Client-Side Config Serial Communication
 *
 * Used by any controller or app that sends config management commands
 * to a HubFX-class hub over USB. Extends BusClient with config-specific
 * command methods and CONFIG_STATUS_RESP parsing.
 *
 * Shared library component (controllers/lib/sfx_config/).
 * Depends on: sfx_serial (BusClient, HubFxPacket, HubFxError).
 */

#ifndef CONFIG_CLIENT_H
#define CONFIG_CLIENT_H

#include <protocol/config_protocol.h>
#include <serial/client/bus_client.h>

// ============================================================================
// ConfigClient — Client for Config Commands
// ============================================================================

class ConfigClient : public BusClient {
public:
    // ========================================================================
    // Commands
    // ========================================================================

    /**
     * @brief Request server to reload config from file
     *
     * Instant response category: ACK / NACK CONFIG_ERROR.
     * Optionally specify a file path; if omitted, server uses default.
     *
     * @param path File path (nullptr = server default), max 127 chars
     * @return CommandResult with ACK/NACK
     */
    CommandResult configReload(const char* path = nullptr);

    /**
     * @brief Request config status from server
     *
     * Query response category: response arrives as CONFIG_STATUS_RESP.
     * Tag is resolved in onModulePacket().
     *
     * @return CommandResult (Ack when CONFIG_STATUS_RESP received)
     */
    CommandResult configStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================

    /** @brief Register callback for config status responses */
    void onConfigInfo(HubFxConfigInfoCallback cb) { _configCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================

    /** @brief Get last received config info */
    const HubFxConfigInfo& lastConfigInfo() const { return _lastInfo; }

protected:
    void onModulePacket(uint8_t type, uint8_t tag,
                        const uint8_t* payload, size_t len) override;

    const char* getModuleErrorMessage(uint8_t code) override {
        return ConfigError::getMessage(code);
    }

private:
    HubFxConfigInfo _lastInfo;
    HubFxConfigInfoCallback _configCallback;
};

#endif // CONFIG_CLIENT_H
