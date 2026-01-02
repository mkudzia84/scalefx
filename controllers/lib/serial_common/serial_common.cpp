/*
 * Serial Common - Implementation
 * 
 * Object-oriented serial communication library for ScaleFX controllers.
 */

#include "serial_common.h"

// USB Host functionality is only needed for HubFX (master)
#ifndef GUNFX_SLAVE
#include "pio_usb.h"
#include "tusb.h"
// Explicitly include TinyUSB host CDC driver
#include "class/cdc/cdc_host.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#endif

// ============================================================================
// Protocol Implementation
// ============================================================================

namespace SerialProtocol {

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output) {
    if (length == 0 || length > MAX_PACKET_SIZE) {
        return 0;
    }
    
    size_t readIndex = 0;
    size_t writeIndex = 1;
    size_t codeIndex = 0;
    uint8_t code = 1;
    
    while (readIndex < length) {
        if (input[readIndex] == 0) {
            output[codeIndex] = code;
            code = 1;
            codeIndex = writeIndex++;
            readIndex++;
        } else {
            output[writeIndex++] = input[readIndex++];
            code++;
            if (code == 0xFF) {
                output[codeIndex] = code;
                code = 1;
                codeIndex = writeIndex++;
            }
        }
    }
    output[codeIndex] = code;
    
    return writeIndex;
}

size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t maxOutput) {
    if (length == 0) {
        return 0;
    }
    
    size_t readIndex = 0;
    size_t writeIndex = 0;
    
    while (readIndex < length) {
        uint8_t code = input[readIndex++];
        if (code == 0) {
            return 0;  // Unexpected zero
        }
        
        for (int i = 1; i < code; i++) {
            if (readIndex >= length || writeIndex >= maxOutput) {
                return 0;
            }
            output[writeIndex++] = input[readIndex++];
        }
        
        if (code < 0xFF && readIndex < length) {
            if (writeIndex >= maxOutput) {
                return 0;
            }
            output[writeIndex++] = 0;
        }
    }
    
    return writeIndex;
}

size_t buildPacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen) {
    if (payloadLen > MAX_PAYLOAD_SIZE) {
        return 0;
    }
    
    output[0] = type;
    output[1] = (uint8_t)payloadLen;
    
    if (payloadLen > 0 && payload != nullptr) {
        memcpy(&output[2], payload, payloadLen);
    }
    
    size_t packetLen = 2 + payloadLen;
    output[packetLen] = crc8(output, packetLen);
    
    return packetLen + 1;
}

size_t encodePacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen) {
    uint8_t raw[MAX_PACKET_SIZE];
    
    size_t rawLen = buildPacket(raw, type, payload, payloadLen);
    if (rawLen == 0) {
        return 0;
    }
    
    size_t encodedLen = cobsEncode(raw, rawLen, output);
    if (encodedLen == 0) {
        return 0;
    }
    
    output[encodedLen] = FRAME_DELIMITER;
    return encodedLen + 1;
}

bool parsePacket(const uint8_t* input, size_t length, uint8_t* type, 
                 const uint8_t** payload, size_t* payloadLen) {
    if (length < 3) {
        return false;
    }
    
    uint8_t pktType = input[0];
    uint8_t pktLen = input[1];
    
    if (length != (size_t)(2 + pktLen + 1)) {
        return false;
    }
    
    uint8_t crc = crc8(input, 2 + pktLen);
    if (crc != input[2 + pktLen]) {
        return false;
    }
    
    *type = pktType;
    *payload = (pktLen > 0) ? &input[2] : nullptr;
    *payloadLen = pktLen;
    
    return true;
}

} // namespace SerialProtocol

#ifndef GUNFX_SLAVE
// ============================================================================
// Global state for TinyUSB callbacks
// ============================================================================

static UsbHost* g_usbHost = nullptr;
static pio_usb_configuration_t g_pioUsbConfig = PIO_USB_DEFAULT_CONFIG;

// Legacy callbacks for C-style API
static UsbMountCallback g_legacyMountCallback = nullptr;
static UsbUnmountCallback g_legacyUnmountCallback = nullptr;
static UsbCdcRxCallback g_legacyCdcRxCallback = nullptr;

// ============================================================================
// UsbHost Implementation
// ============================================================================

UsbHost::~UsbHost() {
    end();
}

bool UsbHost::begin() {
    UsbPortConfig defaultConfig;
    defaultConfig.enabled = true;
    defaultConfig.dp_pin = USB_HOST_PORT_0_DP_DEFAULT;
    strncpy(defaultConfig.name, "USB0", sizeof(defaultConfig.name));
    
    return begin(&defaultConfig, 1);
}

bool UsbHost::begin(const UsbPortConfig* configs, int numPorts) {
    if (_initialized) {
        return true;
    }
    
    Serial.println("[UsbHost] Initializing...");
    
    // Set CPU clock for USB timing
    uint32_t cpuHz = clock_get_hz(clk_sys);
    if (cpuHz != 120000000UL && cpuHz != 240000000UL) {
        Serial.printf("[UsbHost] Adjusting CPU clock from %lu to 120MHz\n", cpuHz);
        set_sys_clock_khz(120000, true);
    }
    
    // Store configurations
    for (int i = 0; i < USB_HOST_MAX_PORTS && i < numPorts; i++) {
        _ports[i] = configs[i];
        if (configs[i].enabled) {
            Serial.printf("[UsbHost] Port %d: D+=%d, D-=%d, name='%s'\n",
                         i, configs[i].dp_pin, configs[i].dp_pin + 1, configs[i].name);
        }
    }
    
    // Configure PIO USB
    for (int i = 0; i < USB_HOST_MAX_PORTS; i++) {
        if (_ports[i].enabled) {
            g_pioUsbConfig.pin_dp = _ports[i].dp_pin;
            Serial.printf("[UsbHost] Primary USB port: D+ pin %d\n", g_pioUsbConfig.pin_dp);
            break;
        }
    }
    
    g_usbHost = this;
    _initialized = true;
    
    Serial.println("[UsbHost] Initialization complete");
    return true;
}

void UsbHost::end() {
    if (!_initialized) return;
    
    _initialized = false;
    _taskRunning = false;
    g_usbHost = nullptr;
    
    Serial.println("[UsbHost] Deinitialized");
}

bool UsbHost::init() {
    if (!_initialized) {
        Serial.println("[UsbHost] Not initialized - call begin() first");
        return false;
    }
    
    Serial.println("[UsbHost] Initializing TinyUSB host...");
    
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &g_pioUsbConfig);
    
    if (!tuh_init(1)) {
        Serial.println("[UsbHost] Core 1: TinyUSB host init failed!");
        return false;
    }
    
    _taskRunning = true;
    Serial.println("[UsbHost] TinyUSB host initialized");
    return true;
}

void UsbHost::process() {
    if (!_initialized || !_taskRunning) return;
    
    // Run TinyUSB host task - handles enumeration, callbacks, etc.
    tuh_task();
    
    // Process CDC receive callbacks
    for (int i = 0; i < _cdcDeviceCount; i++) {
        CdcDeviceInfo& dev = _cdcDevices[i];
        if (dev.connected && dev.state == UsbDeviceState::Ready) {
            if (tuh_cdc_mounted(dev.itf_num) && tuh_cdc_read_available(dev.itf_num)) {
                uint8_t buf[64];
                uint32_t count = tuh_cdc_read(dev.itf_num, buf, sizeof(buf));
                if (count > 0) {
                    _stats.bytes_received += count;
                    if (_cdcRxCallback) {
                        _cdcRxCallback(dev.dev_addr, buf, count);
                    }
                }
            }
        }
    }
}

bool UsbHost::cdcConnected() const {
    for (int i = 0; i < _cdcDeviceCount; i++) {
        if (_cdcDevices[i].connected && _cdcDevices[i].state == UsbDeviceState::Ready) {
            return true;
        }
    }
    return false;
}

int UsbHost::cdcAvailable(int devIndex) const {
    if (devIndex < 0 || devIndex >= _cdcDeviceCount) return 0;
    
    const CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return 0;
    
    if (tuh_cdc_mounted(dev.itf_num)) {
        return tuh_cdc_read_available(dev.itf_num);
    }
    return 0;
}

int UsbHost::cdcRead(int devIndex, uint8_t* buffer, size_t maxLen) {
    if (!buffer || devIndex < 0 || devIndex >= _cdcDeviceCount) return -1;
    
    CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return -1;
    if (!tuh_cdc_mounted(dev.itf_num)) return -1;
    
    uint32_t count = tuh_cdc_read(dev.itf_num, buffer, maxLen);
    if (count > 0) {
        _stats.bytes_received += count;
    }
    return (int)count;
}

int UsbHost::cdcReadByte(int devIndex) {
    uint8_t byte;
    return (cdcRead(devIndex, &byte, 1) == 1) ? byte : -1;
}

int UsbHost::cdcWrite(int devIndex, const uint8_t* data, size_t len) {
    if (!data || devIndex < 0 || devIndex >= _cdcDeviceCount) return -1;
    
    CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (!dev.connected || dev.state != UsbDeviceState::Ready) return -1;
    if (!tuh_cdc_mounted(dev.itf_num)) return -1;
    
    uint32_t count = tuh_cdc_write(dev.itf_num, data, len);
    if (count > 0) {
        _stats.bytes_sent += count;
        tuh_cdc_write_flush(dev.itf_num);
    }
    return (int)count;
}

int UsbHost::cdcPrint(int devIndex, const char* str) {
    if (!str) return -1;
    return cdcWrite(devIndex, (const uint8_t*)str, strlen(str));
}

int UsbHost::cdcPrintln(int devIndex, const char* str) {
    int written = cdcPrint(devIndex, str);
    if (written < 0) return written;
    
    int nl = cdcWrite(devIndex, (const uint8_t*)"\r\n", 2);
    return (nl < 0) ? nl : written + nl;
}

void UsbHost::cdcFlush(int devIndex) {
    if (devIndex < 0 || devIndex >= _cdcDeviceCount) return;
    
    CdcDeviceInfo& dev = _cdcDevices[devIndex];
    if (dev.connected && tuh_cdc_mounted(dev.itf_num)) {
        tuh_cdc_write_flush(dev.itf_num);
    }
}

const CdcDeviceInfo* UsbHost::getCdcDevice(int devIndex) const {
    if (devIndex < 0 || devIndex >= _cdcDeviceCount) return nullptr;
    return &_cdcDevices[devIndex];
}

void UsbHost::printStatus() const {
    Serial.println("\n=== USB Host Status ===");
    Serial.printf("Initialized: %s\n", _initialized ? "Yes" : "No");
    Serial.printf("Task Running: %s\n", _taskRunning ? "Yes" : "No");
    Serial.printf("CPU Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    
    Serial.println("\nUSB Port:");
    for (int i = 0; i < USB_HOST_MAX_PORTS; i++) {
        Serial.printf("  Port %d: %s, D+=%d, D-=%d, name='%s'\n",
                     i, _ports[i].enabled ? "Enabled" : "Disabled",
                     _ports[i].dp_pin, _ports[i].dp_pin + 1, _ports[i].name);
    }
    
    Serial.println("\nCDC Devices:");
    if (_cdcDeviceCount == 0) {
        Serial.println("  (none connected)");
    } else {
        for (int i = 0; i < _cdcDeviceCount; i++) {
            const CdcDeviceInfo& dev = _cdcDevices[i];
            const char* stateStr;
            switch (dev.state) {
                case UsbDeviceState::Disconnected: stateStr = "Disconnected"; break;
                case UsbDeviceState::Connected: stateStr = "Connected"; break;
                case UsbDeviceState::Mounted: stateStr = "Mounted"; break;
                case UsbDeviceState::Ready: stateStr = "Ready"; break;
                default: stateStr = "Unknown"; break;
            }
            Serial.printf("  Device %d: addr=%d, VID=%04X, PID=%04X, state=%s\n",
                         i, dev.dev_addr, dev.vid, dev.pid, stateStr);
        }
    }
    
    Serial.println("\nStatistics:");
    Serial.printf("  Devices mounted: %lu\n", _stats.devices_mounted);
    Serial.printf("  Devices unmounted: %lu\n", _stats.devices_unmounted);
    Serial.printf("  Bytes sent: %lu\n", _stats.bytes_sent);
    Serial.printf("  Bytes received: %lu\n", _stats.bytes_received);
    Serial.println("========================\n");
}

int UsbHost::findDeviceByAddr(uint8_t devAddr) const {
    for (int i = 0; i < _cdcDeviceCount; i++) {
        if (_cdcDevices[i].dev_addr == devAddr) {
            return i;
        }
    }
    return -1;
}

int UsbHost::addCdcDevice(uint8_t devAddr, uint8_t itfNum) {
    if (_cdcDeviceCount >= USB_HOST_MAX_CDC_DEVICES) {
        Serial.println("[UsbHost] Max CDC devices reached");
        return -1;
    }
    
    int idx = _cdcDeviceCount;
    CdcDeviceInfo& dev = _cdcDevices[idx];
    
    dev.connected = true;
    dev.dev_addr = devAddr;
    dev.itf_num = itfNum;
    dev.state = UsbDeviceState::Connected;
    
    tuh_vid_pid_get(devAddr, &dev.vid, &dev.pid);
    
    _cdcDeviceCount++;
    _stats.devices_mounted++;
    
    Serial.printf("[UsbHost] CDC device added: addr=%d, itf=%d, VID=%04X, PID=%04X\n",
                  devAddr, itfNum, dev.vid, dev.pid);
    
    return idx;
}

void UsbHost::removeCdcDevice(uint8_t devAddr) {
    int idx = findDeviceByAddr(devAddr);
    if (idx < 0) return;
    
    Serial.printf("[UsbHost] CDC device removed: addr=%d\n", devAddr);
    
    for (int i = idx; i < _cdcDeviceCount - 1; i++) {
        _cdcDevices[i] = _cdcDevices[i + 1];
    }
    _cdcDeviceCount--;
    _stats.devices_unmounted++;
}

// Internal callbacks from TinyUSB
void UsbHost::_onDeviceMount(uint8_t devAddr) {
    Serial.printf("[UsbHost] Device mounted: addr=%d\n", devAddr);
    
    uint16_t vid, pid;
    tuh_vid_pid_get(devAddr, &vid, &pid);
    
    if (_mountCallback) {
        _mountCallback(devAddr, vid, pid);
    }
}

void UsbHost::_onDeviceUnmount(uint8_t devAddr) {
    Serial.printf("[UsbHost] Device unmounted: addr=%d\n", devAddr);
    
    removeCdcDevice(devAddr);
    
    if (_unmountCallback) {
        _unmountCallback(devAddr);
    }
}

void UsbHost::_onCdcMount(uint8_t idx) {
    Serial.printf("[UsbHost] CDC mounted: idx=%d\n", idx);
    
    tuh_itf_info_t info;
    if (tuh_cdc_itf_get_info(idx, &info)) {
        int devIdx = addCdcDevice(info.daddr, idx);
        if (devIdx >= 0) {
            _cdcDevices[devIdx].state = UsbDeviceState::Ready;
        }
    }
    
    cdc_line_coding_t lineCoding = {
        .bit_rate = 115200,
        .stop_bits = 0,
        .parity = 0,
        .data_bits = 8
    };
    tuh_cdc_set_line_coding(idx, &lineCoding, nullptr, 0);
    tuh_cdc_set_control_line_state(idx, CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS, nullptr, 0);
}

void UsbHost::_onCdcUnmount(uint8_t idx) {
    Serial.printf("[UsbHost] CDC unmounted: idx=%d\n", idx);
}

void UsbHost::_onCdcRx(uint8_t idx) {
    if (_cdcRxCallback) {
        uint8_t buf[64];
        uint32_t count = tuh_cdc_read(idx, buf, sizeof(buf));
        if (count > 0) {
            for (int i = 0; i < _cdcDeviceCount; i++) {
                if (_cdcDevices[i].itf_num == idx) {
                    _cdcRxCallback(_cdcDevices[i].dev_addr, buf, count);
                    _stats.bytes_received += count;
                    break;
                }
            }
        }
    }
}

#endif // !GUNFX_SLAVE

#ifndef GUNFX_SLAVE
// ============================================================================
// SerialBus Implementation
// ============================================================================

bool SerialBus::begin(UsbHost* usbHost, int deviceIndex) {
    if (_initialized) return true;
    
    _usbHost = usbHost;
    _deviceIndex = deviceIndex;
    _rxIndex = 0;
    _stats = {};
    _initialized = true;
    
    Serial.printf("[SerialBus] Initialized for device %d\n", deviceIndex);
    return true;
}

void SerialBus::end() {
    if (!_initialized) return;
    
    _initialized = false;
    _usbHost = nullptr;
    Serial.println("[SerialBus] Deinitialized");
}

void SerialBus::setDevice(int deviceIndex) {
    _deviceIndex = deviceIndex;
    _rxIndex = 0;
    Serial.printf("[SerialBus] Device changed to %d\n", deviceIndex);
}

int SerialBus::sendPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_usbHost) return -1;
    if (len > SerialProtocol::MAX_PAYLOAD_SIZE) return -1;
    
    uint8_t encoded[SerialProtocol::COBS_BUFFER_SIZE];
    size_t encLen = SerialProtocol::encodePacket(encoded, type, payload, len);
    if (encLen == 0) {
        Serial.printf("[SerialBus] Encode failed for type 0x%02X\n", type);
        return -1;
    }
    
    int written = _usbHost->cdcWrite(_deviceIndex, encoded, encLen);
    if (written < 0) {
        Serial.printf("[SerialBus] Write failed for type 0x%02X\n", type);
        return -1;
    }
    
    _stats.packets_sent++;
    return 0;
}

int SerialBus::sendKeepalive() {
    int result = sendPacket(SerialProtocol::SFX_PKT_KEEPALIVE);
    if (result == 0) {
        _lastKeepaliveMs = millis();
    }
    return result;
}

int SerialBus::process() {
    if (!_initialized || !_usbHost) return 0;
    
    int packetsProcessed = 0;
    
    uint8_t readBuf[64];
    int n = _usbHost->cdcRead(_deviceIndex, readBuf, sizeof(readBuf));
    if (n <= 0) return 0;
    
    for (int i = 0; i < n; i++) {
        uint8_t byte = readBuf[i];
        
        if (byte == SerialProtocol::FRAME_DELIMITER) {
            if (_rxIndex > 0) {
                processFrame(_rxBuffer, _rxIndex);
                packetsProcessed++;
            }
            _rxIndex = 0;
        } else {
            if (_rxIndex < SERIAL_BUS_RX_BUFFER_SIZE) {
                _rxBuffer[_rxIndex++] = byte;
            } else {
                _rxIndex = 0;
                _stats.framing_errors++;
            }
        }
    }
    
    return packetsProcessed;
}

void SerialBus::processFrame(const uint8_t* frame, size_t frameLen) {
    if (frameLen < 4) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t decoded[SerialProtocol::MAX_PACKET_SIZE];
    size_t decLen = SerialProtocol::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
    if (decLen < 3) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t type;
    const uint8_t* payload;
    size_t payloadLen;
    
    if (!SerialProtocol::parsePacket(decoded, decLen, &type, &payload, &payloadLen)) {
        _stats.crc_errors++;
        Serial.printf("[SerialBus] Packet parse failed, len=%zu\n", decLen);
        return;
    }
    
    _stats.packets_received++;
    
    if (_rxCallback) {
        _rxCallback(type, payload, payloadLen);
    }
}

void SerialBus::setKeepaliveInterval(unsigned long intervalMs) {
    _keepaliveIntervalMs = intervalMs;
    _lastKeepaliveMs = millis();
}

bool SerialBus::processKeepalive() {
    if (!_initialized || _keepaliveIntervalMs == 0) return false;
    
    unsigned long now = millis();
    if (now - _lastKeepaliveMs >= _keepaliveIntervalMs) {
        sendKeepalive();
        return true;
    }
    return false;
}

bool SerialBus::isConnected() const {
    if (!_initialized || !_usbHost) return false;
    
    const CdcDeviceInfo* device = _usbHost->getCdcDevice(_deviceIndex);
    return (device != nullptr && device->connected);
}

void SerialBus::resetStats() {
    _stats = {};
}

#endif // !GUNFX_SLAVE

#ifndef GUNFX_SLAVE
// ============================================================================
// TinyUSB Callbacks
// ============================================================================

void tuh_mount_cb(uint8_t dev_addr) {
    if (g_usbHost) {
        g_usbHost->_onDeviceMount(dev_addr);
    }
    if (g_legacyMountCallback) {
        uint16_t vid, pid;
        tuh_vid_pid_get(dev_addr, &vid, &pid);
        g_legacyMountCallback(dev_addr, vid, pid);
    }
}

void tuh_umount_cb(uint8_t dev_addr) {
    if (g_usbHost) {
        g_usbHost->_onDeviceUnmount(dev_addr);
    }
    if (g_legacyUnmountCallback) {
        g_legacyUnmountCallback(dev_addr);
    }
}

void tuh_cdc_mount_cb(uint8_t idx) {
    if (g_usbHost) {
        g_usbHost->_onCdcMount(idx);
    }
}

void tuh_cdc_umount_cb(uint8_t idx) {
    if (g_usbHost) {
        g_usbHost->_onCdcUnmount(idx);
    }
}

void tuh_cdc_rx_cb(uint8_t idx) {
    if (g_usbHost) {
        g_usbHost->_onCdcRx(idx);
    }
    if (g_legacyCdcRxCallback && g_usbHost) {
        uint8_t buf[64];
        uint32_t count = tuh_cdc_read(idx, buf, sizeof(buf));
        if (count > 0) {
            for (int i = 0; i < g_usbHost->cdcDeviceCount(); i++) {
                const CdcDeviceInfo* dev = g_usbHost->getCdcDevice(i);
                if (dev && dev->itf_num == idx) {
                    g_legacyCdcRxCallback(dev->dev_addr, buf, count);
                    break;
                }
            }
        }
    }
}

void tuh_cdc_tx_complete_cb(uint8_t idx) {
    (void)idx;
}

#endif // !GUNFX_SLAVE
