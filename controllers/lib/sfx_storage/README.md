# sfx_storage — SD Card & Flash Storage

Thread-safe storage singletons for SD card and onboard flash. Both modules share a uniform `FileEntry`-based API so protocol handlers (e.g., `StorageServicePolicy`) and config systems (e.g., LightFX, GearControl `ConfigStore`) can operate on either backend interchangeably.

**Requires** `SFX_HAS_STORAGE=1` build flag.

## Files

| File | Purpose |
|------|---------|
| `storage_types.h` | Shared `FileEntry` struct (name, isDirectory, size) + `MAX_TREE_DEPTH`/`MAX_TREE_ENTRIES` |
| `sd_card.h` / `sd_card.ipp` | `SdCardModuleT<TPolicy>` template — generic state machine + locking; policy injects backend |
| `flash.h` / `flash.cpp` | `FlashModule` singleton — LittleFS via `esp_littlefs` (ESP32) or Arduino LittleFS (Pico) |
| `esp32/native_file.h` / `.cpp` | **(ESP32)** `NativeFile` — POSIX RAII file/dir wrapper used by both SD and flash |
| `esp32/esp_idf_sd_policy.h` / `.cpp` | **(ESP32)** `EspIdfSdio4BitPolicy` — mounts SD via `esp_vfs_fat_sdmmc_mount` |
| `pico/pico_sd_policy.h` | **(Pico)** `PicoSpiSdPolicy` — SdFat over SPI |
| `bring_up.h` | One-liner `bringUpStorage(sdCfg)` for controller `setup()` |

## Architecture

```
                            ┌─────────────────┐
                            │   StorageFile   │
                            │ (handle alias)  │
                            └────────┬────────┘
                                     │
        ┌───────── ESP32 ────────────┼───────── Pico ───────────┐
        │                            │                          │
        │    using StorageFile       │    using StorageFile     │
        │      = NativeFile          │      = ::File (Arduino)  │
        │                            │                          │
        │  ┌─────────────────┐       │   ┌─────────────────┐    │
        │  │  SdCardModule   │       │   │  SdCardModule   │    │
        │  │ EspIdfSdio4Bit  │       │   │  PicoSpiSdPolicy│    │
        │  │  Policy         │       │   │                 │    │
        │  │                 │       │   │  SdFat / SPI    │    │
        │  │  esp_vfs_fat_   │       │   │  → File32       │    │
        │  │  sdmmc_mount    │       │   │                 │    │
        │  │  → POSIX over   │       │   │                 │    │
        │  │   "/sdcard/..." │       │   │                 │    │
        │  └─────────────────┘       │   └─────────────────┘    │
        │                            │                          │
        │  ┌─────────────────┐       │   ┌─────────────────┐    │
        │  │  FlashModule    │       │   │  FlashModule    │    │
        │  │                 │       │   │                 │    │
        │  │  esp_vfs_       │       │   │  Arduino        │    │
        │  │  littlefs_      │       │   │  LittleFS       │    │
        │  │  register       │       │   │  (Dir / File)   │    │
        │  │  → POSIX over   │       │   │                 │    │
        │  │  "/littlefs/.." │       │   │                 │    │
        │  └─────────────────┘       │   └─────────────────┘    │
        │                            │                          │
        └────────────────────────────┴──────────────────────────┘
                                     │
                            ┌────────┴────────┐
                            │    FileEntry    │
                            │  storage_types  │
                            └─────────────────┘
```

Both modules expose the same surface:
- `listDirectory(path, callback)` — iterate entries
- `listTree(path, callback)` — recursive listing with depth
- `getFileInfo(path, entry)` — single file metadata
- `getStorageInfo(info)` — capacity/usage
- `removeFile(path)` — delete a single file
- `removeDirectory(path, recursive = true)` — delete a directory (recursive deletes non-empty; non-recursive fails on non-empty)
- `makeDirectory(path, createParents = false)` — create a directory (with `createParents=true`, mkdir `-p` semantics, idempotent)
- `openRead(path, file)` / `openWrite(path, file, truncate)` — file I/O (caller holds lock)
- `lock()` / `tryLock()` / `unlock()` — mutex access — see Thread Safety below

### Recursive delete — iterator-safe implementation

Both `FlashModule::removeDirectoryRecursive` and `SdCardModuleT::removeDirectoryRecursive` follow a **snapshot-then-delete** pattern: the child list is copied into a local `std::vector<ChildEntry>` with the directory iterator open, the iterator is closed, and only then are entries removed. Deleting through a live iterator can silently truncate enumeration on some filesystem backends (originally observed with RP2040 LittleFS), leaving orphans that would cause the final `rmdir` to fail with `IO_ERROR`. Per-level entries are bounded by `MAX_TREE_ENTRIES`; nesting by `MAX_TREE_DEPTH`.

## ESP32 — ESP-IDF native backend (since 2026-05-26)

The ESP32 branch uses the **ESP-IDF native VFS path** for both SD and flash. Arduino's `SD_MMC.*` and `LittleFS.*` wrappers were retired after they correlated with cross-task / cross-core wedges on HubFX.

### SD card — `esp_vfs_fat_sdmmc_mount`

[esp32/esp_idf_sd_policy.cpp](storage/esp32/esp_idf_sd_policy.cpp) mounts the card via:

```cpp
sdmmc_host_t       host    = SDMMC_HOST_DEFAULT();   // host.slot = SDMMC_HOST_SLOT_1
sdmmc_slot_config_t slot   = SDMMC_SLOT_CONFIG_DEFAULT();
slot.width = 4;
slot.clk = ...; slot.cmd = ...; slot.d0..d3 = ...;
slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
    .format_if_mount_failed = true,
    .max_files              = 5,
    .allocation_unit_size   = 0,
};

sdmmc_card_t* card = nullptr;
esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &card);
```

After mount, **all file ops are POSIX** (`fopen`, `fread`, `fwrite`, `fseek`, `opendir`, `readdir`, `stat`, `mkdir`, `unlink`, `rmdir`) against paths under `/sdcard/...`. `NativeFile` wraps `FILE*` / `DIR*` with move-only RAII semantics.

### Flash — `esp_vfs_littlefs_register`

`FlashModule` (ESP32 branch in [storage/flash.cpp](storage/flash.cpp)) calls:

```cpp
esp_vfs_littlefs_conf_t conf = {
    .base_path              = "/littlefs",
    .partition_label        = "littlefs",  // matches partitions.csv
    .format_if_mount_failed = true,
    .dont_mount             = false,
};
esp_vfs_littlefs_register(&conf);
```

Same POSIX surface as SD, just rooted at `/littlefs/...`. The partition layout (LittleFS subtype) is unchanged from before the rewrite — no data migration required.

### Why not raw `esp_flash.h` / `esp_partition.h`?

That's the layer **underneath** `esp_vfs_littlefs_register`. Going one level down would mean either reimplementing the VFS adapter ourselves or losing POSIX file ops — neither has a payoff. The native VFS layer is exactly the right abstraction.

### Why not Arduino-ESP32 `LittleFS.*` / `SD_MMC.*`?

They wrap `fs::File`, which is a shared-pointer-style refcount wrapper around `FILE*`. Cross-task / cross-core sharing of `fs::File` instances has surfaced races we couldn't easily eliminate. The IDF native path:

- **No `fs::File`** — `NativeFile` is move-only, deterministic close on dtor.
- **FATFS `FF_FS_REENTRANT=1` (IDF default)** + `esp_littlefs` internal locking — concurrent `fopen`/`fread`/`fwrite` from Core 0 and Core 1 is safe at the VFS layer.
- **Mount/unmount is a single host-side call** — no Arduino global-state shuffling.

## Thread Safety

| Operation Type | Lock | Who Holds It |
|----------------|------|-------------- |
| `listDirectory`, `listTree` | Auto (internal) | Module acquires/releases |
| `getFileInfo`, `getStorageInfo` | Auto (internal) | Module acquires/releases |
| `removeFile`, `removeDirectory`, `makeDirectory` | Auto (internal) | Module acquires/releases |
| `openRead`, `openWrite` + subsequent I/O | **Manual** | Caller must `lock()`/`unlock()` around the open + read/write + close sequence |

On ESP32, VFS-FAT (`FF_FS_REENTRANT=1`) and `esp_littlefs` provide their **own internal mutex** for per-call atomicity. The `SfxMutex` in each module is therefore a coarser gate, used for:

1. **Mount / unmount / `retryInit()`** — must serialize against any other op.
2. **Upload exclusivity** (Rule 28) — the mutex doubles as the "an upload is in flight, other subsystems back off" signal for the storage server.
3. **`openRead`/`openWrite` + file ops** — caller-side contract preserved so all existing call sites (audio mixer, config store, storage server) remain unchanged.

On Pico, the platform `SfxMutex` is the only mechanism — no underlying reentrant FATFS.

## SdCardModule — Policy Contract

`SdCardModuleT<TPolicy>` is a state-machine template; `TPolicy` injects the backend. Current policies:

| Platform | Policy | Bus | File Type | Backend |
|----------|--------|-----|-----------|---------|
| **Pico** | `PicoSpiSdPolicy` | SPI | `SdFat::File32` | `<SdFat.h>` |
| **ESP32** | `EspIdfSdio4BitPolicy` | 4-bit SDIO | `NativeFile` | ESP-IDF VFS-FAT |

A policy must provide:

```cpp
struct Config { ... };               // pin assignments + flags
using FileHandle = ...;              // SdFat::File32 or NativeFile
static constexpr SdBusMode BUS_MODE = ...;

bool mount(const Config& cfg);
void unmount();
SdCardType cardType();
void       fillStorageInfo(StorageInfo& info);

FileHandle openDir       (const char* path);
FileHandle openReadFile  (const char* path);
FileHandle openWriteFile (const char* path, bool truncate);
static FileHandle nextFile(FileHandle& dir);

bool exists    (const char* path);
bool removeFile(const char* path);
bool makeDir   (const char* path);
bool removeDir (const char* path);

static bool     isValid     (const FileHandle& f);
static bool     isDirectory (FileHandle& f);
static uint32_t fileSize    (FileHandle& f);
static void     closeFile   (FileHandle& f);
static void     extractName (FileHandle& f, char* buf, size_t len);
```

The legacy `EspSpiSdPolicy` / `EspSdio1BitPolicy` / `EspSdio4BitPolicy` Arduino-based policies were removed in the 2026-05-26 native rewrite. If ESP32 SPI SD is ever needed again, write an `EspIdfSpiSdPolicy` using `sdspi_host_init` + `esp_vfs_fat_sdspi_mount` — same POSIX-over-VFS pattern, not the Arduino shim.

## SdCardModule — Usage

### Lifecycle

```cpp
SdCardModule& sd = SdCardModule::instance();
SdCardModule::Config cfg {
    .clk = 39, .cmd = 38,
    .d0  = 40, .d1  = 41, .d2 = 42, .d3 = 45,
};
sd.begin(cfg);
```

### Retry

```cpp
sd.config().speed_mhz = 40;          // SDMMC_FREQ_HIGHSPEED (40 MHz)
sd.retryInit();                       // unmount + re-mount with stored config
```

### File I/O (caller holds the lock)

```cpp
sd.lock();
StorageFile file;
if (sd.openRead("/sounds/foo.wav", file) == SdError::OK) {
    uint8_t buf[256];
    while (file.available()) {
        int n = file.read(buf, sizeof(buf));
        // ...
    }
    file.close();
}
sd.unlock();
```

### Error Codes (`SdError`)

| Code | Name |
|------|------|
| 0 | OK |
| 1 | NOT_INITIALIZED |
| 2 | NOT_FOUND |
| 3 | IO_ERROR |
| 4 | IS_DIRECTORY |
| 5 | ALREADY_EXISTS |
| 6 | LIMIT_EXCEEDED (`MAX_TREE_DEPTH` / `MAX_TREE_ENTRIES`) |

### StorageInfo

```cpp
struct StorageInfo {
    bool initialized;
    uint32_t cardSize_MB;
    uint32_t totalSpace_MB;
    uint32_t usedSpace_MB;
    uint32_t freeSpace_MB;
    uint8_t  fatType;             // FS_FAT12 / FAT16 / FAT32 / EXFAT (from FATFS)
    uint32_t clusterSize_bytes;
    SdBusMode busMode;
    SdCardType cardType;
};
```

## FlashModule — Usage

### Lifecycle

```cpp
FlashModule& flash = FlashModule::instance();
flash.begin();   // ESP32: esp_vfs_littlefs_register("/littlefs", "littlefs", ...)
                 // Pico:  LittleFS.begin()
```

### Storage Info

```cpp
struct FlashStorageInfo {
    bool initialized;
    uint32_t totalBytes;
    uint32_t usedBytes;
    uint32_t freeBytes;
};
```

### Error Codes — same values as `SdError`.

## File Handle Type — `StorageFile`

The storage server's upload buffer uses a single file-handle type that resolves per platform:

```cpp
#if SFX_PLATFORM_ESP32
    using StorageFile = NativeFile;     // POSIX RAII over VFS
#else
    using StorageFile = ::File;         // Arduino LittleFS File
#endif
```

The audio mixer's SD-specific handle is `SdFile` (= `SdCardModule::FileHandle`), which is `NativeFile` on ESP32. Both names point at the same type on ESP32; they exist to clarify intent at the call site (storage-server vs SD-direct).

## Dependencies

| Dependency | Platform | Reason |
|------------|----------|--------|
| `sfx_platform` | All | `sfx_platform.h` (SfxMutex, platform macros) |
| `sfx_serial` | All | `diag_log.h` (logging) |
| `Arduino` | All | base types (only) |
| `SdFat` | Pico only | SD card backend. Excluded on ESP32 via `lib_ignore`. |
| `<esp_vfs_fat.h>` + `<driver/sdmmc_host.h>` + `<sdmmc_cmd.h>` | ESP32 only | Bundled with Arduino-ESP32 framework — no `lib_deps` change needed |
| `<esp_littlefs.h>` | ESP32 only | Bundled with Arduino-ESP32 framework (`libjoltwallet__littlefs.a`) |
| `<LittleFS.h>` | Pico only | Arduino LittleFS wrapper |

**No dependency on:** sfx_audio, sfx_usb, sfx_peripherals, sfx_server.
