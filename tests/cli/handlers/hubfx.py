"""
HubFX Command Handlers

HubFX-specific CLI commands:
- Slave management (list, init, status)
- Audio control (play, stop, volume, fade, queue, status)
- Engine FX control (start, stop, status)
- Config management (reload, get)
- SD card management (init, status)
- Flash management (status)
- File operations (ls, rm, mkdir, info, download, upload, cat) -- SD and flash targets, recursive for directories
- Passthrough commands to slave controllers via hub routing (subcmd pattern)

When connected to HubFX, all GunFX/LightFX/GearControl commands are
wrapped in SLAVE_ROUTE_* packets using the subcmd pattern:
    SLAVE_ROUTE_xxx [subcmd:u8][original_payload...]
The hub extracts the subcmd and forwards to the appropriate slave.
"""""

import os
import struct
from typing import List, Dict, Tuple, Callable, Optional

from tests.framework import (
    HubFxCommands, HubFxPacket, HubFxError, HubFxAudio, HubFxStorage, EngineState, SlaveType,
    CoreError, StreamPacket,
)
from tests.framework.packets import known_device_name
from tests.framework.protocol import read_u16_le, read_u32_le, crc16_ccitt
from ..base import CommandHandlerBase, CommandInfo, ControllerType, Fore, Style, Spinner, format_progress_bar
from .. import parsers
from .storage import StorageHandler
from .gunfx import GunFxCommandHandler
from .lightfx import LightFxCommandHandler
from .gearcontrol import GearControlCommandHandler


class HubFxCommandHandler(CommandHandlerBase):
    """
    Handler for HubFX-specific commands.

    These commands are only available when connected to a HubFX controller.
    They manage the slave controller registry and hub routing.

    Additionally, this handler exposes passthrough commands for all slave
    controller types -- the hub routes them transparently.

    Feature gating: HubFX subsystems (audio, SD, USB) may be inactive.
    The CLI tracks which features are active and annotates help accordingly.
    Commands for inactive features are still executable (firmware will NACK
    if truly not ready) but dimmed in help output.
    """

    # Group -> required feature mapping.
    # Groups not listed here are always available (e.g., Flash Storage).
    GROUP_FEATURES = {
        'Audio': 'audio',
        'Engine FX': 'audio',
        'Config': 'sd',
        'SD Card & Files': 'sd',
        'Hub Management': 'usb',
        'Slave Controllers': 'usb',
    }

    # Slave sub-group -> required feature mapping.
    SLAVE_GROUP_FEATURES = {
        'GunFX (gfx.*)': 'slave:gunfx',
        'LightFX (lfx.*)': 'slave:lightfx',
        'GearControl (gc.*)': 'slave:gearcontrol',
    }

    # Human-readable descriptions for feature requirements.
    FEATURE_LABELS = {
        'audio': 'audio not initialized',
        'sd': 'SD card not ready',
        'usb': 'USB host not active',
        'slave:gunfx': 'GunFX not connected',
        'slave:lightfx': 'LightFX not connected',
        'slave:gearcontrol': 'GearControl not connected',
    }

    def __init__(self):
        super().__init__()
        self._active_features: dict = {}  # feature name -> bool

        # Composed slave handlers -- reuse direct handler implementations
        # with packet wrapper for transparent hub slave routing
        self._gunfx = GunFxCommandHandler()
        self._gunfx._packet_wrapper = HubFxCommands.slave_route
        self._lightfx = LightFxCommandHandler()
        self._lightfx._packet_wrapper = HubFxCommands.slave_route
        self._gearcontrol = GearControlCommandHandler()
        self._gearcontrol._packet_wrapper = HubFxCommands.slave_route
        self._slave_handlers = [self._gunfx, self._lightfx, self._gearcontrol]

        self._sd_storage = StorageHandler(
            "sd", HubFxStorage.TARGET_SD, "SD",
            "SD Card & Files", ControllerType.HUBFX)
        self._flash_storage = StorageHandler(
            "flash", HubFxStorage.TARGET_FLASH, "flash",
            "Flash Storage", ControllerType.HUBFX)

    def set_connection(self, conn):
        """Propagate connection to all composed handlers."""
        super().set_connection(conn)
        for h in self._slave_handlers:
            h.set_connection(conn)
        self._sd_storage.set_connection(conn)
        self._flash_storage.set_connection(conn)

    def set_cancel_event(self, event):
        """Propagate cancel event to all composed handlers."""
        super().set_cancel_event(event)
        for h in self._slave_handlers:
            h.set_cancel_event(event)
        self._sd_storage.set_cancel_event(event)
        self._flash_storage.set_cancel_event(event)

    def set_controller_type(self, ctrl_type):
        """Propagate controller type to all composed handlers."""
        super().set_controller_type(ctrl_type)
        for h in self._slave_handlers:
            h.set_controller_type(ctrl_type)
        self._sd_storage.set_controller_type(ctrl_type)
        self._flash_storage.set_controller_type(ctrl_type)

    def set_active_features(self, features: dict):
        """Update active feature flags from STATUS response."""
        self._active_features = dict(features)

    def is_feature_active(self, feature: str) -> bool:
        """Check if a HubFX feature is currently active."""
        return self._active_features.get(feature, False)

    @property
    def active_features(self) -> dict:
        """Get current feature flags (copy)."""
        return dict(self._active_features)

    @property
    def has_features(self) -> bool:
        """True if feature flags have been fetched at least once."""
        return len(self._active_features) > 0

    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return HubFX command registry."""
        cmds = {
            # =================================================================
            # Hub Management
            # =================================================================
            'hub.slaves': (self.cmd_slave_list, CommandInfo(
                'hub.slaves', 'hub.slaves',
                'List connected slave controllers',
                requires_init=True, controller=ControllerType.HUBFX, group='Hub Management')),
            'hub.init': (self.cmd_slave_init, CommandInfo(
                'hub.init', 'hub.init <type>',
                'Init a slave (gunfx|lightfx|gearcontrol or 1|2|3)',
                requires_init=True, controller=ControllerType.HUBFX, group='Hub Management')),
            'hub.usb': (self.cmd_usb_devices, CommandInfo(
                'hub.usb', 'hub.usb',
                'List USB host devices (CDC ports, VID/PID, state)',
                requires_init=True, controller=ControllerType.HUBFX, group='Hub Management')),

            # =================================================================
            # Audio Control
            # =================================================================
            'audio.play': (self.cmd_audio_play, CommandInfo(
                'audio.play', 'audio.play <ch> <path> [vol] [left|right] [loop [N|inf]]',
                'Play audio file on channel',
                requires_init=True, controller=ControllerType.HUBFX, group='Audio')),
            'audio.stop': (self.cmd_audio_stop, CommandInfo(
                'audio.stop', 'audio.stop [ch|all]',
                'Stop audio (channel or all)',
                requires_init=True, controller=ControllerType.HUBFX, group='Audio')),
            'audio.volume': (self.cmd_audio_volume, CommandInfo(
                'audio.volume', 'audio.volume <ch|master> <0-100>',
                'Set channel or master volume',
                requires_init=True, controller=ControllerType.HUBFX, group='Audio')),
            'audio.fade': (self.cmd_audio_fade, CommandInfo(
                'audio.fade', 'audio.fade <ch>',
                'Fade out audio channel',
                requires_init=True, controller=ControllerType.HUBFX, group='Audio')),
            'audio.queue': (self.cmd_audio_queue, CommandInfo(
                'audio.queue', 'audio.queue <ch> <path> [vol] [loop N]',
                'Queue sound to play after current',
                requires_init=True, controller=ControllerType.HUBFX, group='Audio')),
            'audio.clear': (self.cmd_audio_queue_clear, CommandInfo(
                'audio.clear', 'audio.clear [ch|all]',
                'Clear audio queue',
                requires_init=True, controller=ControllerType.HUBFX, group='Audio')),
            'audio.status': (self.cmd_audio_status, CommandInfo(
                'audio.status', 'audio.status',
                'Show audio mixer status',
                requires_init=True, controller=ControllerType.HUBFX, group='Audio')),

            # =================================================================
            # Engine FX
            # =================================================================
            'engine.start': (self.cmd_engine_start, CommandInfo(
                'engine.start', 'engine.start',
                'Start engine effects',
                requires_init=True, controller=ControllerType.HUBFX, group='Engine FX')),
            'engine.stop': (self.cmd_engine_stop, CommandInfo(
                'engine.stop', 'engine.stop',
                'Stop engine effects',
                requires_init=True, controller=ControllerType.HUBFX, group='Engine FX')),
            'engine.status': (self.cmd_engine_status, CommandInfo(
                'engine.status', 'engine.status',
                'Show engine FX status',
                requires_init=True, controller=ControllerType.HUBFX, group='Engine FX')),

            # =================================================================
            # Config Management
            # =================================================================
            'config.reload': (self.cmd_config_reload, CommandInfo(
                'config.reload', 'config.reload',
                'Reload config from SD card',
                requires_init=True, controller=ControllerType.HUBFX, group='Config')),
            'config.get': (self.cmd_config_get, CommandInfo(
                'config.get', 'config.get',
                'Get config info (loaded, size)',
                requires_init=True, controller=ControllerType.HUBFX, group='Config')),

            # =================================================================
            # SD Card & Files
            # =================================================================
            'sd.init': (self.cmd_sd_init, CommandInfo(
                'sd.init', 'sd.init',
                'Remount SD card (unmounts first, then re-initializes)',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.status': (self.cmd_sd_status, CommandInfo(
                'sd.status', 'sd.status',
                'Show SD card status',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),

            # =================================================================
            # Flash Storage
            # =================================================================
            'flash.status': (self.cmd_flash_status, CommandInfo(
                'flash.status', 'flash.status',
                'Show onboard flash status',
                requires_init=True, controller=ControllerType.HUBFX, group='Flash Storage')),

            # =================================================================
            # Slave Controllers (routed via hub)
            # =================================================================
            'slave': (self.cmd_slave, CommandInfo(
                'slave', 'slave <gfx|lfx|gc>.<cmd> [args...]',
                'Route command to slave controller (help gfx|lfx|gc for details)',
                requires_init=True, controller=ControllerType.HUBFX, group='Slave Controllers')),
        }

        # Merge StorageHandler commands (sd.ls, sd.upload, flash.ls, etc.)
        cmds.update(self._sd_storage.get_commands())
        cmds.update(self._flash_storage.get_commands())

        return cmds

    # =========================================================================
    # Slave Command Dispatcher
    # =========================================================================

    def get_slave_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return slave sub-command registry for help display and dispatch."""
        if not hasattr(self, '_slave_cache'):
            self._slave_cache = self._build_slave_registry()
        return self._slave_cache

    def _build_slave_registry(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Build slave sub-command dispatch table from composed handlers.

        Reuses the direct handler command methods (GunFxCommandHandler,
        LightFxCommandHandler, GearControlCommandHandler) with _packet_wrapper
        set to HubFxCommands.slave_route for transparent hub routing.

        Query commands that require custom response handling are excluded
        since SLAVE_ROUTE only forwards ACK/NACK responses.
        """
        GFX = 'GunFX (gfx.*)'
        LFX = 'LightFX (lfx.*)'
        GC = 'GearControl (gc.*)'

        # Commands that require custom response packets (not ACK-based)
        # and can't be routed through SLAVE_ROUTE
        EXCLUDE = {'lfx.led.seq.status', 'lfx.led.seq.queue', 'lfx.led.status'}

        GROUPS = [
            (self._gunfx, GFX),
            (self._lightfx, LFX),
            (self._gearcontrol, GC),
        ]

        registry = {}
        for handler, group in GROUPS:
            for name, (method, info) in handler.get_commands().items():
                if name in EXCLUDE:
                    continue
                slave_info = CommandInfo(
                    name, f'slave {info.usage}', info.description, group=group)
                registry[name] = (method, slave_info)

        return registry

    def cmd_slave(self, args: List[str]):
        """Route a command to a slave controller via hub."""
        if not self._require_init():
            return

        if not args:
            self.print_error("Usage: slave <gfx|lfx|gc>.<cmd> [args...]")
            self.print_info("  Try: help gfx, help lfx, help gc")
            return

        subcmd = args[0]
        rest = args[1:]
        registry = self.get_slave_commands()

        if subcmd in registry:
            handler, _info = registry[subcmd]
            handler(rest)
        else:
            # Check if it's a valid prefix
            prefix = subcmd.split('.')[0] if '.' in subcmd else subcmd
            matching = [k for k in registry if k.startswith(prefix + '.') or k == prefix]
            if matching:
                self.print_error(f"Unknown sub-command: {subcmd}")
                self.print_info(f"  Available {prefix}.* commands: {', '.join(sorted(matching))}")
            else:
                self.print_error(f"Unknown slave command: {subcmd}")
                self.print_info("  Prefixes: gfx.* (GunFX), lfx.* (LightFX), gc.* (GearControl)")

    # =========================================================================
    # Hub Management Commands
    # =========================================================================

    def cmd_slave_list(self, args: List[str]):
        """List connected slave controllers."""
        if not self._require_init():
            return

        packet = HubFxCommands.slave_list()
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return

        if response.is_nack:
            code = response.error_code
            name = HubFxError.name(code)
            self.print_error(f"NACK: {name} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.SLAVE_LIST_RESP:
            self._parse_slave_list(response.payload)
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def _parse_slave_list(self, payload: bytes):
        """Parse and display SLAVE_LIST_RESP payload."""
        if len(payload) < 1:
            self.print_error("Empty slave list response")
            return

        count = payload[0]
        print(f"\n  {Fore.YELLOW}Slave Controllers ({count}):{Style.RESET_ALL}")

        if count == 0:
            print(f"  {Fore.YELLOW}(no slaves registered){Style.RESET_ALL}")
            return

        pos = 1
        for i in range(count):
            if pos + 4 > len(payload):
                break

            stype = payload[pos]
            connected = payload[pos + 1]
            ready = payload[pos + 2]
            name_len = payload[pos + 3]
            pos += 4

            name = ""
            if name_len > 0 and pos + name_len <= len(payload):
                name = payload[pos:pos + name_len].decode('utf-8', errors='replace')
                pos += name_len

            type_name = SlaveType.name(stype)
            status_color = Fore.GREEN if ready else (Fore.YELLOW if connected else Fore.RED)
            status_text = "ready" if ready else ("connected" if connected else "disconnected")

            display_name = f" ({name})" if name else ""
            print(f"    [{i}] {type_name}{display_name}: "
                  f"{status_color}{status_text}{Style.RESET_ALL}")

        print()

    def cmd_slave_init(self, args: List[str]):
        """Init a slave controller by type."""
        if not self._require_init():
            return

        if not args:
            self.print_error("Usage: hub.init <type> (gunfx|lightfx|gearcontrol or 1|2|3)")
            return

        # Parse slave type
        type_str = args[0].lower()
        type_map = {
            'gunfx': SlaveType.GUNFX, '1': SlaveType.GUNFX,
            'lightfx': SlaveType.LIGHTFX, '2': SlaveType.LIGHTFX,
            'gearcontrol': SlaveType.GEARCONTROL, '3': SlaveType.GEARCONTROL,
        }
        slave_type = type_map.get(type_str)
        if slave_type is None:
            self.print_error(f"Unknown slave type: {type_str}")
            self.print_info("Valid types: gunfx (1), lightfx (2), gearcontrol (3)")
            return

        self.print_info(f"Initializing {SlaveType.name(slave_type)} slave...")
        packet = HubFxCommands.slave_init(slave_type)
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)

        if success:
            self.print_ok(f"{SlaveType.name(slave_type)} slave initialized")
        elif response is not None:
            code = response.error_code
            name = HubFxError.name(code)
            self.print_error(f"Init failed: {name} (0x{code:02X})")
        else:
            self.print_error("No response (timeout)")

    def cmd_usb_devices(self, args: List[str]):
        """List USB host devices."""
        if not self._require_init():
            return

        packet = HubFxCommands.usb_devices()
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return

        if response.is_nack:
            code = response.error_code
            name = HubFxError.name(code)
            self.print_error(f"NACK: {name} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.USB_DEVICES_RESP:
            self._parse_usb_devices(response.payload)
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def _parse_usb_devices(self, payload: bytes):
        """Parse and display USB_DEVICES_RESP payload."""
        if len(payload) < 4:
            self.print_error("USB devices response too short")
            return

        pos = 0
        initialized = payload[pos]; pos += 1
        task_running = payload[pos]; pos += 1
        backend_len = payload[pos]; pos += 1

        if pos + backend_len > len(payload):
            self.print_error("Malformed USB devices response")
            return

        backend = payload[pos:pos + backend_len].decode('utf-8', errors='replace')
        pos += backend_len

        if pos >= len(payload):
            self.print_error("Malformed USB devices response")
            return

        device_count = payload[pos]; pos += 1

        # Status display
        init_color = Fore.GREEN if initialized else Fore.RED
        init_text = "initialized" if initialized else "not initialized"
        task_color = Fore.GREEN if task_running else Fore.RED
        task_text = "running" if task_running else "stopped"

        print(f"\n  {Fore.YELLOW}USB Host ({backend}):{Style.RESET_ALL}")
        print(f"    Status: {init_color}{init_text}{Style.RESET_ALL}, "
              f"Task: {task_color}{task_text}{Style.RESET_ALL}")
        print(f"    CDC Devices: {device_count}")

        if device_count == 0:
            print(f"    {Fore.YELLOW}(no USB devices connected){Style.RESET_ALL}")
            print()
            return

        state_names = {0: "Disconnected", 1: "Connected", 2: "Mounted", 3: "Ready"}

        for i in range(device_count):
            if pos + 7 > len(payload):
                break

            addr = payload[pos]; pos += 1
            vid = read_u16_le(payload, pos); pos += 2
            pid = read_u16_le(payload, pos); pos += 2
            state = payload[pos]; pos += 1
            slave_type = payload[pos]; pos += 1

            state_text = state_names.get(state, f"Unknown({state})")
            state_color = Fore.GREEN if state == 3 else (
                Fore.YELLOW if state >= 1 else Fore.RED)

            slave_text = ""
            if slave_type > 0:
                slave_text = f" -> {SlaveType.name(slave_type)}"

            # Look up friendly name from VID/PID
            dev_name = known_device_name(vid, pid)
            name_text = f" ({dev_name})" if dev_name else ""

            print(f"    [{i}] addr={addr} VID={vid:04X} PID={pid:04X} "
                  f"{state_color}{state_text}{Style.RESET_ALL}{slave_text}{name_text}")

        print()

    # =========================================================================
    # Audio Control Commands
    # =========================================================================

    def cmd_audio_play(self, args: List[str]):
        """Play audio file on a channel."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: hub.audio.play <ch> <path> [vol] [left|right] [loop [N|inf]]")
            return

        try:
            channel = int(args[0])
            path = args[1]
            volume = 100
            output = HubFxAudio.OUTPUT_STEREO
            loop_mode = HubFxAudio.LOOP_NONE
            loop_count = 0

            # Parse optional args
            i = 2
            while i < len(args):
                arg = args[i].lower()
                if arg in ('left', 'right', 'stereo'):
                    output = {'left': HubFxAudio.OUTPUT_LEFT,
                              'right': HubFxAudio.OUTPUT_RIGHT,
                              'stereo': HubFxAudio.OUTPUT_STEREO}[arg]
                elif arg == 'loop':
                    if i + 1 < len(args):
                        i += 1
                        if args[i].lower() == 'inf':
                            loop_mode = HubFxAudio.LOOP_INFINITE
                        else:
                            loop_mode = HubFxAudio.LOOP_FINITE
                            loop_count = int(args[i])
                    else:
                        loop_mode = HubFxAudio.LOOP_INFINITE
                elif arg.isdigit():
                    volume = int(arg)
                else:
                    try:
                        volume = int(arg)
                    except ValueError:
                        self.print_warning(f"Ignoring unknown arg: {arg}")
                i += 1

            packet = HubFxCommands.audio_play(channel, path, volume, output,
                                              loop_mode, loop_count)
            output_name = {HubFxAudio.OUTPUT_LEFT: ' [L]',
                           HubFxAudio.OUTPUT_RIGHT: ' [R]',
                           HubFxAudio.OUTPUT_STEREO: ''}[output]
            loop_str = ''
            if loop_mode == HubFxAudio.LOOP_INFINITE:
                loop_str = ' (loop inf)'
            elif loop_mode == HubFxAudio.LOOP_FINITE:
                loop_str = f' (loop x{loop_count})'
            self._send_ack(packet,
                           f"Play ch{channel}: {path} vol={volume}%{output_name}{loop_str}")
        except ValueError:
            self.print_error("Invalid channel or volume value")

    def cmd_audio_stop(self, args: List[str]):
        """Stop audio playback."""
        if not self._require_init():
            return

        channel = 0xFF  # default: all
        if args:
            if args[0].lower() == 'all':
                channel = 0xFF
            else:
                try:
                    channel = int(args[0])
                except ValueError:
                    self.print_error("Usage: hub.audio.stop [ch|all]")
                    return

        packet = HubFxCommands.audio_stop(channel)
        target = "all channels" if channel == 0xFF else f"ch{channel}"
        self._send_ack(packet, f"Audio stop {target}")

    def cmd_audio_volume(self, args: List[str]):
        """Set channel or master volume."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: hub.audio.volume <ch|master> <0-100>")
            return

        try:
            if args[0].lower() == 'master':
                channel = 0xFF
            else:
                channel = int(args[0])
            volume = int(args[1])

            packet = HubFxCommands.audio_volume(channel, volume)
            target = "master" if channel == 0xFF else f"ch{channel}"
            self._send_ack(packet, f"Volume {target} -> {volume}%")
        except ValueError:
            self.print_error("Invalid channel or volume value")

    def cmd_audio_fade(self, args: List[str]):
        """Fade out audio channel."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: hub.audio.fade <ch>")
            return
        try:
            channel = int(args[0])
            packet = HubFxCommands.audio_fade(channel)
            self._send_ack(packet, f"Fade out ch{channel}")
        except ValueError:
            self.print_error("Invalid channel")

    def cmd_audio_queue(self, args: List[str]):
        """Queue a sound to play after current."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: hub.audio.queue <ch> <path> [vol] [loop N]")
            return

        try:
            channel = int(args[0])
            path = args[1]
            volume = 100
            loop_count = 0

            i = 2
            while i < len(args):
                arg = args[i].lower()
                if arg == 'loop' and i + 1 < len(args):
                    i += 1
                    loop_count = int(args[i])
                else:
                    try:
                        volume = int(arg)
                    except ValueError:
                        self.print_warning(f"Ignoring unknown arg: {arg}")
                i += 1

            packet = HubFxCommands.audio_queue(channel, path, volume, loop_count)
            loop_str = f' (loop x{loop_count})' if loop_count > 0 else ''
            self._send_ack(packet,
                           f"Queue ch{channel}: {path} vol={volume}%{loop_str}")
        except ValueError:
            self.print_error("Invalid channel, volume, or loop count")

    def cmd_audio_queue_clear(self, args: List[str]):
        """Clear audio queue."""
        if not self._require_init():
            return

        channel = 0xFF  # default: all
        if args:
            if args[0].lower() == 'all':
                channel = 0xFF
            else:
                try:
                    channel = int(args[0])
                except ValueError:
                    self.print_error("Usage: hub.audio.clear [ch|all]")
                    return

        packet = HubFxCommands.audio_queue_clear(channel)
        target = "all channels" if channel == 0xFF else f"ch{channel}"
        self._send_ack(packet, f"Queue cleared {target}")

    def cmd_audio_status(self, args: List[str]):
        """Show audio mixer status."""
        if not self._require_init():
            return

        packet = HubFxCommands.audio_status()
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.is_nack:
            code = response.error_code
            self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.AUDIO_STATUS_RESP:
            self._parse_audio_status(response.payload)
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def _parse_audio_status(self, payload: bytes):
        """Parse and display AUDIO_STATUS_RESP payload (v3 extended format with ring stats)."""
        if len(payload) < 7:
            self.print_error("Audio status response too short")
            return

        output_names = {
            HubFxAudio.OUTPUT_STEREO: 'stereo',
            HubFxAudio.OUTPUT_LEFT: 'left',
            HubFxAudio.OUTPUT_RIGHT: 'right',
        }

        pos = 0

        # --- System header ---
        master_vol = payload[pos]; pos += 1
        flags = payload[pos]; pos += 1
        initialized = bool(flags & 0x01)
        i2s_running = bool(flags & 0x02)
        has_codec   = bool(flags & 0x04)
        has_ring_stats = bool(flags & 0x08)  # v3: has ring buffer stats
        has_buffer_caps = bool(flags & 0x10)  # v4: has buffer capacities

        sample_rate = read_u16_le(payload, pos); pos += 2
        bit_depth   = payload[pos]; pos += 1
        max_channels = payload[pos]; pos += 1

        codec_name_len = payload[pos]; pos += 1
        codec_name = payload[pos:pos + codec_name_len].decode('utf-8', errors='replace') if codec_name_len > 0 else ''
        pos += codec_name_len

        # --- Ring buffer stats (v3) ---
        ring_fill_pct = 0
        ring_avail_read = 0
        ring_avail_write = 0
        underruns = 0
        consume_loops = 0
        consume_frames = 0
        if has_ring_stats and pos + 9 <= len(payload):
            ring_fill_pct = payload[pos]; pos += 1
            ring_avail_read = read_u16_le(payload, pos); pos += 2
            ring_avail_write = read_u16_le(payload, pos); pos += 2
            underruns = read_u32_le(payload, pos); pos += 4
            # Consumer diagnostic counters (appended after underruns)
            if pos + 8 <= len(payload):
                consume_loops = read_u32_le(payload, pos); pos += 4
                consume_frames = read_u32_le(payload, pos); pos += 4

        # --- Buffer capacities (v4) ---
        wav_buf_capacity = 0
        ring_capacity = 0
        if has_buffer_caps and pos + 4 <= len(payload):
            wav_buf_capacity = read_u16_le(payload, pos); pos += 2
            ring_capacity = read_u16_le(payload, pos); pos += 2

        # Display system info
        print(f"\n  {Fore.CYAN}Audio Mixer Status{Style.RESET_ALL}")
        init_str = f"{Fore.GREEN}yes{Style.RESET_ALL}" if initialized else f"{Fore.RED}no{Style.RESET_ALL}"
        i2s_str  = f"{Fore.GREEN}running{Style.RESET_ALL}" if i2s_running else f"{Fore.RED}stopped{Style.RESET_ALL}"
        print(f"    Initialized: {init_str}")
        print(f"    I2S:         {i2s_str} ({sample_rate}Hz / {bit_depth}bit)")
        if has_codec:
            print(f"    Codec:       {codec_name}")
        else:
            print(f"    Codec:       {Fore.YELLOW}none (I2S only){Style.RESET_ALL}")
        print(f"    Max Ch:      {max_channels}")
        print(f"    Master Vol:  {master_vol}%")

        # Display ring buffer stats (v3)
        if has_ring_stats:
            ring_total = ring_avail_read + ring_avail_write
            underrun_str = f"{Fore.RED}{underruns}{Style.RESET_ALL}" if underruns > 0 else f"{Fore.GREEN}0{Style.RESET_ALL}"
            fill_color = Fore.GREEN if ring_fill_pct >= 50 else (Fore.YELLOW if ring_fill_pct >= 25 else Fore.RED)
            ring_cap_str = f"/{ring_capacity}" if ring_capacity > 0 else f"/{ring_total}"
            ring_ms = f" ({ring_avail_read * 1000 // sample_rate}ms)" if sample_rate > 0 and ring_avail_read > 0 else ""
            print(f"    Ring Buf:    {fill_color}{ring_fill_pct}%{Style.RESET_ALL} ({ring_avail_read}{ring_cap_str} frames{ring_ms})")
            print(f"    Underruns:   {underrun_str}")
            print(f"    Consumer:    {consume_loops} loops, {consume_frames} frames written to I2S")
        if has_buffer_caps and wav_buf_capacity > 0:
            wav_ms = wav_buf_capacity * 1000 // sample_rate if sample_rate > 0 else 0
            print(f"    WAV Buf:     {wav_buf_capacity} frames/ch ({wav_ms}ms, {wav_buf_capacity * 8 * 2 * 4 // 1024}KB total)")

        if pos >= len(payload):
            print(f"    {Fore.YELLOW}No channel data{Style.RESET_ALL}")
            print()
            return

        active_mask = payload[pos]; pos += 1

        if active_mask == 0:
            print(f"    {Fore.YELLOW}No active channels{Style.RESET_ALL}")
            print()
            return

        active_count = bin(active_mask).count('1')
        print(f"    Active:      {active_count} channel(s) (mask: 0b{active_mask:08b})")
        print()

        for _ in range(active_count):
            if pos + 16 > len(payload):
                break

            ch = payload[pos]; pos += 1
            vol = payload[pos]; pos += 1
            playing = payload[pos]; pos += 1
            looping = payload[pos]; pos += 1
            loop_count = read_u16_le(payload, pos); pos += 2
            remaining_ms = read_u32_le(payload, pos); pos += 4
            queue_len = payload[pos]; pos += 1
            output = payload[pos]; pos += 1

            # WAV format info
            wav_rate = read_u16_le(payload, pos); pos += 2
            wav_ch = payload[pos]; pos += 1
            wav_bits = payload[pos]; pos += 1

            # WAV decode buffer fill % (v4)
            wav_buf_fill = 0
            if has_buffer_caps and pos < len(payload):
                wav_buf_fill = payload[pos]; pos += 1

            # Filename
            fname_len = payload[pos]; pos += 1
            fname = payload[pos:pos + fname_len].decode('utf-8', errors='replace') if fname_len > 0 else ''
            pos += fname_len

            status = f"{Fore.GREEN}> playing{Style.RESET_ALL}" if playing else f"{Fore.YELLOW}- queued{Style.RESET_ALL}"
            out_name = output_names.get(output, f'out{output}')
            loop_str = ''
            if looping:
                if loop_count == 0xFFFF:
                    loop_str = ' loop=inf'
                else:
                    loop_str = f' loop=x{loop_count}'

            if remaining_ms > 0:
                rem_s = remaining_ms // 1000
                rem_frac = remaining_ms % 1000
                remaining_str = f' {rem_s}.{rem_frac:03d}s left'
            else:
                remaining_str = ''
            queue_str = f' [queue: {queue_len}]' if queue_len > 0 else ''
            wav_str = f'{wav_rate}Hz/{wav_bits}bit/{"stereo" if wav_ch == 2 else "mono"}' if wav_rate > 0 else ''

            # WAV buffer fill indicator
            if has_buffer_caps and playing:
                buf_color = Fore.GREEN if wav_buf_fill >= 80 else (Fore.YELLOW if wav_buf_fill >= 40 else Fore.RED)
                buf_str = f' buf={buf_color}{wav_buf_fill}%{Style.RESET_ALL}'
            else:
                buf_str = ''

            print(f"    ch{ch}: {status} vol={vol}% {out_name}{loop_str}{remaining_str}{queue_str}{buf_str}")
            if fname:
                print(f"          file: {fname}")
            if wav_str:
                print(f"          wav:  {wav_str}")

        print()

    # =========================================================================
    # Engine FX Commands
    # =========================================================================

    def cmd_engine_start(self, args: List[str]):
        """Start engine effects."""
        if not self._require_init():
            return
        packet = HubFxCommands.engine_start()
        self._send_ack(packet, "Engine FX started")

    def cmd_engine_stop(self, args: List[str]):
        """Stop engine effects."""
        if not self._require_init():
            return
        packet = HubFxCommands.engine_stop()
        self._send_ack(packet, "Engine FX stopped")

    def cmd_engine_status(self, args: List[str]):
        """Show engine FX status."""
        if not self._require_init():
            return

        packet = HubFxCommands.engine_status()
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.is_nack:
            code = response.error_code
            self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.ENGINE_STATUS_RESP:
            self._parse_engine_status(response.payload)
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def _parse_engine_status(self, payload: bytes):
        """Parse and display ENGINE_STATUS_RESP payload."""
        if len(payload) < 3:
            self.print_error("Engine status response too short")
            return

        state = payload[0]
        toggle_engaged = payload[1]
        active = payload[2]

        state_names = {
            EngineState.STOPPED: (f'{Fore.RED}Stopped', '-'),
            EngineState.STARTING: (f'{Fore.YELLOW}Starting', '*'),
            EngineState.RUNNING: (f'{Fore.GREEN}Running', '>'),
            EngineState.STOPPING: (f'{Fore.YELLOW}Stopping', '~'),
        }
        state_text, icon = state_names.get(state, (f'{Fore.RED}Unknown({state})', '?'))

        print(f"\n  {Fore.CYAN}Engine FX Status{Style.RESET_ALL}")
        print(f"    State:    {icon} {state_text}{Style.RESET_ALL}")
        print(f"    Toggle:   {'engaged' if toggle_engaged else 'disengaged'}")
        print(f"    Active:   {'yes' if active else 'no'}")
        print()

    # =========================================================================
    # Config Management Commands
    # =========================================================================

    def cmd_config_reload(self, args: List[str]):
        """Reload configuration from SD card."""
        if not self._require_init():
            return
        self.print_info("Reloading config from /config.yaml...")
        packet = HubFxCommands.config_reload()
        self._send_ack(packet, "Config reloaded", timeout=5.0)

    def cmd_config_get(self, args: List[str]):
        """Get configuration info."""
        if not self._require_init():
            return

        packet = HubFxCommands.config_get()
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.is_nack:
            code = response.error_code
            self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.CONFIG_GET_RESP:
            self._parse_config_get(response.payload)
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def _parse_config_get(self, payload: bytes):
        """Parse and display CONFIG_GET_RESP payload."""
        if len(payload) < 4:
            self.print_error("Config response too short")
            return

        loaded = payload[0]
        size = read_u16_le(payload, 1)
        # payload[3] is reserved

        print(f"\n  {Fore.CYAN}Config Status{Style.RESET_ALL}")
        status = f"{Fore.GREEN}loaded{Style.RESET_ALL}" if loaded else f"{Fore.RED}not loaded{Style.RESET_ALL}"
        print(f"    Status: {status}")
        print(f"    Size:   {size} bytes")
        print()

    # =========================================================================
    # SD Card Management Commands
    # =========================================================================

    def cmd_sd_init(self, args: List[str]):
        """Remount SD card (unmount + re-initialize)."""
        if not self._require_init():
            return

        packet = HubFxCommands.sd_init(0)  # speed ignored for SDIO
        with Spinner("Remounting SD card..."):
            success, response = self.conn.send_expect_ack(packet, timeout=15.0)
        if success:
            self.print_ok("SD card remounted")
        else:
            self._print_ack_response(response)

    def cmd_sd_status(self, args: List[str]):
        """Show SD card status."""
        if not self._require_init():
            return

        packet = HubFxCommands.sd_status()
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.is_nack:
            code = response.error_code
            self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.SD_STATUS_RESP:
            self._parse_sd_status(response.payload)
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def _parse_sd_status(self, payload: bytes):
        """Parse and display SD_STATUS_RESP payload."""
        if len(payload) < 1:
            self.print_error("SD status response too short")
            return

        initialized = payload[0]
        CARD_TYPES = {0: 'NONE', 1: 'MMC', 2: 'SD', 3: 'SDHC', 4: 'UNKNOWN'}
        BUS_MODES = {0: 'SPI', 1: 'SDIO 1-bit', 2: 'SDIO 4-bit'}

        print(f"\n  {Fore.CYAN}SD Card Status{Style.RESET_ALL}")
        if initialized:
            print(f"    Status: {Fore.GREEN}initialized{Style.RESET_ALL}")
            # Base payload: [init:u8][cardSize_MB:u32][totalSpace_MB:u32][freeSpace_MB:u32][fatType:u8]
            if len(payload) >= 14:
                card_size   = read_u32_le(payload, 1)    # MB
                total_space = read_u32_le(payload, 5)    # MB
                free_space  = read_u32_le(payload, 9)    # MB
                fat_type    = payload[13]
                print(f"    Card:   {card_size} MB")
                print(f"    Total:  {total_space} MB")
                print(f"    Free:   {free_space} MB")
                if fat_type > 0:
                    print(f"    FAT:    FAT{fat_type}")

            # Extended fields (v0.5+): [cardType:u8][busMode:u8][usedSpace_MB:u32LE]
            if len(payload) >= 20:
                card_type   = payload[14]
                bus_mode    = payload[15]
                used_space  = read_u32_le(payload, 16)
                type_name   = CARD_TYPES.get(card_type, f'0x{card_type:02X}')
                bus_name    = BUS_MODES.get(bus_mode, f'0x{bus_mode:02X}')
                print(f"    Type:   {type_name}")
                print(f"    Bus:    {bus_name}")
                print(f"    Used:   {used_space} MB")
        else:
            print(f"    Status: {Fore.RED}not initialized{Style.RESET_ALL}")
            print(f"    {Fore.YELLOW}Use 'sd.init' to remount{Style.RESET_ALL}")
        print()

    # =========================================================================
    # Flash Storage Commands
    # =========================================================================

    def cmd_flash_status(self, args: List[str]):
        """Show onboard flash (LittleFS) status."""
        if not self._require_init():
            return

        packet = HubFxCommands.flash_status()
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.is_nack:
            code = response.error_code
            self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.FLASH_STATUS_REQ:
            payload = response.payload
            if len(payload) < 1:
                self.print_error("Flash status response too short")
                return

            initialized = payload[0]
            print(f"\n  {Fore.CYAN}Flash Status{Style.RESET_ALL}")
            if initialized and len(payload) >= 13:
                total = read_u32_le(payload, 1)
                used  = read_u32_le(payload, 5)
                free  = read_u32_le(payload, 9)
                print(f"    Status: {Fore.GREEN}initialized{Style.RESET_ALL}")
                print(f"    Total:  {total} bytes ({total // 1024} KB)")
                print(f"    Used:   {used} bytes ({used // 1024} KB)")
                print(f"    Free:   {free} bytes ({free // 1024} KB)")
            else:
                print(f"    Status: {Fore.RED}not initialized{Style.RESET_ALL}")
            print()
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")
