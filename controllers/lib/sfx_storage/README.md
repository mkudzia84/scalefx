# sfx_storage — SD Card & Flash Storage

Thread-safe storage singletons for SD card and onboard LittleFS flash. Both modules share a uniform `FileEntry`-based API so protocol handlers (e.g., `HubFxStorageServer`) can operate on either backend interchangeably.

**Requires** `SFX_HAS_STORAGE=1` build flag.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `storage_types.h` | 28 | Shared `FileEntry` struct (name, isDirectory, size) |
| `sd_card.h` | ~300 | `SdCardModule` singleton — SPI (all platforms) + SDIO (ESP32) |
| `sd_card.cpp` | ~370 | SdCardModule implementation (platform-conditional) |
| `flash.h` | 229 | `FlashModule` singleton — LittleFS on onboard flash |
| `flash.cpp` | 308 | FlashModule implementation (platform-conditional) |

## Architecture

```
    ┌──────────────────────────┐     ┌──────────────────┐
    │     SdCardModule         │     │  FlashModule      │
    │     (singleton)          │     │  (singleton)      │
    │                          │     │                   │
    │  ┌─────────┬───────────┐ │     │  Backend: LittleFS│
    │  │SdFat/SPI│ SD/SD_MMC │ │     │  Media: Onboard   │
    │  │ (Pico)  │  (ESP32)  │ │     │  File type:LFSFile│
    │  └─────────┴───────────┘ │     └────────┬──────────┘
    │  File type: SdFile       │              │
    └───────────┬──────────────┘              │
                │   Shared API pattern        │
                │  ┌─────────────────┐        │
                └──│   FileEntry     │────────┘
                   │   storage_types │
                   └─────────────────┘
```

Both modules follow the same API pattern:
- `listDirectory(path, callback)` — iterate entries
- `listTree(path, callback)` — recursive listing with depth
- `getFileInfo(path, entry)` — single file metadata
- `getStorageInfo(info)` — capacity/usage
- `removeFile(path)` / `makeDirectory(path)` — modification
- `openRead(path, file)` / `openWrite(path, file, truncate)` — file I/O (caller holds lock)
- `lock()` / `tryLock()` / `unlock()` — mutex access for file I/O

## SdCardModule — SD Card (SPI + SDIO)

Singleton wrapping platform-specific SD card libraries with automatic backend selection:

| Platform | Backend | Bus Modes | File Type | Library |
|----------|---------|-----------|-----------|---------|
| **Pico** (RP2040/RP2350) | SdFat (Adafruit fork) | SPI only | `File32` | `<SdFat.h>` |
| **ESP32-S3** | Arduino-ESP32 SD/SD_MMC | **SPI + SDIO** (1-bit/4-bit) | `fs::File` | `<SD.h>` / `<SD_MMC.h>` |

Platform detection is automatic via `SFX_PLATFORM_PICO` / `SFX_PLATFORM_ESP32` macros. A unified `SdFile` typedef aliases the platform-specific file type.

### Bus Mode

```cpp
enum class SdBusMode : uint8_t {
    SPI,        // SPI bus (all platforms, ~2-4 MB/s)
    SDIO_1BIT,  // 1-bit SDIO (ESP32 only)
    SDIO_4BIT   // 4-bit SDIO (ESP32 only, ~20-25 MB/s)
};
```

### Lifecycle — SPI Mode (all platforms)

```cpp
SdCardModule& sd = SdCardModule::instance();
SdCardModule::Config cfg { .cs_pin = 5, .sck_pin = 2, .mosi_pin = 3, .miso_pin = 4, .speed_mhz = 25 };
sd.begin(cfg);
```

### Lifecycle — SDIO Mode (ESP32 only)

```cpp
SdCardModule& sd = SdCardModule::instance();

// 1-bit SDIO with custom pins
SdCardModule::Config cfg { .clk = 7, .cmd = 9, .d0 = 8 };
sd.begin(cfg);

// 4-bit SDIO — use SdCardModuleT<EspSdio4BitPolicy>
// (change the using alias in sd_card.h to select 4-bit mode)
```

### Retry

```cpp
sd.config().speed_mhz = 10;  // Change SPI speed before retry (ignored for SDIO)
sd.retryInit();               // Unmounts, then re-mounts with stored config
```

### Policy Access

```cpp
// Access the underlying filesystem for low-level operations
auto& policy = sd.policy();

// Pico: policy.rawSD() returns SdFat&
// ESP32: policy.rawFS() returns fs::FS& (SD or SD_MMC)
```

### Error Codes (`SdError`)

| Code | Name | Value |
|------|------|-------|
| OK | Success | 0 |
| NOT_INITIALIZED | `begin()` not called or failed | 1 |
| NOT_FOUND | File/directory doesn't exist | 2 |
| IO_ERROR | SPI/FAT operation failed | 3 |
| IS_DIRECTORY | Expected file, got directory | 4 |
| ALREADY_EXISTS | File exists where directory expected | 5 |

### StorageInfo

```cpp
struct StorageInfo {
    bool initialized;
    uint32_t cardSize_MB;
    uint32_t totalSpace_MB;
    uint32_t usedSpace_MB;
    uint32_t freeSpace_MB;
    uint8_t fatType;             // FAT16/32 (Pico/SdFat), 0 on ESP32
    uint32_t clusterSize_bytes;  // Pico/SdFat only, 0 on ESP32
    SdBusMode busMode;           // Active bus mode
    SdCardType cardType;         // Card type (SD, SDHC, MMC, etc.)
};
```

### Backend Detection

Backend is selected automatically at compile time:

| Macro | When True | Backend |
|-------|-----------|--------|
| `SFX_SD_BACKEND_SDFAT` | Pico + SdFat available | SdFat over SPI |
| `SFX_SD_BACKEND_ESP` | ESP32 | Arduino SD.h / SD_MMC.h |
| `SFX_HAS_SD` | Either backend available | Class compiles |

On Pico, `__has_include(<SdFat.h>)` gracefully degrades if SdFat isn't in the library path.
On ESP32, `lib_ignore = SdFat` in platformio.ini prevents conflicts with the framework's native `SD.h`.

### Direct Policy Access

```cpp
// Pico (SdFat backend)
SdFat& raw = sd.policy().rawSD();   // SdFat object

// ESP32 (SD/SD_MMC backend)
fs::FS& raw = sd.policy().rawFS();  // Points to SD or SD_MMC depending on policy
```

## FlashModule — LittleFS Onboard Flash

Singleton wrapping the **LittleFS** filesystem for onboard flash storage.

### Lifecycle

```cpp
FlashModule& flash = FlashModule::instance();
flash.begin();  // Mounts LittleFS partition
```

### Error Codes (`FlashError`)

Same values as `SdError` for consistency:

| Code | Name | Value |
|------|------|-------|
| OK | Success | 0 |
| NOT_INITIALIZED | `begin()` not called or failed | 1 |
| NOT_FOUND | File/directory doesn't exist | 2 |
| IO_ERROR | Flash write/read failed | 3 |
| IS_DIRECTORY | Expected file, got directory | 4 |
| ALREADY_EXISTS | File exists where directory expected | 5 |

### Storage Info (`FlashStorageInfo`)

```cpp
struct FlashStorageInfo {
    bool initialized;
    uint32_t totalBytes;   // Byte-level (flash is typically < 2 MB)
    uint32_t usedBytes;
    uint32_t freeBytes;
};
```

### Platform Differences

The LittleFS Arduino wrapper has slightly different APIs per platform. FlashModule handles this with `#ifdef` branches:

| Operation | RP2040 (Arduino-Pico) | ESP32 (Arduino-ESP32) |
|-----------|-----------------------|------------------------|
| Directory listing | `Dir` iterator (`openDir`/`next`) | `File.openNextFile()` pattern |
| Storage info | `FSInfo` struct via `LittleFS.info()` | `LittleFS.totalBytes()`/`usedBytes()` |
| File open/close | `::File` | `::File` (same) |
| mkdir | Recursive by default | Recursive by default |

### ESP32 LittleFS — Native API Assessment

**Current:** Uses Arduino `LittleFS` wrapper (`<LittleFS.h>`).

**Underneath:** Arduino-ESP32's LittleFS is a thin VFS shim over ESP-IDF's `esp_littlefs` component. It mounts LittleFS onto the ESP VFS layer, so `LittleFS.open()` ultimately calls `esp_littlefs` → native SPI flash driver.

**Going native (raw `esp_littlefs`)?** Not beneficial:
- Same underlying `lfs_*` library — no performance difference
- Would require manual VFS path registration, partition table lookup, and raw `lfs_file_*` calls
- Loses Arduino `File` compatibility (breaking uniform API with Pico)
- The `#ifdef` branches in FlashModule already handle the only meaningful API differences

**Verdict:** Current approach is correct. No native API change recommended.

## Thread Safety

Both modules use `SfxMutex` (platform-abstracted: `mutex_t` on Pico, `SemaphoreHandle_t` on ESP32):

| Operation Type | Lock | Who Holds It |
|----------------|------|-------------- |
| `listDirectory`, `listTree` | Auto (internal) | Module acquires/releases |
| `getFileInfo`, `getStorageInfo` | Auto (internal) | Module acquires/releases |
| `removeFile`, `makeDirectory` | Auto (internal) | Module acquires/releases |
| `openRead`, `openWrite` + I/O | **Manual** | Caller must `lock()`/`unlock()` |

The split exists because file I/O spans multiple calls (`open` → `read`/`write` → `close`), so the caller must hold the lock for the duration.

## ESP32 SD — SPI vs SDIO

### Throughput Comparison

| Bus Mode | Throughput | Use Case |
|----------|-----------|----------|
| SPI | ~2-4 MB/s | Dev boards (GPIO breakout), simple wiring |
| SDIO 1-bit | ~10-12 MB/s | Fewer pins, moderate throughput |
| SDIO 4-bit | ~20-25 MB/s | Production boards, audio streaming |

For HubFX audio (8-channel WAV mixing), reading 8 simultaneous 44.1 kHz 16-bit stereo WAV files requires ~1.4 MB/s sustained. SPI's ~2-4 MB/s leaves thin margin. **SDIO 4-bit provides 10x headroom.**

### ESP32-S3 SDIO Wiring

| Signal | GPIO | Note |
|--------|------|------|
| CLK | GPIO36 | Recommended |
| CMD | GPIO35 | Recommended |
| D0 | GPIO37 | Required (1-bit minimum) |
| D1 | GPIO38 | 4-bit mode |
| D2 | GPIO33 | 4-bit mode |
| D3 | GPIO34 | 4-bit mode (also CS for SPI fallback) |

*(Actual pins depend on PCB design — these are ESP32-S3 DevKit defaults.)*

### Implementation

The SD card module uses conditional compilation (Option B from original analysis):

```cpp
#if SFX_SD_BACKEND_SDFAT
    using SdFile = File32;   // Pico: SdFat type
#elif SFX_SD_BACKEND_ESP
    using SdFile = fs::File; // ESP32: Arduino FS type (same for SD and SD_MMC)
#endif
```

Internally, `SdCardModule` stores a `fs::FS*` pointer on ESP32 that points to either `SD` (SPI mode) or `SD_MMC` (SDIO mode). All file operations dispatch through this pointer. Only `getStorageInfo()` needs bus-mode-specific dispatch (for card size/capacity queries).

## Dependencies

| Dependency | Platform | Reason |
|------------|----------|--------|
| `sfx_platform` | All | `sfx_platform.h` (SfxMutex, platform macros), `diag_log.h` (logging) |
| `Arduino` | All | `<Arduino.h>` (base types) |
| `SdFat` | Pico only | SD card backend (SPI). Excluded on ESP32 via `lib_ignore`. |
| `SD.h` | ESP32 only | SD card SPI mode (Arduino-ESP32 framework, auto-available) |
| `SD_MMC.h` | ESP32 only | SD card SDIO mode (Arduino-ESP32 framework, auto-available) |
| `LittleFS` | All | Arduino wrapper over native flash filesystem |

**No dependency on:** sfx_serial, sfx_audio, sfx_usb, sfx_peripherals, sfx_server.
