/*
 * Slave Manager — USB Slave Controller Lifecycle Manager
 *
 * Owns the full slave lifecycle: USB Host initialization, device discovery,
 * controller identification, client binding, polling, and reconnection.
 *
 * DESIGN:
 *   - Data-driven registration via SlaveDescriptor table
 *   - Adding a new slave type requires only: enum value + addSlave() call
 *   - All discovery/identification uses the descriptor table (no hardcoded
 *     switch statements for specific controller types)
 *   - Template parameter MaxSlaves aligns with UsbRegistryT<MaxSlaves>
 *
 * USAGE:
 *   // Declare typed clients (file scope)
 *   GunFxClient gunfxClient;
 *   LightFxClient lightfxClient;
 *
 *   // In setup():
 *   SlaveManager& mgr = SlaveManager::instance();
 *   mgr.addSlave({ SlaveType::GunFX,   "GunFX",   "GunFX",  &gunfxClient,   true });
 *   mgr.addSlave({ SlaveType::LightFX, "LightFX", "LightFX", &lightfxClient, true });
 *   mgr.begin();
 *
 *   // In loop():
 *   mgr.process();   // handles polling + periodic discovery
 *
 *   // Slaves are identified via IDENTIFY (non-destructive). When the
 *   // descriptor has autoInit=true the manager immediately follows up with
 *   // INIT(mode=SLAVE) and marks the entry ready on success — the registry's
 *   // onReady callback then fires for downstream config-push hooks. Set
 *   // autoInit=false to defer activation to an explicit SLAVE_INIT command.
 */

#ifndef SLAVE_MANAGER_H
#define SLAVE_MANAGER_H

#include <Arduino.h>
#include <atomic>
#include <serial/client/bus_client.h>
#include <usb/sfx_usb_host.h>
#include <usb/usb_registry.h>
#include <platform/sfx_platform.h>
#include <platform/diag_log.h>

// ============================================================================
// SlaveDescriptor — Data-driven slave type registration
// ============================================================================

/**
 * @brief Describes a slave controller type for data-driven registration
 *
 * Each descriptor binds a SlaveType enum to its identification prefix,
 * log label, and client instance. The manager uses this table for
 * discovery (namePrefix matching) and lifecycle management.
 */
struct SlaveDescriptor {
    SlaveType   type;           ///< Enum identifying this slave type
    const char* namePrefix;     ///< INIT_READY name prefix for identification (e.g., "GunFX")
    const char* logPrefix;      ///< Short label for DiagLog relay (e.g., "GearCtrl")
    BusClient*  client;         ///< Typed client instance (externally owned, NOT nullptr)
    bool        autoInit = false;  ///< Auto-send INIT(SLAVE) after IDENTIFY succeeds
};

// ============================================================================
// Utility — Shared by SlaveManager and SlaveServer
// ============================================================================

/**
 * @brief Poll a BusClient until isServerReady() or timeout.
 *
 * Used by both SlaveManager (during IDENTIFY discovery) and SlaveServer
 * (for SLAVE_INIT activation command). Extracted as a free function to
 * avoid duplication.
 *
 * Works with both IDENTIFY and INIT responses — both set isServerReady().
 * Note: BusClient::sendInit() resets _serverReady, so this function
 * correctly waits for a new INIT_READY after an explicit INIT.
 *
 * @param client     BusClient to poll
 * @param timeout_ms Maximum wait time (default 3000ms)
 * @return true if server is ready within timeout
 */
inline bool awaitSlaveReady(BusClient& client, uint32_t timeout_ms = 3000) {
    unsigned long start = millis();
    while (!client.isServerReady() && (millis() - start < timeout_ms)) {
        client.process();
        SFX_DELAY_MS(1);
    }
    return client.isServerReady();
}

// ============================================================================
// SlaveManagerT — Templated Slave Lifecycle Manager
// ============================================================================

/**
 * @brief Singleton managing USB slave controller lifecycle
 *
 * Template parameter MaxSlaves aligns with UsbRegistryT<MaxSlaves> to
 * ensure consistent capacity across the registry and manager.
 *
 * @tparam MaxSlaves Maximum number of slave types (default: 4)
 */
template <size_t MaxSlaves = 4>
class SlaveManagerT {
public:
    static SlaveManagerT& instance() {
        static SlaveManagerT inst;
        return inst;
    }

    // Delete copy/move (Rule 14)
    SlaveManagerT(const SlaveManagerT&) = delete;
    SlaveManagerT& operator=(const SlaveManagerT&) = delete;
    SlaveManagerT(SlaveManagerT&&) = delete;
    SlaveManagerT& operator=(SlaveManagerT&&) = delete;

    // ========================================================================
    // Registration (call before begin)
    // ========================================================================

    /**
     * @brief Register a slave type with its descriptor
     *
     * Call once per slave type during setup(), before begin().
     * The descriptor's client pointer must remain valid for the lifetime
     * of the manager (typically file-scope objects).
     *
     * @return true if registered, false if table is full
     */
    bool addSlave(const SlaveDescriptor& desc);

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize USB Host, pre-register slaves with UsbRegistry,
     *        and set up DiagLog relays.
     *
     * Must be called after all addSlave() calls are complete.
     * @return true if USB Host initialized successfully
     */
    bool begin();

    /**
     * @brief Periodic processing — call from loop()
     *
     * Rate-limited internally. Handles:
     *   - Slave client polling (every POLL_INTERVAL_ms)
     *   - Periodic discovery scan (every DISCOVERY_INTERVAL_ms)
     */
    void process();

    // ========================================================================
    // Actions
    // ========================================================================

    /**
     * @brief Trigger immediate slave scan and identification via IDENTIFY
     *
     * Scans all USB CDC devices, probes unidentified ones via IDENTIFY,
     * identifies by name prefix, and binds the appropriate typed client.
     * Marks the entry connected; if the descriptor has autoInit=true the
     * manager also sends INIT(SLAVE) and marks the entry ready on success
     * (the UsbRegistry onReady callback then fires). Otherwise activation
     * is deferred to an explicit SLAVE_INIT command.
     * Called from onInit callback and during periodic discovery.
     */
    void scanAndIdentify();

    /**
     * @brief Build slave presence bitmask for STATUS response
     *
     * Each bit corresponds to (SlaveType - 1): bit0=GunFX, bit1=LightFX, etc.
     * Only ready slaves are included.
     */
    uint8_t slaveMask() const;

    /** @brief True if USB Host initialized and running */
    bool isUsbReady() const { return _usbReady.load(std::memory_order_acquire); }

    // ========================================================================
    // Registry & Descriptor Access
    // ========================================================================

    UsbRegistryT<MaxSlaves>& registry() { return UsbRegistryT<MaxSlaves>::instance(); }
    const UsbRegistryT<MaxSlaves>& registry() const { return UsbRegistryT<MaxSlaves>::instance(); }

    /** @brief Find descriptor by slave type (nullptr if not registered) */
    const SlaveDescriptor* findDescriptor(SlaveType type) const;

    /** @brief Number of registered slave descriptors */
    uint8_t descriptorCount() const { return _count; }

    // ========================================================================
    // Timing Constants
    // ========================================================================

    static constexpr uint32_t POLL_INTERVAL_ms      = 100;   ///< Client process() interval
    static constexpr uint32_t DISCOVERY_INTERVAL_ms  = 5000;  ///< Discovery scan interval
    static constexpr uint32_t INIT_TIMEOUT_ms        = 3000;  ///< awaitSlaveReady timeout

private:
    SlaveManagerT() = default;

    // Discovery
    SlaveType tryIdentifySlave(int usbIndex);
    SlaveType identifyByName(const char* name) const;
    BusClient* clientForType(SlaveType type) const;

    // USB callbacks
    void onMount(uint8_t devAddr, uint16_t vid, uint16_t pid);
    void onUnmount(uint8_t devAddr);

    // Internal setup
    void setupLogRelays();

    // Descriptor table
    SlaveDescriptor _descriptors[MaxSlaves];
    uint8_t _count = 0;

    // State
    std::atomic<bool> _usbReady{false};        ///< USB Host initialized
    uint32_t _lastPoll_ms = 0;
    uint32_t _lastDiscovery_ms = 0;
};

/// Default manager with 4 slave slots (matches ScaleFX topology)
using SlaveManager = SlaveManagerT<4>;

// Template implementation
#include "slave_manager.ipp"

#endif // SLAVE_MANAGER_H
