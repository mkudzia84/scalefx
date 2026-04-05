"""HubFX command builders (slave management, audio, engine, config, storage)."""

import struct

from ._common import (
    build_packet, parse_packet, u16_le, u32_le,
    _warn_range, _warn_u16,
    HubFxPacket, HubFxAudio, HubFxStorage,
)
from .core import CommandBuilder


class HubFxCommands(CommandBuilder):
    """HubFX-specific commands (slave management, slave routing, audio, engine, config, SD)."""

    # =========================================================================
    # Slave Routing (subcmd pattern)
    # =========================================================================

    @staticmethod
    def slave_route(slave_packet: bytes) -> bytes:
        """
        Wrap a pre-built slave command packet in the appropriate SLAVE_ROUTE_*
        hub routing packet.

        Unpacks the slave packet to extract (packet_type, payload), determines
        the target slave from the packet type range, and wraps it as:
            SLAVE_ROUTE_xxx [subcmd:u8][payload...]

        This allows reusing existing command builders (GunFxCommands,
        LightFxCommands, GearControlCommands) for hub-routed commands.

        Args:
            slave_packet: A fully COBS-encoded slave command packet
                          (as returned by GunFxCommands.*, etc.)

        Returns:
            COBS-encoded SLAVE_ROUTE_* packet

        Raises:
            ValueError: If packet cannot be parsed or slave type unknown
        """
        parsed = parse_packet(slave_packet)
        if parsed is None:
            raise ValueError("Cannot parse slave packet for routing")

        pkt_type, _tag, payload = parsed

        # Determine routing packet type from the slave's packet type range
        if 0x01 <= pkt_type <= 0x2F:
            route_type = HubFxPacket.SLAVE_ROUTE_GUNFX
        elif 0x40 <= pkt_type <= 0x5F:
            route_type = HubFxPacket.SLAVE_ROUTE_LIGHTFX
        elif 0x60 <= pkt_type <= 0x7F:
            route_type = HubFxPacket.SLAVE_ROUTE_GEARCONTROL
        else:
            raise ValueError(f"Unknown slave packet type range: 0x{pkt_type:02X}")

        return build_packet(route_type, bytes([pkt_type]) + payload)

    @staticmethod
    def slave_route_gunfx(subcmd: int, payload: bytes = b'') -> bytes:
        """
        Route a command to the GunFX slave via the hub.

        Wire format: SLAVE_ROUTE_GUNFX [subcmd:u8][payload...]

        Args:
            subcmd: Original GunFX packet type byte (e.g., GunFxPacket.TRIGGER_ON)
            payload: Original command payload
        """
        return build_packet(HubFxPacket.SLAVE_ROUTE_GUNFX, bytes([subcmd]) + payload)

    @staticmethod
    def slave_route_lightfx(subcmd: int, payload: bytes = b'') -> bytes:
        """
        Route a command to the LightFX slave via the hub.

        Args:
            subcmd: Original LightFX packet type byte
            payload: Original command payload
        """
        return build_packet(HubFxPacket.SLAVE_ROUTE_LIGHTFX, bytes([subcmd]) + payload)

    @staticmethod
    def slave_route_gearcontrol(subcmd: int, payload: bytes = b'') -> bytes:
        """
        Route a command to the GearControl slave via the hub.

        Args:
            subcmd: Original GearControl packet type byte
            payload: Original command payload
        """
        return build_packet(HubFxPacket.SLAVE_ROUTE_GEARCONTROL, bytes([subcmd]) + payload)

    # =========================================================================
    # Slave Management
    # =========================================================================

    @staticmethod
    def slave_list() -> bytes:
        """
        Request list of known slave controllers.

        Response is SLAVE_LIST_RESP with format:
          [count:u8]
          Per slave × count:
            [type:u8][connected:u8][ready:u8][name_len:u8][name:str]
        """
        return build_packet(HubFxPacket.SLAVE_LIST)

    @staticmethod
    def slave_init(slave_type: int) -> bytes:
        """
        Send INIT to a specific slave controller by type.

        Args:
            slave_type: Slave type (1=GunFX, 2=LightFX, 3=GearControl)
        """
        _warn_range("slave_type", slave_type, 1, 3)
        return build_packet(HubFxPacket.SLAVE_INIT, bytes([slave_type]))

    @staticmethod
    def slave_status() -> bytes:
        """
        Request hub-level status.

        Returns ACK; hub status data comes via core STATUS callback.
        """
        return build_packet(HubFxPacket.SLAVE_STATUS)

    @staticmethod
    def slave_info(slave_type: int) -> bytes:
        """
        Request cached board info for a specific slave controller.

        Returns SLAVE_INFO_RESP with format:
          [slaveType:u8][ready:u8][connected:u8]
          [nameLen:u8][name:str][verLen:u8][ver:str][platLen:u8][plat:str]
          [cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]

        Args:
            slave_type: Slave type (1=GunFX, 2=LightFX, 3=GearControl)
        """
        _warn_range("slave_type", slave_type, 1, 3)
        return build_packet(HubFxPacket.SLAVE_INFO, bytes([slave_type]))

    @staticmethod
    def usb_devices() -> bytes:
        """
        Request USB host device list.

        Response is USB_DEVICES_RESP with format:
          [initialized:u8][taskRunning:u8]
          [backendLen:u8][backend:str]
          [deviceCount:u8]
          Per device × count:
            [addr:u8][vid:u16LE][pid:u16LE][state:u8][slaveType:u8]
        """
        return build_packet(HubFxPacket.USB_DEVICES_REQ)

    @staticmethod
    def usb_reset_bus() -> bytes:
        """
        Power-cycle the USB root port to force re-enumeration.

        This disconnects the hub and all downstream devices momentarily,
        then re-enumerates everything.  Useful when a device fails to
        enumerate after hot-plug (ext_port driver port-disable bug).

        Returns ACK on success.
        """
        return build_packet(HubFxPacket.USB_RESET_BUS)

    # =========================================================================
    # Audio Control
    # =========================================================================

    @staticmethod
    def audio_play(channel: int, path: str, volume: int = 100,
                   output: int = HubFxAudio.OUTPUT_ALL,
                   loop_mode: int = HubFxAudio.LOOP_NONE,
                   loop_count: int = 0) -> bytes:
        """
        Play audio file on a channel.

        Args:
            channel: Audio channel (0-7)
            path: File path on SD card (e.g., "/sounds/fire.wav")
            volume: Volume percentage (0-100)
            output: Output channel bitmask (CH1=0x01, CH2=0x02, ALL=0x03)
            loop_mode: LOOP_NONE=0, LOOP_FINITE=1, LOOP_INFINITE=2
            loop_count: Number of loops (for LOOP_FINITE)
        """
        _warn_range("channel", channel, 0, 7)
        _warn_range("volume", volume, 0, 100)
        path_bytes = path.encode('utf-8')
        payload = bytes([channel, volume, output, loop_mode])
        payload += u16_le(loop_count)
        payload += bytes([len(path_bytes)]) + path_bytes
        return build_packet(HubFxPacket.AUDIO_PLAY, payload)

    @staticmethod
    def audio_stop(channel: int = 0xFF) -> bytes:
        """
        Stop audio playback.

        Args:
            channel: Audio channel (0-7) or 0xFF for all channels
        """
        return build_packet(HubFxPacket.AUDIO_STOP, bytes([channel]))

    @staticmethod
    def audio_volume(channel: int, volume: int) -> bytes:
        """
        Set audio volume.

        Args:
            channel: Audio channel (0-7) or 0xFF for master volume
            volume: Volume percentage (0-100)
        """
        _warn_range("volume", volume, 0, 100)
        return build_packet(HubFxPacket.AUDIO_VOLUME, bytes([channel, volume]))

    @staticmethod
    def audio_fade(channel: int) -> bytes:
        """
        Fade out an audio channel.

        Args:
            channel: Audio channel (0-7)
        """
        _warn_range("channel", channel, 0, 7)
        return build_packet(HubFxPacket.AUDIO_FADE, bytes([channel]))

    @staticmethod
    def audio_queue(channel: int, path: str, volume: int = 100,
                    loop_count: int = 0,
                    behavior: int = HubFxAudio.QUEUE_FINISH_LOOP) -> bytes:
        """
        Queue a sound to play after the current one finishes.

        Args:
            channel: Audio channel (0-7)
            path: File path on SD card
            volume: Volume percentage (0-100)
            loop_count: Number of loops (0 = play once)
            behavior: QUEUE_FINISH_LOOP=0, QUEUE_STOP_NOW=1
        """
        _warn_range("channel", channel, 0, 7)
        _warn_range("volume", volume, 0, 100)
        path_bytes = path.encode('utf-8')
        payload = bytes([channel, volume])
        payload += u16_le(loop_count)
        payload += bytes([behavior, len(path_bytes)]) + path_bytes
        return build_packet(HubFxPacket.AUDIO_QUEUE, payload)

    @staticmethod
    def audio_queue_clear(channel: int = 0xFF) -> bytes:
        """
        Clear the audio queue for a channel or all channels.

        Args:
            channel: Audio channel (0-7) or 0xFF for all
        """
        return build_packet(HubFxPacket.AUDIO_QUEUE_CLEAR, bytes([channel]))

    @staticmethod
    def audio_status() -> bytes:
        """
        Request audio mixer status.

        Response is AUDIO_STATUS_RESP with format:
          [masterVol:u8][activeMask:u8]
          Per active channel:
            [ch:u8][vol:u8][playing:u8][looping:u8]
            [loopCount:u16LE][remaining_ms:u16LE][queueLen:u8][output:u8]
        """
        return build_packet(HubFxPacket.AUDIO_STATUS_REQ)

    @staticmethod
    def codec_status() -> bytes:
        """
        Request codec hardware status.

        Response is CODEC_STATUS_RESP with format:
          [codecType:u8][initialized:u8][i2cOk:u8][sdaPin:u8][sclPin:u8]
          [supplyVoltage:u8][muted:u8][digitalVol:u8][deviceCtrl:u8][faultStatus:u8]
          [codecNameLen:u8][codecName:str]
        """
        return build_packet(HubFxPacket.CODEC_STATUS_REQ)

    # =========================================================================
    # Engine FX Control
    # =========================================================================

    @staticmethod
    def engine_start() -> bytes:
        """Start engine effects (force start)."""
        return build_packet(HubFxPacket.ENGINE_START)

    @staticmethod
    def engine_stop() -> bytes:
        """Stop engine effects (force stop)."""
        return build_packet(HubFxPacket.ENGINE_STOP)

    @staticmethod
    def engine_status() -> bytes:
        """
        Request engine FX status.

        Response is ENGINE_STATUS_RESP:
          [state:u8][toggleEngaged:u8][active:u8]
        """
        return build_packet(HubFxPacket.ENGINE_STATUS_REQ)

    # =========================================================================
    # Config Management
    # =========================================================================

    @staticmethod
    def config_reload(path: str = None) -> bytes:
        """Reload configuration from file (default: /config.yaml)."""
        if path:
            path_bytes = path.encode('utf-8')
            payload = struct.pack('<B', len(path_bytes)) + path_bytes
            return build_packet(HubFxPacket.CONFIG_RELOAD, payload)
        return build_packet(HubFxPacket.CONFIG_RELOAD)

    @staticmethod
    def config_status() -> bytes:
        """
        Get configuration status.

        Response is CONFIG_STATUS_RESP:
          [loaded:u8][size:u16LE][validOk:u8]
        """
        return build_packet(HubFxPacket.CONFIG_STATUS)

    @staticmethod
    def config_save(path: str = None) -> bytes:
        """Save current config to flash (default: /config.yaml)."""
        if path:
            path_bytes = path.encode('utf-8')
            payload = struct.pack('<B', len(path_bytes)) + path_bytes
            return build_packet(HubFxPacket.CONFIG_SAVE, payload)
        return build_packet(HubFxPacket.CONFIG_SAVE)

    # =========================================================================
    # SD Card Management
    # =========================================================================

    @staticmethod
    def sd_init(speed_mhz: int = 20) -> bytes:
        """
        Initialize or re-initialize the SD card.

        Args:
            speed_mhz: SPI clock speed in MHz (1-50, default 20)
        """
        _warn_range("speed_mhz", speed_mhz, 1, 50, "MHz")
        return build_packet(HubFxPacket.SD_INIT, bytes([speed_mhz]))

    @staticmethod
    def sd_status() -> bytes:
        """
        Request SD card status.

        Response is SD_STATUS_RESP:
          [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]
        """
        return build_packet(HubFxPacket.SD_STATUS_REQ)

    @staticmethod
    def flash_status() -> bytes:
        """
        Request onboard flash (LittleFS) status.

        Response is FLASH_STATUS_REQ (same packet type for req/resp):
          [initialized:u8][totalBytes:u32LE][usedBytes:u32LE][freeBytes:u32LE]
        """
        return build_packet(HubFxPacket.FLASH_STATUS_REQ)

    # =========================================================================
    # File Operations
    # =========================================================================

    @staticmethod
    def _path_payload(path: str, target: int = 0) -> bytes:
        """Build [pathLen:u8][path:str][target:u8?] payload."""
        path_bytes = path.encode('utf-8')
        payload = bytes([len(path_bytes)]) + path_bytes
        if target != HubFxStorage.TARGET_SD:
            payload += bytes([target])
        return payload

    @staticmethod
    def file_list(path: str = "/", target: int = 0) -> bytes:
        """
        List directory contents.

        Response is streamed: STREAM_BEGIN → STREAM_DATA chunks → STREAM_END.
        Content is POSIX-like text listing.

        Args:
            path: Directory path (e.g., "/", "/sounds")
            target: Storage target (0=SD, 1=Flash)
        """
        return build_packet(HubFxPacket.FILE_LIST, HubFxCommands._path_payload(path, target))

    @staticmethod
    def file_tree(path: str = "/", target: int = 0) -> bytes:
        """
        Recursive directory tree listing.

        Response is streamed: STREAM_BEGIN → STREAM_DATA chunks → STREAM_END.
        Content is structured text: "<depth> <d|f> <name> <size>" per line.
        CLI renders as POSIX-style tree output.

        Args:
            path: Root directory path (e.g., "/", "/sounds")
            target: Storage target (0=SD, 1=Flash)
        """
        return build_packet(HubFxPacket.FILE_TREE, HubFxCommands._path_payload(path, target))

    @staticmethod
    def file_delete(path: str, target: int = 0) -> bytes:
        """
        Delete a file.

        Args:
            path: File path to delete
            target: Storage target (0=SD, 1=Flash)
        """
        return build_packet(HubFxPacket.FILE_DELETE, HubFxCommands._path_payload(path, target))

    @staticmethod
    def file_mkdir(path: str, target: int = 0) -> bytes:
        """
        Create a directory (recursive).

        Args:
            path: Directory path to create
            target: Storage target (0=SD, 1=Flash)
        """
        return build_packet(HubFxPacket.FILE_MKDIR, HubFxCommands._path_payload(path, target))

    @staticmethod
    def file_info(path: str, target: int = 0) -> bytes:
        """
        Get file or directory information.

        Response is FILE_INFO_RESP:
          [exists:u8][isDir:u8][size:u32LE]

        Args:
            path: File or directory path
            target: Storage target (0=SD, 1=Flash)
        """
        return build_packet(HubFxPacket.FILE_INFO, HubFxCommands._path_payload(path, target))

    @staticmethod
    def file_download(path: str, target: int = 0) -> bytes:
        """
        Download a file.

        Response is streamed: STREAM_BEGIN → STREAM_DATA chunks → STREAM_END.
        Content is raw file bytes.

        Args:
            path: File path to download
            target: Storage target (0=SD, 1=Flash)
        """
        return build_packet(HubFxPacket.FILE_DOWNLOAD, HubFxCommands._path_payload(path, target))

    @staticmethod
    def file_upload_begin(path: str, size: int, target: int = 0,
                          mode: int = 0) -> bytes:
        """
        Begin a file upload.

        Mode 0 (sync): send FILE_UPLOAD_DATA chunks with per-chunk ACK.
        Mode 3 (stream): ACK payload contains
            [segment_size:u32LE][segment_count:u16LE].
            Client sends raw binary in segments, waits for
            FILE_UPLOAD_PROGRESS between each, then FILE_UPLOAD_END.

        Args:
            path: Destination file path
            size: Total file size in bytes
            target: Storage target (0=SD, 1=Flash)
            mode: Upload mode (0=sync, 3=stream)
        """
        path_bytes = path.encode('utf-8')
        payload = u32_le(size) + bytes([len(path_bytes)]) + path_bytes
        if mode != 0:
            # Mode requires target to be present (positional)
            payload += bytes([target, mode])
        elif target != HubFxStorage.TARGET_SD:
            payload += bytes([target])
        return build_packet(HubFxPacket.FILE_UPLOAD_BEGIN, payload)

    @staticmethod
    def file_upload_data(seq_num: int, data: bytes) -> bytes:
        """
        Send an upload data chunk with CRC-16 integrity.

        Server ACKs on success, NACKs with CRC_ERROR for retry.

        Args:
            seq_num: Sequence number (0-based, incrementing)
            data: Chunk data (up to MAX_PAYLOAD_SIZE - 4 bytes)
        """
        from tests.framework.protocol import crc16_ccitt
        crc = crc16_ccitt(data)
        payload = u16_le(seq_num) + u16_le(crc) + data
        return build_packet(HubFxPacket.FILE_UPLOAD_DATA, payload)

    @staticmethod
    def file_upload_end() -> bytes:
        """End a file upload. Server verifies total size and ACKs."""
        return build_packet(HubFxPacket.FILE_UPLOAD_END)

    @staticmethod
    def file_upload_cancel() -> bytes:
        """Cancel an in-progress upload. Server deletes partial file."""
        return build_packet(HubFxPacket.FILE_UPLOAD_CANCEL)
