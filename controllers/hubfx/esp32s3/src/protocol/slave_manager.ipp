/*
 * Slave Manager — Template Implementation
 *
 * Included at the bottom of slave_manager.h. Do not include directly.
 *
 * Contains all SlaveManagerT<MaxSlaves> method implementations.
 */

#define SLAVE_MGR_LOG(fmt, ...) SFX_LOG_DEBUG("[SlaveMgr] " fmt, ##__VA_ARGS__)

// ============================================================================
// Registration
// ============================================================================

template <size_t MaxSlaves>
bool SlaveManagerT<MaxSlaves>::addSlave(const SlaveDescriptor& desc) {
    if (_count >= MaxSlaves) {
        SFX_LOG_ERROR("[SlaveMgr] Cannot add slave %s — table full (%d/%d)",
                      slaveTypeName(desc.type), _count, MaxSlaves);
        return false;
    }
    _descriptors[_count++] = desc;
    SLAVE_MGR_LOG("Registered %s (prefix='%s', log='%s')",
                  slaveTypeName(desc.type), desc.namePrefix, desc.logPrefix);
    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

template <size_t MaxSlaves>
bool SlaveManagerT<MaxSlaves>::begin() {
    auto& reg = registry();

    // Pre-register all slave types with UsbRegistry (client pointers, no USB index yet)
    for (uint8_t i = 0; i < _count; i++) {
        reg.registerSlave(_descriptors[i].type, _descriptors[i].client, -1);
    }
    SLAVE_MGR_LOG("Registry: %d slots pre-registered", reg.count());

    // Set up DiagLog relays for all registered clients
    setupLogRelays();

    // Initialize USB Host
    UsbHost& usb = UsbHost::instance();

    // Register mount/unmount callbacks BEFORE init() so we catch
    // any devices that enumerate during startup.
    usb.onMount([this](uint8_t devAddr, uint16_t vid, uint16_t pid) {
        onMount(devAddr, vid, pid);
    });
    usb.onUnmount([this](uint8_t devAddr) {
        onUnmount(devAddr);
    });

    if (usb.begin()) {
        if (usb.init()) {
            _usbReady.store(true, std::memory_order_release);
            SFX_LOG_INFO("USB Host ready (%s)", usb.backendName());
            return true;
        } else {
            SFX_LOG_ERROR("USB Host init() failed");
        }
    } else {
        SFX_LOG_ERROR("USB Host begin() failed");
    }
    return false;
}

template <size_t MaxSlaves>
void SlaveManagerT<MaxSlaves>::process() {
    uint32_t now = millis();
    auto& reg = registry();

    // ---- Slave client polling (rate-limited) ----
    if (now - _lastPoll_ms >= POLL_INTERVAL_ms) {
        _lastPoll_ms = now;
        for (uint8_t i = 0; i < reg.count(); i++) {
            SlaveEntry& slave = reg[i];
            if (slave.client && slave.connected && slave.ready) {
                slave.client->process();
            }
        }
    }

    // ---- Periodic slave discovery (rate-limited) ----
    if (_usbReady.load(std::memory_order_acquire) &&
        now - _lastDiscovery_ms >= DISCOVERY_INTERVAL_ms) {
        _lastDiscovery_ms = now;

        // Check if any slot still needs identification
        bool anyUnidentified = false;
        for (uint8_t i = 0; i < reg.count(); i++) {
            if (!reg[i].connected) {
                anyUnidentified = true;
                break;
            }
        }

        UsbHost& usb = UsbHost::instance();
        if (anyUnidentified && usb.cdcDeviceCount() > 0) {
            scanAndIdentify();
        }
    }
}

// ============================================================================
// Discovery
// ============================================================================

template <size_t MaxSlaves>
void SlaveManagerT<MaxSlaves>::scanAndIdentify() {
    UsbHost& usb = UsbHost::instance();
    int devCount = usb.cdcDeviceCount();
    SLAVE_MGR_LOG("Scanning %d USB CDC devices for slaves...", devCount);

    auto& reg = registry();

    for (int i = 0; i < devCount; i++) {
        const CdcDeviceInfo* info = usb.getCdcDevice(i);
        if (!info || !info->connected) continue;

        // Check if this device index is already assigned to a connected slave
        bool alreadyAssigned = false;
        for (uint8_t s = 0; s < reg.count(); s++) {
            if (reg[s].usbIndex == i && reg[s].connected) {
                alreadyAssigned = true;
                break;
            }
        }

        if (!alreadyAssigned) {
            tryIdentifySlave(i);
        }
    }
}

template <size_t MaxSlaves>
SlaveType SlaveManagerT<MaxSlaves>::tryIdentifySlave(int usbIndex) {
    SLAVE_MGR_LOG("Probing USB index %d via IDENTIFY...", usbIndex);

    // Create a temporary generic client to probe the device
    BusClient probe;
    if (!probe.begin(usbIndex)) {
        SFX_LOG_ERROR("[SlaveMgr] Failed to begin probe on USB index %d", usbIndex);
        return SlaveType::Unknown;
    }

    // Send IDENTIFY (non-destructive — does not trigger init on the slave)
    probe.sendIdentify();

    if (!awaitSlaveReady(probe, INIT_TIMEOUT_ms)) {
        SFX_LOG_WARN("[SlaveMgr] No IDENTIFY response from USB index %d (timeout %lums)",
                     usbIndex, (unsigned long)INIT_TIMEOUT_ms);
        return SlaveType::Unknown;
    }

    const char* name = probe.serverName();
    SFX_LOG_INFO("[SlaveMgr] USB index %d identified as: %s", usbIndex, name);

    // Match by name prefix against registered descriptors
    SlaveType type = identifyByName(name);
    if (type == SlaveType::Unknown) {
        SFX_LOG_WARN("[SlaveMgr] Unknown slave type: %s", name);
        return SlaveType::Unknown;
    }

    // Find the registered client for this type
    BusClient* client = clientForType(type);
    if (!client) {
        SFX_LOG_ERROR("[SlaveMgr] No client registered for %s", slaveTypeName(type));
        return SlaveType::Unknown;
    }

    // Bind the typed client to this USB device
    if (!client->begin(usbIndex)) {
        SFX_LOG_ERROR("[SlaveMgr] Failed to bind %s client to USB index %d",
                      slaveTypeName(type), usbIndex);
        return SlaveType::Unknown;
    }

    // Send IDENTIFY on the typed client to populate its boardInfo
    client->sendIdentify();

    if (!client->isServerReady() && !awaitSlaveReady(*client, INIT_TIMEOUT_ms)) {
        SFX_LOG_ERROR("[SlaveMgr] Typed client IDENTIFY timeout for %s", slaveTypeName(type));
        return SlaveType::Unknown;
    }

    auto& reg = registry();
    reg.registerSlave(type, client, usbIndex);
    reg.setConnected(type, true);

    // Optional auto-INIT in SLAVE mode. When the descriptor opts in, the
    // manager sends INIT immediately so the slave activates and the
    // registry onReady callback fires (config-push, etc.) without waiting
    // for an explicit SLAVE_INIT command from the upstream host.
    const SlaveDescriptor* desc = findDescriptor(type);
    if (desc && desc->autoInit) {
        SFX_LOG_INFO("[SlaveMgr] Auto-INIT %s in SLAVE mode...", slaveTypeName(type));
        if (client->sendInit() < 0) {
            SFX_LOG_ERROR("[SlaveMgr] Auto-INIT send failed for %s", slaveTypeName(type));
            return type;  // entry stays connected; explicit SLAVE_INIT can retry
        }
        if (awaitSlaveReady(*client, INIT_TIMEOUT_ms)) {
            reg.setReady(type, true);   // fires onReady callback
            SFX_LOG_INFO("[SlaveMgr] Auto-INIT OK: %s → %s (ready)",
                         slaveTypeName(type), client->serverName());
        } else {
            SFX_LOG_WARN("[SlaveMgr] Auto-INIT timeout for %s — entry stays connected; "
                         "host may retry via SLAVE_INIT or unplug/replug",
                         slaveTypeName(type));
        }
        return type;
    }

    SFX_LOG_INFO("[SlaveMgr] Slave %s identified: %s (connected, awaiting INIT)",
                 slaveTypeName(type), client->serverName());
    return type;
}

template <size_t MaxSlaves>
SlaveType SlaveManagerT<MaxSlaves>::identifyByName(const char* name) const {
    if (!name) return SlaveType::Unknown;

    for (uint8_t i = 0; i < _count; i++) {
        const char* prefix = _descriptors[i].namePrefix;
        if (prefix && strncmp(name, prefix, strlen(prefix)) == 0) {
            return _descriptors[i].type;
        }
    }
    return SlaveType::Unknown;
}

template <size_t MaxSlaves>
BusClient* SlaveManagerT<MaxSlaves>::clientForType(SlaveType type) const {
    for (uint8_t i = 0; i < _count; i++) {
        if (_descriptors[i].type == type) {
            return _descriptors[i].client;
        }
    }
    return nullptr;
}

// ============================================================================
// Status
// ============================================================================

template <size_t MaxSlaves>
uint8_t SlaveManagerT<MaxSlaves>::slaveMask() const {
    uint8_t mask = 0;
    auto& reg = registry();
    for (uint8_t i = 0; i < reg.count(); i++) {
        if (reg[i].ready) {
            mask |= (1 << ((uint8_t)reg[i].type - 1));
        }
    }
    return mask;
}

template <size_t MaxSlaves>
const SlaveDescriptor* SlaveManagerT<MaxSlaves>::findDescriptor(SlaveType type) const {
    for (uint8_t i = 0; i < _count; i++) {
        if (_descriptors[i].type == type) {
            return &_descriptors[i];
        }
    }
    return nullptr;
}

// ============================================================================
// USB Callbacks
// ============================================================================

template <size_t MaxSlaves>
void SlaveManagerT<MaxSlaves>::onMount(uint8_t devAddr, uint16_t vid, uint16_t pid) {
    const char* name = knownDeviceName(vid, pid);
    if (name) {
        SFX_LOG_INFO("USB device mounted: addr=%d VID=%04X PID=%04X — %s",
                     devAddr, vid, pid, name);
    } else {
        SFX_LOG_INFO("USB device mounted: addr=%d VID=%04X PID=%04X (unknown)",
                     devAddr, vid, pid);
    }
}

template <size_t MaxSlaves>
void SlaveManagerT<MaxSlaves>::onUnmount(uint8_t devAddr) {
    SFX_LOG_WARN("USB device unmounted: addr=%d", devAddr);

    UsbHost& usb = UsbHost::instance();
    auto& reg = registry();
    for (uint8_t i = 0; i < reg.count(); i++) {
        SlaveEntry& slave = reg[i];
        if (slave.connected) {
            const CdcDeviceInfo* info = usb.getCdcDevice(slave.usbIndex);
            if (!info || !info->connected || info->dev_addr == devAddr) {
                reg.setConnected(slave.type, false);
                SFX_LOG_WARN("Slave %s disconnected (USB addr=%d)",
                             slaveTypeName(slave.type), devAddr);
            }
        }
    }
}

// ============================================================================
// Internal Setup
// ============================================================================

template <size_t MaxSlaves>
void SlaveManagerT<MaxSlaves>::setupLogRelays() {
    for (uint8_t i = 0; i < _count; i++) {
        const char* prefix = _descriptors[i].logPrefix;
        BusClient* client = _descriptors[i].client;
        if (client && prefix) {
            client->onLogMessage([prefix](uint8_t level, uint32_t, const char* msg) {
                char buf[160];
                snprintf(buf, sizeof(buf), "[%s] %s", prefix, msg);
                DiagLog::instance().ingest(level, buf);
            });
        }
    }
}
