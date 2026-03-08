"""
HubFX Command Handlers

HubFX-specific CLI commands:
- Slave management (list, init, status)
- Audio control (play, stop, volume, fade, queue, status)
- Engine FX control (start, stop, status)
- Config management (reload, get)
- SD card management (init, status)
- File operations (ls, rm, mkdir, info, download, upload, cat)
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
    HubFxCommands, HubFxPacket, HubFxError, HubFxAudio, EngineState, SlaveType,
    GunFxCommands, LightFxCommands, GearControlCommands, CoreError, StreamPacket,
)
from tests.framework.protocol import read_u16_le, read_u32_le, crc16_ccitt
from ..base import CommandHandlerBase, CommandInfo, ControllerType, Fore, Style
from .. import parsers


class HubFxCommandHandler(CommandHandlerBase):
    """
    Handler for HubFX-specific commands.

    These commands are only available when connected to a HubFX controller.
    They manage the slave controller registry and hub routing.

    Additionally, this handler exposes passthrough commands for all slave
    controller types — the hub routes them transparently.
    """

    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return HubFX command registry."""
        cmds = {
            # Hub management commands
            'hub.slaves': (self.cmd_slave_list, CommandInfo(
                'hub.slaves', 'hub.slaves',
                'List connected slave controllers',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.init': (self.cmd_slave_init, CommandInfo(
                'hub.init', 'hub.init <type>',
                'Init a slave (gunfx|lightfx|gearcontrol or 1|2|3)',
                requires_init=True, controller=ControllerType.HUBFX)),

            # Audio control commands
            'hub.audio.play': (self.cmd_audio_play, CommandInfo(
                'hub.audio.play', 'hub.audio.play <ch> <path> [vol] [left|right] [loop [N|inf]]',
                'Play audio file on channel',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.audio.stop': (self.cmd_audio_stop, CommandInfo(
                'hub.audio.stop', 'hub.audio.stop [ch|all]',
                'Stop audio (channel or all)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.audio.volume': (self.cmd_audio_volume, CommandInfo(
                'hub.audio.volume', 'hub.audio.volume <ch|master> <0-100>',
                'Set channel or master volume',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.audio.fade': (self.cmd_audio_fade, CommandInfo(
                'hub.audio.fade', 'hub.audio.fade <ch>',
                'Fade out audio channel',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.audio.queue': (self.cmd_audio_queue, CommandInfo(
                'hub.audio.queue', 'hub.audio.queue <ch> <path> [vol] [loop N]',
                'Queue sound to play after current',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.audio.clear': (self.cmd_audio_queue_clear, CommandInfo(
                'hub.audio.clear', 'hub.audio.clear [ch|all]',
                'Clear audio queue',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.audio.status': (self.cmd_audio_status, CommandInfo(
                'hub.audio.status', 'hub.audio.status',
                'Show audio mixer status',
                requires_init=True, controller=ControllerType.HUBFX)),

            # Engine FX commands
            'hub.engine.start': (self.cmd_engine_start, CommandInfo(
                'hub.engine.start', 'hub.engine.start',
                'Start engine effects',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.engine.stop': (self.cmd_engine_stop, CommandInfo(
                'hub.engine.stop', 'hub.engine.stop',
                'Stop engine effects',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.engine.status': (self.cmd_engine_status, CommandInfo(
                'hub.engine.status', 'hub.engine.status',
                'Show engine FX status',
                requires_init=True, controller=ControllerType.HUBFX)),

            # Config management commands
            'hub.config.reload': (self.cmd_config_reload, CommandInfo(
                'hub.config.reload', 'hub.config.reload',
                'Reload config from SD card',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.config.get': (self.cmd_config_get, CommandInfo(
                'hub.config.get', 'hub.config.get',
                'Get config info (loaded, size)',
                requires_init=True, controller=ControllerType.HUBFX)),

            # SD card management commands
            'hub.sd.init': (self.cmd_sd_init, CommandInfo(
                'hub.sd.init', 'hub.sd.init [speed_mhz]',
                'Initialize SD card (default 20 MHz)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.sd.status': (self.cmd_sd_status, CommandInfo(
                'hub.sd.status', 'hub.sd.status',
                'Show SD card status',
                requires_init=True, controller=ControllerType.HUBFX)),

            # File operation commands
            'hub.ls': (self.cmd_file_list, CommandInfo(
                'hub.ls', 'hub.ls [path]',
                'List directory contents (POSIX-style)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.rm': (self.cmd_file_delete, CommandInfo(
                'hub.rm', 'hub.rm <path>',
                'Delete a file',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.mkdir': (self.cmd_file_mkdir, CommandInfo(
                'hub.mkdir', 'hub.mkdir <path>',
                'Create a directory',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.info': (self.cmd_file_info, CommandInfo(
                'hub.info', 'hub.info <path>',
                'Show file or directory info',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.cat': (self.cmd_file_cat, CommandInfo(
                'hub.cat', 'hub.cat <path>',
                'Display file contents',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.download': (self.cmd_file_download, CommandInfo(
                'hub.download', 'hub.download <remote_path> <local_path>',
                'Download file from SD card',
                requires_init=True, controller=ControllerType.HUBFX)),
            'hub.upload': (self.cmd_file_upload, CommandInfo(
                'hub.upload', 'hub.upload <local_path> <remote_path>',
                'Upload file to SD card',
                requires_init=True, controller=ControllerType.HUBFX)),
        }

        # =====================================================================
        # GunFX passthrough commands (sent via hub routing)
        # =====================================================================
        cmds.update({
            'gfx.trigger': (self.cmd_gfx_trigger, CommandInfo(
                'gfx.trigger', 'gfx.trigger on <rpm> | gfx.trigger off',
                'GunFX: trigger control (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'gfx.servo': (self.cmd_gfx_servo, CommandInfo(
                'gfx.servo', 'gfx.servo set <id> <pulse_us>',
                'GunFX: set servo position (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'gfx.smoke': (self.cmd_gfx_smoke, CommandInfo(
                'gfx.smoke', 'gfx.smoke heat on|off',
                'GunFX: smoke heater control (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
        })

        # =====================================================================
        # LightFX passthrough commands
        # =====================================================================
        cmds.update({
            'lfx.led': (self.cmd_lfx_led, CommandInfo(
                'lfx.led', 'lfx.led <ch> <brightness>',
                'LightFX: set LED brightness (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'lfx.led.off': (self.cmd_lfx_led_off, CommandInfo(
                'lfx.led.off', 'lfx.led.off [ch]',
                'LightFX: turn off LED (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'lfx.servo': (self.cmd_lfx_servo, CommandInfo(
                'lfx.servo', 'lfx.servo set <id> <pulse_us>',
                'LightFX: set servo position (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
        })

        # =====================================================================
        # GearControl passthrough commands
        # =====================================================================
        cmds.update({
            'gc.deploy': (self.cmd_gc_deploy, CommandInfo(
                'gc.deploy', 'gc.deploy <gear_id> | gc.deploy all',
                'GearControl: deploy gear (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'gc.retract': (self.cmd_gc_retract, CommandInfo(
                'gc.retract', 'gc.retract <gear_id> | gc.retract all',
                'GearControl: retract gear (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
            'gc.stop': (self.cmd_gc_stop, CommandInfo(
                'gc.stop', 'gc.stop <gear_id>',
                'GearControl: emergency stop gear (routed via hub)',
                requires_init=True, controller=ControllerType.HUBFX)),
        })

        return cmds

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
            success, response = self.conn.send_expect_ack(packet)
            output_name = {HubFxAudio.OUTPUT_LEFT: ' [L]',
                           HubFxAudio.OUTPUT_RIGHT: ' [R]',
                           HubFxAudio.OUTPUT_STEREO: ''}[output]
            loop_str = ''
            if loop_mode == HubFxAudio.LOOP_INFINITE:
                loop_str = ' (loop ∞)'
            elif loop_mode == HubFxAudio.LOOP_FINITE:
                loop_str = f' (loop ×{loop_count})'
            self._print_ack_nack(success, response,
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
        success, response = self.conn.send_expect_ack(packet)
        target = "all channels" if channel == 0xFF else f"ch{channel}"
        self._print_ack_nack(success, response, f"Audio stop {target}")

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
            success, response = self.conn.send_expect_ack(packet)
            target = "master" if channel == 0xFF else f"ch{channel}"
            self._print_ack_nack(success, response, f"Volume {target} → {volume}%")
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
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Fade out ch{channel}")
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
            success, response = self.conn.send_expect_ack(packet)
            loop_str = f' (loop ×{loop_count})' if loop_count > 0 else ''
            self._print_ack_nack(success, response,
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
        success, response = self.conn.send_expect_ack(packet)
        target = "all channels" if channel == 0xFF else f"ch{channel}"
        self._print_ack_nack(success, response, f"Queue cleared {target}")

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
        """Parse and display AUDIO_STATUS_RESP payload."""
        if len(payload) < 2:
            self.print_error("Audio status response too short")
            return

        master_vol = payload[0]
        active_mask = payload[1]

        output_names = {
            HubFxAudio.OUTPUT_STEREO: 'stereo',
            HubFxAudio.OUTPUT_LEFT: 'left',
            HubFxAudio.OUTPUT_RIGHT: 'right',
        }

        print(f"\n  {Fore.CYAN}Audio Mixer Status{Style.RESET_ALL}")
        print(f"    Master Volume: {master_vol}%")

        if active_mask == 0:
            print(f"    {Fore.YELLOW}No active channels{Style.RESET_ALL}")
            print()
            return

        active_count = bin(active_mask).count('1')
        print(f"    Active: {active_count} channel(s) (mask: 0b{active_mask:08b})")

        pos = 2
        for _ in range(active_count):
            if pos + 10 > len(payload):
                break

            ch = payload[pos]
            vol = payload[pos + 1]
            playing = payload[pos + 2]
            looping = payload[pos + 3]
            loop_count = read_u16_le(payload, pos + 4)
            remaining_ms = read_u16_le(payload, pos + 6)
            queue_len = payload[pos + 8]
            output = payload[pos + 9]
            pos += 10

            status = f"{Fore.GREEN}▶ playing{Style.RESET_ALL}" if playing else f"{Fore.YELLOW}■ stopped{Style.RESET_ALL}"
            out_name = output_names.get(output, f'out{output}')
            loop_str = ''
            if looping:
                if loop_count == 0xFFFF:
                    loop_str = ' (loop ∞)'
                else:
                    loop_str = f' (loop ×{loop_count})'

            remaining_str = f' {remaining_ms}ms left' if remaining_ms > 0 else ''
            queue_str = f' [queue: {queue_len}]' if queue_len > 0 else ''

            print(f"    ch{ch}: {status} vol={vol}% {out_name}{loop_str}{remaining_str}{queue_str}")

        print()

    # =========================================================================
    # Engine FX Commands
    # =========================================================================

    def cmd_engine_start(self, args: List[str]):
        """Start engine effects."""
        if not self._require_init():
            return
        packet = HubFxCommands.engine_start()
        success, response = self.conn.send_expect_ack(packet)
        self._print_ack_nack(success, response, "Engine FX started")

    def cmd_engine_stop(self, args: List[str]):
        """Stop engine effects."""
        if not self._require_init():
            return
        packet = HubFxCommands.engine_stop()
        success, response = self.conn.send_expect_ack(packet)
        self._print_ack_nack(success, response, "Engine FX stopped")

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
            EngineState.STOPPED: (f'{Fore.RED}Stopped', '■'),
            EngineState.STARTING: (f'{Fore.YELLOW}Starting', '▸'),
            EngineState.RUNNING: (f'{Fore.GREEN}Running', '▶'),
            EngineState.STOPPING: (f'{Fore.YELLOW}Stopping', '▪'),
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
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)
        self._print_ack_nack(success, response, "Config reloaded")

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
        """Initialize SD card."""
        if not self._require_init():
            return
        speed = 20
        if args:
            try:
                speed = int(args[0])
            except ValueError:
                self.print_error("Usage: hub.sd.init [speed_mhz]")
                return

        self.print_info(f"Initializing SD card at {speed} MHz...")
        packet = HubFxCommands.sd_init(speed)
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)
        self._print_ack_nack(success, response, f"SD card initialized ({speed} MHz)")

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

        print(f"\n  {Fore.CYAN}SD Card Status{Style.RESET_ALL}")
        if initialized:
            print(f"    Status: {Fore.GREEN}initialized{Style.RESET_ALL}")
            # Enhanced payload: [init:u8][cardSize_MB:u32][totalSpace_MB:u32][freeSpace_MB:u32][fatType:u8]
            if len(payload) >= 14:
                card_size   = read_u32_le(payload, 1)    # MB
                total_space = read_u32_le(payload, 5)    # MB
                free_space  = read_u32_le(payload, 9)    # MB
                fat_type    = payload[13]
                print(f"    Card:   {card_size} MB")
                print(f"    Total:  {total_space} MB")
                print(f"    Free:   {free_space} MB")
                print(f"    Type:   FAT{fat_type}")
        else:
            print(f"    Status: {Fore.RED}not initialized{Style.RESET_ALL}")
        print()

    # =========================================================================
    # Stream Receiving Helper
    # =========================================================================

    def _receive_stream(self, tag: int, timeout: float = 10.0) -> Optional[Tuple[bytes, dict]]:
        """
        Receive a complete stream (BEGIN + DATA chunks + END).

        Returns (data_bytes, end_info) or None on error.
        end_info contains: total_segs, total_bytes, crc_all.
        """
        data = bytearray()
        total_expected = 0
        crc_errors = 0

        while True:
            response = self.conn._wait_for_tag(tag, timeout=timeout)
            if response is None:
                self.print_error("Stream timeout")
                return None

            if response.is_nack:
                code = response.error_code
                self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
                return None

            if response.packet_type == StreamPacket.STREAM_BEGIN:
                if len(response.payload) >= 4:
                    total_expected = read_u32_le(response.payload, 0)

            elif response.packet_type == StreamPacket.STREAM_DATA:
                if len(response.payload) < 4:
                    continue
                seq    = read_u16_le(response.payload, 0)
                crc    = read_u16_le(response.payload, 2)
                chunk  = response.payload[4:]
                # Verify CRC-16
                computed = crc16_ccitt(chunk)
                if computed != crc:
                    crc_errors += 1
                    self.print_warning(f"CRC mismatch on segment {seq}")
                data.extend(chunk)

            elif response.packet_type == StreamPacket.STREAM_END:
                end_info = {}
                if len(response.payload) >= 8:
                    end_info['total_segs']  = read_u16_le(response.payload, 0)
                    end_info['total_bytes'] = read_u32_le(response.payload, 2)
                    end_info['crc_all']     = read_u16_le(response.payload, 6)
                end_info['crc_errors'] = crc_errors

                # Verify overall CRC
                if 'crc_all' in end_info:
                    computed_all = crc16_ccitt(data)
                    if computed_all != end_info['crc_all']:
                        self.print_warning("Overall CRC mismatch on stream")

                return (bytes(data), end_info)

        return None

    # =========================================================================
    # File Operation Commands
    # =========================================================================

    def cmd_file_list(self, args: List[str]):
        """List directory contents."""
        if not self._require_init():
            return

        path = args[0] if args else "/"
        self.print_info(f"Listing {path} ...")

        packet = HubFxCommands.file_list(path)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return

        result = self._receive_stream(tag, timeout=10.0)
        if result is None:
            return

        data, end_info = result
        text = data.decode('utf-8', errors='replace')

        print(f"\n  {Fore.CYAN}{path}{Style.RESET_ALL}")
        for line in text.splitlines():
            if line.strip():
                print(f"    {line}")
        segs = end_info.get('total_segs', '?')
        total = end_info.get('total_bytes', len(data))
        print(f"\n    ({total} bytes, {segs} segments)")
        print()

    def cmd_file_delete(self, args: List[str]):
        """Delete a file."""
        if not self._require_init():
            return

        if not args:
            self.print_error("Usage: hub.rm <path>")
            return

        path = args[0]
        packet = HubFxCommands.file_delete(path)
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)
        self._print_ack_nack(success, response, f"Deleted: {path}")

    def cmd_file_mkdir(self, args: List[str]):
        """Create a directory."""
        if not self._require_init():
            return

        if not args:
            self.print_error("Usage: hub.mkdir <path>")
            return

        path = args[0]
        packet = HubFxCommands.file_mkdir(path)
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)
        self._print_ack_nack(success, response, f"Created: {path}")

    def cmd_file_info(self, args: List[str]):
        """Show file or directory info."""
        if not self._require_init():
            return

        if not args:
            self.print_error("Usage: hub.info <path>")
            return

        path = args[0]
        packet = HubFxCommands.file_info(path)
        response = self.conn.send_and_wait(packet)

        if response is None:
            self.print_error("No response (timeout)")
            return
        if response.is_nack:
            code = response.error_code
            self.print_error(f"NACK: {HubFxError.name(code)} (0x{code:02X})")
            return

        if response.packet_type == HubFxPacket.FILE_INFO_RESP:
            if len(response.payload) >= 6:
                exists = response.payload[0]
                is_dir = response.payload[1]
                size   = read_u32_le(response.payload, 2)

                print(f"\n  {Fore.CYAN}File Info: {path}{Style.RESET_ALL}")
                if exists:
                    kind = "directory" if is_dir else "file"
                    print(f"    Type:   {kind}")
                    if not is_dir:
                        print(f"    Size:   {size} bytes")
                else:
                    print(f"    {Fore.RED}Not found{Style.RESET_ALL}")
                print()
        else:
            self.print_error(f"Unexpected response: 0x{response.packet_type:02X}")

    def cmd_file_cat(self, args: List[str]):
        """Display file contents (download + print)."""
        if not self._require_init():
            return

        if not args:
            self.print_error("Usage: hub.cat <path>")
            return

        path = args[0]
        self.print_info(f"Reading {path} ...")

        packet = HubFxCommands.file_download(path)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return

        result = self._receive_stream(tag, timeout=30.0)
        if result is None:
            return

        data, end_info = result
        text = data.decode('utf-8', errors='replace')
        print()
        print(text)
        total = end_info.get('total_bytes', len(data))
        print(f"\n    ({total} bytes)")

    def cmd_file_download(self, args: List[str]):
        """Download file from SD card to local filesystem."""
        if not self._require_init():
            return

        if len(args) < 2:
            self.print_error("Usage: hub.download <remote_path> <local_path>")
            return

        remote_path = args[0]
        local_path = args[1]

        self.print_info(f"Downloading {remote_path} ...")

        packet = HubFxCommands.file_download(remote_path)
        tag = self.conn.next_tag()
        tagged = self.conn._inject_tag(packet, tag)
        if not self.conn.send(tagged):
            self.print_error("Send failed")
            return

        result = self._receive_stream(tag, timeout=60.0)
        if result is None:
            return

        data, end_info = result

        try:
            with open(local_path, 'wb') as f:
                f.write(data)
            total = end_info.get('total_bytes', len(data))
            self.print_ok(f"Downloaded {total} bytes → {local_path}")
        except IOError as e:
            self.print_error(f"Failed to write local file: {e}")

    def cmd_file_upload(self, args: List[str]):
        """Upload local file to SD card."""
        if not self._require_init():
            return

        if len(args) < 2:
            self.print_error("Usage: hub.upload <local_path> <remote_path>")
            return

        local_path = args[0]
        remote_path = args[1]

        # Read local file
        try:
            with open(local_path, 'rb') as f:
                file_data = f.read()
        except IOError as e:
            self.print_error(f"Cannot read local file: {e}")
            return

        file_size = len(file_data)
        self.print_info(f"Uploading {local_path} ({file_size} bytes) → {remote_path}")

        # Begin upload
        packet = HubFxCommands.file_upload_begin(remote_path, file_size)
        success, response = self.conn.send_expect_ack(packet, timeout=5.0)
        if not success:
            if response:
                code = response.error_code
                self.print_error(f"Upload begin failed: {HubFxError.name(code)}")
            else:
                self.print_error("Upload begin failed (timeout)")
            return

        # Send data chunks
        chunk_size = 508  # StreamProtocol::MAX_CHUNK_DATA
        offset = 0
        seq = 0
        max_retries = 3

        while offset < file_size:
            chunk = file_data[offset:offset + chunk_size]
            packet = HubFxCommands.file_upload_data(seq, chunk)

            # Retry loop for CRC errors
            sent = False
            for retry in range(max_retries):
                success, response = self.conn.send_expect_ack(packet, timeout=5.0)
                if success:
                    sent = True
                    break
                if response and response.error_code == CoreError.CRC_ERROR:
                    self.print_warning(f"CRC error on segment {seq}, retrying ({retry + 1}/{max_retries})")
                    continue
                # Other error — abort
                code = response.error_code if response else 0
                self.print_error(f"Upload failed at segment {seq}: {HubFxError.name(code)}")
                # Cancel the upload
                cancel_pkt = HubFxCommands.file_upload_cancel()
                self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                return

            if not sent:
                self.print_error(f"Upload failed: max retries on segment {seq}")
                cancel_pkt = HubFxCommands.file_upload_cancel()
                self.conn.send_expect_ack(cancel_pkt, timeout=5.0)
                return

            offset += len(chunk)
            seq += 1

            # Progress
            pct = (offset * 100) // file_size
            print(f"\r    [{pct:3d}%] {offset}/{file_size} bytes ({seq} segments)", end='', flush=True)

        print()  # newline after progress

        # End upload
        packet = HubFxCommands.file_upload_end()
        success, response = self.conn.send_expect_ack(packet, timeout=10.0)
        self._print_ack_nack(success, response, f"Uploaded {file_size} bytes → {remote_path}")

    # =========================================================================
    # GunFX Passthrough Commands
    # =========================================================================

    def cmd_gfx_trigger(self, args: List[str]):
        """GunFX trigger control (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gfx.trigger on <rpm> | gfx.trigger off")
            return

        action = args[0].lower()
        if action == 'on':
            if len(args) < 2:
                self.print_error("Usage: gfx.trigger on <rpm>")
                return
            try:
                rpm = int(args[1])
                packet = HubFxCommands.slave_route(GunFxCommands.trigger_on(rpm))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Trigger ON at {rpm} RPM")
            except ValueError:
                self.print_error("Invalid RPM value")
        elif action == 'off':
            packet = HubFxCommands.slave_route(GunFxCommands.trigger_off())
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, "Trigger OFF")
        else:
            self.print_error("Usage: gfx.trigger on <rpm> | gfx.trigger off")

    def cmd_gfx_servo(self, args: List[str]):
        """GunFX servo control (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: gfx.servo set <id> <pulse_us>")
            return
        try:
            servo_id = int(args[1])
            pulse_us = int(args[2])
            packet = HubFxCommands.slave_route(GunFxCommands.servo_set(servo_id, pulse_us))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Servo {servo_id} → {pulse_us}µs")
        except ValueError:
            self.print_error("Invalid servo parameters")

    def cmd_gfx_smoke(self, args: List[str]):
        """GunFX smoke control (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: gfx.smoke heat on|off")
            return
        if args[0].lower() == 'heat':
            on = args[1].lower() == 'on'
            packet = HubFxCommands.slave_route(GunFxCommands.smoke_heat(on))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Smoke heater {'ON' if on else 'OFF'}")
        else:
            self.print_error("Usage: gfx.smoke heat on|off")

    # =========================================================================
    # LightFX Passthrough Commands
    # =========================================================================

    def cmd_lfx_led(self, args: List[str]):
        """LightFX LED control (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: lfx.led <ch> <brightness>")
            return
        try:
            ch = int(args[0])
            brightness = int(args[1])
            packet = HubFxCommands.slave_route(LightFxCommands.led_set(ch, brightness))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"LED ch{ch} → {brightness}%")
        except ValueError:
            self.print_error("Invalid LED parameters")

    def cmd_lfx_led_off(self, args: List[str]):
        """LightFX LED off (routed via hub)."""
        if not self._require_init():
            return
        ch = int(args[0]) if args else 0
        packet = HubFxCommands.slave_route(LightFxCommands.led_off(ch))
        success, response = self.conn.send_expect_ack(packet)
        target = f"ch{ch}" if ch > 0 else "all"
        self._print_ack_nack(success, response, f"LED {target} OFF")

    def cmd_lfx_servo(self, args: List[str]):
        """LightFX servo control (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: lfx.servo set <id> <pulse_us>")
            return
        try:
            servo_id = int(args[1])
            pulse_us = int(args[2])
            packet = HubFxCommands.slave_route(LightFxCommands.servo_set(servo_id, pulse_us))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Servo {servo_id} → {pulse_us}µs")
        except ValueError:
            self.print_error("Invalid servo parameters")

    # =========================================================================
    # GearControl Passthrough Commands
    # =========================================================================

    def cmd_gc_deploy(self, args: List[str]):
        """GearControl deploy (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.deploy <gear_id> | gc.deploy all")
            return
        if args[0].lower() == 'all':
            packet = HubFxCommands.slave_route(GearControlCommands.gear_all(deploying=True))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, "Deploy ALL gears")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_deploy(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Deploy gear {gear_id}")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_retract(self, args: List[str]):
        """GearControl retract (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.retract <gear_id> | gc.retract all")
            return
        if args[0].lower() == 'all':
            packet = HubFxCommands.slave_route(GearControlCommands.gear_all(deploying=False))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, "Retract ALL gears")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_retract(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Retract gear {gear_id}")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_stop(self, args: List[str]):
        """GearControl emergency stop (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.stop <gear_id>")
            return
        try:
            gear_id = int(args[0])
            packet = HubFxCommands.slave_route(GearControlCommands.gear_stop(gear_id))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Stop gear {gear_id}")
        except ValueError:
            self.print_error("Invalid gear ID")

    # =========================================================================
    # Helpers
    # =========================================================================

    def _print_ack_nack(self, success: bool, response, ok_msg: str):
        """Print ACK/NACK result for a routed command."""
        if success:
            self.print_ok(ok_msg)
        elif response is not None:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
        else:
            self.print_error("No response (timeout)")
