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
from tests.framework.packets import DoorMode
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
                'sd.init', 'sd.init [speed_mhz]',
                'Initialize SD card (default 20 MHz)',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.status': (self.cmd_sd_status, CommandInfo(
                'sd.status', 'sd.status',
                'Show SD card status',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.ls': (self.cmd_file_list, CommandInfo(
                'sd.ls', 'sd.ls [path]',
                'List directory contents',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.rm': (self.cmd_file_delete, CommandInfo(
                'sd.rm', 'sd.rm <path>',
                'Delete a file',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.mkdir': (self.cmd_file_mkdir, CommandInfo(
                'sd.mkdir', 'sd.mkdir <path>',
                'Create a directory',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.info': (self.cmd_file_info, CommandInfo(
                'sd.info', 'sd.info <path>',
                'Show file or directory info',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.cat': (self.cmd_file_cat, CommandInfo(
                'sd.cat', 'sd.cat <path>',
                'Display file contents',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.download': (self.cmd_file_download, CommandInfo(
                'sd.download', 'sd.download <remote_path> <local_path>',
                'Download file from SD card',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),
            'sd.upload': (self.cmd_file_upload, CommandInfo(
                'sd.upload', 'sd.upload <local_path> <remote_path>',
                'Upload file to SD card',
                requires_init=True, controller=ControllerType.HUBFX, group='SD Card & Files')),

            # =================================================================
            # Slave Controllers (routed via hub)
            # =================================================================
            'slave': (self.cmd_slave, CommandInfo(
                'slave', 'slave <gfx|lfx|gc>.<cmd> [args...]',
                'Route command to slave controller (help gfx|lfx|gc for details)',
                requires_init=True, controller=ControllerType.HUBFX, group='Slave Controllers')),
        }

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
        """Build the slave sub-command dispatch table."""
        GFX = 'GunFX (gfx.*)'
        LFX = 'LightFX (lfx.*)'
        GC = 'GearControl (gc.*)'
        return {
            # GunFX
            'gfx.trigger': (self.cmd_gfx_trigger, CommandInfo(
                'gfx.trigger', 'slave gfx.trigger on <rpm> | off [delay_ms]',
                'Trigger control', group=GFX)),
            'gfx.servo': (self.cmd_gfx_servo, CommandInfo(
                'gfx.servo', 'slave gfx.servo set <id> <pulse_us>',
                'Set servo position (1-3, 500-2500µs)', group=GFX)),
            'gfx.servo.config': (self.cmd_gfx_servo_config, CommandInfo(
                'gfx.servo.config', 'slave gfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits and motion profile', group=GFX)),
            'gfx.servo.recoil': (self.cmd_gfx_servo_recoil, CommandInfo(
                'gfx.servo.recoil', 'slave gfx.servo.recoil <id> <jerk_us> <variance_us>',
                'Configure recoil effect', group=GFX)),
            'gfx.smoke': (self.cmd_gfx_smoke, CommandInfo(
                'gfx.smoke', 'slave gfx.smoke heat on|off',
                'Smoke heater control', group=GFX)),
            'gfx.smoke.config': (self.cmd_gfx_smoke_config, CommandInfo(
                'gfx.smoke.config', 'slave gfx.smoke.config [key=value ...]',
                'Configure smoke fan (pulsing,speed,high,low,pulse_ms,spindown_ms)', group=GFX)),
            'gfx.smoke.reset': (self.cmd_gfx_smoke_reset, CommandInfo(
                'gfx.smoke.reset', 'slave gfx.smoke.reset',
                'Clear smoke error states', group=GFX)),
            'gfx.smoke.limit': (self.cmd_gfx_smoke_limit, CommandInfo(
                'gfx.smoke.limit', 'slave gfx.smoke.limit heater|fan <mA>',
                'Set overcurrent protection limit (0=disable)', group=GFX)),
            # LightFX
            'lfx.led': (self.cmd_lfx_led, CommandInfo(
                'lfx.led', 'slave lfx.led <ch> <brightness> | slave lfx.led off [ch]',
                'Set LED brightness or turn off', group=LFX)),
            'lfx.led.off': (self.cmd_lfx_led_off, CommandInfo(
                'lfx.led.off', 'slave lfx.led.off [ch]',
                'Turn off LED (0=all)', group=LFX)),
            'lfx.led.seq': (self.cmd_lfx_led_seq, CommandInfo(
                'lfx.led.seq', 'slave lfx.led.seq clear|start|stop|restart <ch>',
                'Control LED sequences', group=LFX)),
            'lfx.led.seq.add': (self.cmd_lfx_led_seq_add, CommandInfo(
                'lfx.led.seq.add', 'slave lfx.led.seq.add <ch> <event> <params...>',
                'Add sequence event (on/off/flash/fadein/fadeout)', group=LFX)),
            'lfx.brightness': (self.cmd_lfx_brightness, CommandInfo(
                'lfx.brightness', 'slave lfx.brightness <0-100>',
                'Set master LED brightness', group=LFX)),
            'lfx.servo': (self.cmd_lfx_servo, CommandInfo(
                'lfx.servo', 'slave lfx.servo set <id> <pulse_us>',
                'Set servo position (1-3)', group=LFX)),
            'lfx.servo.config': (self.cmd_lfx_servo_config, CommandInfo(
                'lfx.servo.config', 'slave lfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits and motion profile', group=LFX)),
            'lfx.ll.bind': (self.cmd_lfx_ll_bind, CommandInfo(
                'lfx.ll.bind', 'slave lfx.ll.bind <slot> <servo> <led_ch> <deploy_us> <retract_us> [brightness]',
                'Bind landing light (slot 1-3)', group=LFX)),
            'lfx.ll.unbind': (self.cmd_lfx_ll_unbind, CommandInfo(
                'lfx.ll.unbind', 'slave lfx.ll.unbind [slot]',
                'Unbind landing light (0=all)', group=LFX)),
            'lfx.ll.deploy': (self.cmd_lfx_ll_deploy, CommandInfo(
                'lfx.ll.deploy', 'slave lfx.ll.deploy [slot]',
                'Deploy landing light + turn on (0=all)', group=LFX)),
            'lfx.ll.retract': (self.cmd_lfx_ll_retract, CommandInfo(
                'lfx.ll.retract', 'slave lfx.ll.retract [slot]',
                'Retract landing light + turn off (0=all)', group=LFX)),
            'lfx.reset': (self.cmd_lfx_reset, CommandInfo(
                'lfx.reset', 'slave lfx.reset [ch]',
                'Reset LED channel (stop seq, off, re-enable; 0=all)', group=LFX)),
            'lfx.enable': (self.cmd_lfx_enable, CommandInfo(
                'lfx.enable', 'slave lfx.enable <ch>',
                'Enable LED channel (1-8, 0=all)', group=LFX)),
            'lfx.disable': (self.cmd_lfx_disable, CommandInfo(
                'lfx.disable', 'slave lfx.disable <ch>',
                'Disable LED channel (1-8, 0=all)', group=LFX)),
            # GearControl
            'gc.deploy': (self.cmd_gc_deploy, CommandInfo(
                'gc.deploy', 'slave gc.deploy <gear_id> | all',
                'Deploy landing gear (0=nose, 1=left, 2=right)', group=GC)),
            'gc.retract': (self.cmd_gc_retract, CommandInfo(
                'gc.retract', 'slave gc.retract <gear_id> | all',
                'Retract landing gear', group=GC)),
            'gc.stop': (self.cmd_gc_stop, CommandInfo(
                'gc.stop', 'slave gc.stop <gear_id> | all',
                'Emergency stop motor', group=GC)),
            'gc.servo': (self.cmd_gc_servo, CommandInfo(
                'gc.servo', 'slave gc.servo set <id> <pulse_us>',
                'Set servo position (0-7, 500-2500µs)', group=GC)),
            'gc.servo.config': (self.cmd_gc_servo_config, CommandInfo(
                'gc.servo.config', 'slave gc.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits and motion profile', group=GC)),
            'gc.gear.config': (self.cmd_gc_gear_config, CommandInfo(
                'gc.gear.config', 'slave gc.gear.config <id> <flags...> [stall_mA] [timeout_ms]',
                'Configure gear behavior (flags: yaw none)', group=GC)),
            'gc.door.config': (self.cmd_gc_door_config, CommandInfo(
                'gc.door.config', 'slave gc.door.config <id> <open0> <close0> <open1> <close1>',
                'Configure door servo positions', group=GC)),
            'gc.door.mode': (self.cmd_gc_door_mode, CommandInfo(
                'gc.door.mode', 'slave gc.door.mode <gear_id> <pre_deploy> [post_deploy] [delay_ms]',
                'Set door modes (none/single/dual-sync/dual-delay/dual-seq)', group=GC)),
            'gc.yaw.config': (self.cmd_gc_yaw_config, CommandInfo(
                'gc.yaw.config', 'slave gc.yaw.config <gear_id> <neutral> <min> <max>',
                'Configure yaw servo', group=GC)),
            'gc.yaw': (self.cmd_gc_yaw, CommandInfo(
                'gc.yaw', 'slave gc.yaw <position_us>',
                'Set yaw position (active when associated gear deployed)', group=GC)),
            'gc.calibrate': (self.cmd_gc_calibrate, CommandInfo(
                'gc.calibrate', 'slave gc.calibrate <gear_id> | all [timeout_s]',
                'Calibrate stall current', group=GC)),
            'gc.calibrate.cancel': (self.cmd_gc_calibrate_cancel, CommandInfo(
                'gc.calibrate.cancel', 'slave gc.calibrate.cancel <gear_id> | all',
                'Cancel calibration in progress', group=GC)),
            'gc.reset': (self.cmd_gc_reset, CommandInfo(
                'gc.reset', 'slave gc.reset <gear_id> | all',
                'Clear error state (ERROR → UNKNOWN)', group=GC)),
            'gc.enable': (self.cmd_gc_enable, CommandInfo(
                'gc.enable', 'slave gc.enable <gear_id> | all',
                'Enable gear channel', group=GC)),
            'gc.disable': (self.cmd_gc_disable, CommandInfo(
                'gc.disable', 'slave gc.disable <gear_id> | all',
                'Disable gear channel', group=GC)),
            'gc.battery': (self.cmd_gc_battery, CommandInfo(
                'gc.battery', 'slave gc.battery <on|off> [autodeploy]',
                'Enable/disable battery monitoring', group=GC)),
        }

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
        """Parse and display AUDIO_STATUS_RESP payload (v2 extended format)."""
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

        sample_rate = read_u16_le(payload, pos); pos += 2
        bit_depth   = payload[pos]; pos += 1
        max_channels = payload[pos]; pos += 1

        codec_name_len = payload[pos]; pos += 1
        codec_name = payload[pos:pos + codec_name_len].decode('utf-8', errors='replace') if codec_name_len > 0 else ''
        pos += codec_name_len

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

            # Filename
            fname_len = payload[pos]; pos += 1
            fname = payload[pos:pos + fname_len].decode('utf-8', errors='replace') if fname_len > 0 else ''
            pos += fname_len

            status = f"{Fore.GREEN}▶ playing{Style.RESET_ALL}" if playing else f"{Fore.YELLOW}■ queued{Style.RESET_ALL}"
            out_name = output_names.get(output, f'out{output}')
            loop_str = ''
            if looping:
                if loop_count == 0xFFFF:
                    loop_str = ' loop=∞'
                else:
                    loop_str = f' loop=×{loop_count}'

            if remaining_ms > 0:
                rem_s = remaining_ms // 1000
                rem_frac = remaining_ms % 1000
                remaining_str = f' {rem_s}.{rem_frac:03d}s left'
            else:
                remaining_str = ''
            queue_str = f' [queue: {queue_len}]' if queue_len > 0 else ''
            wav_str = f'{wav_rate}Hz/{wav_bits}bit/{"stereo" if wav_ch == 2 else "mono"}' if wav_rate > 0 else ''

            print(f"    ch{ch}: {status} vol={vol}% {out_name}{loop_str}{remaining_str}{queue_str}")
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
            on = args[1].lower() in ('on', '1', 'true', 'yes')
            packet = HubFxCommands.slave_route(GunFxCommands.smoke_heat(on))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Smoke heater {'ON' if on else 'OFF'}")
        else:
            self.print_error("Usage: gfx.smoke heat on|off")

    def cmd_gfx_servo_config(self, args: List[str]):
        """GunFX servo configuration (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 3:
            self.print_error("Usage: gfx.servo.config <id> <min> <max> [speed] [accel] [decel]")
            return
        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000
            packet = HubFxCommands.slave_route(
                GunFxCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response,
                                 f"Servo {servo_id} configured: range {min_us}-{max_us}µs, speed {speed}µs/s")
        except ValueError:
            self.print_error("Invalid servo config parameters")

    def cmd_gfx_servo_recoil(self, args: List[str]):
        """GunFX servo recoil configuration (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 3:
            self.print_error("Usage: gfx.servo.recoil <id> <jerk_us> <variance_us>")
            return
        try:
            servo_id = int(args[0])
            jerk = int(args[1])
            variance = int(args[2])
            packet = HubFxCommands.slave_route(
                GunFxCommands.servo_recoil(servo_id, jerk, variance))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response,
                                 f"Servo {servo_id} recoil: jerk {jerk}µs, variance ±{variance}µs")
        except ValueError:
            self.print_error("Invalid recoil parameters")

    def cmd_gfx_smoke_config(self, args: List[str]):
        """GunFX smoke fan configuration (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error(
                "Usage: gfx.smoke.config [key=value ...]\n"
                "  Keys: pulsing (0|1), speed (0-255), high (0-255),\n"
                "        low (0-255), pulse_ms (0=auto), spindown_ms (0-60000)\n"
                "  Example: gfx.smoke.config pulsing=1 speed=200")
            return
        pulsing = False
        speed = 255
        high = 255
        low = 80
        pulse_ms = 0
        spindown_ms = 5000
        try:
            for arg in args:
                if '=' not in arg:
                    self.print_error(f"Invalid parameter '{arg}'. Use key=value format.")
                    return
                key, val = arg.split('=', 1)
                key = key.lower()
                if key == 'pulsing':
                    pulsing = val in ('1', 'true', 'on')
                elif key == 'speed':
                    speed = int(val)
                elif key == 'high':
                    high = int(val)
                elif key == 'low':
                    low = int(val)
                elif key in ('pulse_ms', 'pulse'):
                    pulse_ms = int(val)
                elif key in ('spindown_ms', 'spindown'):
                    spindown_ms = int(val)
                else:
                    self.print_error(f"Unknown parameter '{key}'")
                    return
            packet = HubFxCommands.slave_route(
                GunFxCommands.smoke_settings(pulsing, speed, high, low, pulse_ms, spindown_ms))
            success, response = self.conn.send_expect_ack(packet)
            mode = "pulsing" if pulsing else "constant"
            pulse_info = "auto" if pulse_ms == 0 else f"{pulse_ms}ms"
            self._print_ack_nack(success, response,
                                 f"Smoke: {mode}, speed={speed}, high/low={high}/{low}, "
                                 f"pulse={pulse_info}, spindown={spindown_ms}ms")
        except ValueError:
            self.print_error("Invalid parameter value (must be numeric)")

    def cmd_gfx_smoke_reset(self, args: List[str]):
        """Clear GunFX smoke error states (routed via hub)."""
        if not self._require_init():
            return
        packet = HubFxCommands.slave_route(GunFxCommands.smoke_reset())
        success, response = self.conn.send_expect_ack(packet)
        self._print_ack_nack(success, response, "Smoke errors cleared")

    def cmd_gfx_smoke_limit(self, args: List[str]):
        """Set GunFX overcurrent protection limit (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: gfx.smoke.limit heater|fan <mA>  (0=disable)")
            return
        ch_name = args[0].lower()
        if ch_name in ('heater', '0'):
            channel = 0
        elif ch_name in ('fan', '1'):
            channel = 1
        else:
            self.print_error("Channel must be 'heater' (0) or 'fan' (1)")
            return
        try:
            limit_mA = int(args[1])
            packet = HubFxCommands.slave_route(
                GunFxCommands.smoke_current_limit(channel, limit_mA))
            success, response = self.conn.send_expect_ack(packet)
            ch_label = 'Heater' if channel == 0 else 'Fan'
            if limit_mA == 0:
                self._print_ack_nack(success, response, f"{ch_label} overcurrent protection disabled")
            else:
                self._print_ack_nack(success, response, f"{ch_label} current limit set to {limit_mA} mA")
        except ValueError:
            self.print_error("Invalid current limit value")

    # =========================================================================
    # LightFX Passthrough Commands
    # =========================================================================

    def cmd_lfx_led(self, args: List[str]):
        """LightFX LED control (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: lfx.led <ch> <brightness> | lfx.led off [ch]")
            return
        subcmd = args[0].lower()
        if subcmd == 'off':
            ch = int(args[1]) if len(args) > 1 else 0
            packet = HubFxCommands.slave_route(LightFxCommands.led_off(ch))
            success, response = self.conn.send_expect_ack(packet)
            target = f"ch{ch}" if ch > 0 else "all"
            self._print_ack_nack(success, response, f"LED {target} OFF")
        else:
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

    def cmd_lfx_led_seq(self, args: List[str]):
        """LightFX LED sequence control (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: lfx.led.seq clear|start|stop|restart <ch>")
            return
        subcmd = args[0].lower()
        ch = int(args[1]) if len(args) > 1 else 0
        if subcmd == 'clear':
            packet = HubFxCommands.slave_route(LightFxCommands.led_seq_clear(ch))
            msg = f"LED {ch} sequence cleared"
        elif subcmd == 'start':
            packet = HubFxCommands.slave_route(LightFxCommands.led_seq_start(ch))
            msg = f"LED {ch} sequence started"
        elif subcmd == 'stop':
            packet = HubFxCommands.slave_route(LightFxCommands.led_seq_stop(ch))
            msg = f"LED {ch} sequence stopped"
        elif subcmd == 'restart':
            packet = HubFxCommands.slave_route(LightFxCommands.led_seq_restart(ch))
            msg = f"LED {ch} sequence restarted"
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'clear', 'start', 'stop', or 'restart'")
            return
        success, response = self.conn.send_expect_ack(packet)
        self._print_ack_nack(success, response, msg)

    def cmd_lfx_led_seq_add(self, args: List[str]):
        """LightFX LED sequence add event (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: lfx.led.seq.add <ch> <event> <params...>")
            self.print_info("Events: on, off, flash, fadein, fadeout")
            return
        try:
            ch = int(args[0])
            event = args[1].lower()
            if event == 'on':
                if len(args) < 4:
                    self.print_error("Usage: lfx.led.seq.add <ch> on <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = HubFxCommands.slave_route(
                    LightFxCommands.led_seq_add_on(ch, duration, brightness))
                msg = f"LED {ch}: ON {duration}ms at {brightness}%"
            elif event == 'off':
                if len(args) < 3:
                    self.print_error("Usage: lfx.led.seq.add <ch> off <duration_ms>")
                    return
                duration = int(args[2])
                packet = HubFxCommands.slave_route(
                    LightFxCommands.led_seq_add_off(ch, duration))
                msg = f"LED {ch}: OFF {duration}ms"
            elif event == 'flash':
                if len(args) < 5:
                    self.print_error("Usage: lfx.led.seq.add <ch> flash <interval_ms> <duration_ms> <brightness> [duty]")
                    return
                interval = int(args[2])
                duration = int(args[3])
                brightness = int(args[4])
                duty = int(args[5]) if len(args) > 5 else 50
                packet = HubFxCommands.slave_route(
                    LightFxCommands.led_seq_add_flash(ch, interval, duration, brightness, duty))
                msg = f"LED {ch}: FLASH {interval}ms for {duration}ms, {duty}% duty"
            elif event == 'fadein':
                if len(args) < 4:
                    self.print_error("Usage: lfx.led.seq.add <ch> fadein <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = HubFxCommands.slave_route(
                    LightFxCommands.led_seq_add_fade_in(ch, duration, brightness))
                msg = f"LED {ch}: FADE IN {duration}ms to {brightness}%"
            elif event == 'fadeout':
                if len(args) < 4:
                    self.print_error("Usage: lfx.led.seq.add <ch> fadeout <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = HubFxCommands.slave_route(
                    LightFxCommands.led_seq_add_fade_out(ch, duration, brightness))
                msg = f"LED {ch}: FADE OUT {duration}ms from {brightness}%"
            else:
                self.print_error(f"Unknown event: {event}. Available: on, off, flash, fadein, fadeout")
                return
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Sequence event added: {msg}")
        except (ValueError, IndexError) as e:
            self.print_error(f"Invalid parameters: {e}")

    def cmd_lfx_brightness(self, args: List[str]):
        """Set LightFX master LED brightness (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: lfx.brightness <0-100>")
            return
        try:
            pct = int(args[0])
            packet = HubFxCommands.slave_route(LightFxCommands.led_master_brightness(pct))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Master brightness → {pct}%")
        except ValueError:
            self.print_error("Invalid brightness value")

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

    def cmd_lfx_servo_config(self, args: List[str]):
        """LightFX servo configuration (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 3:
            self.print_error("Usage: lfx.servo.config <id> <min> <max> [speed] [accel] [decel]")
            return
        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000
            packet = HubFxCommands.slave_route(
                LightFxCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response,
                                 f"Servo {servo_id} configured: range {min_us}-{max_us}µs, speed {speed}µs/s")
        except ValueError:
            self.print_error("Invalid servo config parameters")

    def cmd_lfx_ll_bind(self, args: List[str]):
        """Bind LightFX landing light (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 5:
            self.print_error("Usage: lfx.ll.bind <slot> <servo_id> <led_ch> <deploy_us> <retract_us> [brightness]")
            return
        try:
            slot = int(args[0])
            servo_id = int(args[1])
            led_ch = int(args[2])
            deploy_us = int(args[3])
            retract_us = int(args[4])
            brightness = int(args[5]) if len(args) > 5 else 100
            packet = HubFxCommands.slave_route(
                LightFxCommands.landing_light_bind(slot, servo_id, led_ch, deploy_us, retract_us, brightness))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response,
                                 f"Landing light {slot}: servo {servo_id} + LED {led_ch}, "
                                 f"deploy {deploy_us}µs, retract {retract_us}µs, brightness {brightness}%")
        except ValueError:
            self.print_error("Invalid parameters")

    def cmd_lfx_ll_unbind(self, args: List[str]):
        """Unbind LightFX landing light slot (routed via hub)."""
        if not self._require_init():
            return
        slot = int(args[0]) if args else 0
        packet = HubFxCommands.slave_route(LightFxCommands.landing_light_unbind(slot))
        success, response = self.conn.send_expect_ack(packet)
        target = f"slot {slot}" if slot > 0 else "all slots"
        self._print_ack_nack(success, response, f"Landing light {target} unbound")

    def cmd_lfx_ll_deploy(self, args: List[str]):
        """Deploy LightFX landing light (routed via hub)."""
        if not self._require_init():
            return
        slot = int(args[0]) if args else 0
        packet = HubFxCommands.slave_route(LightFxCommands.landing_light_deploy(slot))
        success, response = self.conn.send_expect_ack(packet)
        target = f"slot {slot}" if slot > 0 else "all"
        self._print_ack_nack(success, response, f"Landing light {target} deploying")

    def cmd_lfx_ll_retract(self, args: List[str]):
        """Retract LightFX landing light (routed via hub)."""
        if not self._require_init():
            return
        slot = int(args[0]) if args else 0
        packet = HubFxCommands.slave_route(LightFxCommands.landing_light_retract(slot))
        success, response = self.conn.send_expect_ack(packet)
        target = f"slot {slot}" if slot > 0 else "all"
        self._print_ack_nack(success, response, f"Landing light {target} retracting")

    def cmd_lfx_reset(self, args: List[str]):
        """Reset LightFX LED channel (routed via hub)."""
        if not self._require_init():
            return
        ch = int(args[0]) if args else 0
        packet = HubFxCommands.slave_route(LightFxCommands.led_reset(ch))
        success, response = self.conn.send_expect_ack(packet)
        target = f"LED {ch}" if ch > 0 else "All LEDs"
        self._print_ack_nack(success, response, f"{target} reset")

    def cmd_lfx_enable(self, args: List[str]):
        """Enable LightFX LED channel (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: lfx.enable <ch> (1-8, 0=all)")
            return
        try:
            ch = int(args[0])
            packet = HubFxCommands.slave_route(LightFxCommands.led_enable(ch, True))
            success, response = self.conn.send_expect_ack(packet)
            target = f"LED {ch}" if ch > 0 else "All LEDs"
            self._print_ack_nack(success, response, f"{target} enabled")
        except ValueError:
            self.print_error("Invalid channel number")

    def cmd_lfx_disable(self, args: List[str]):
        """Disable LightFX LED channel (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: lfx.disable <ch> (1-8, 0=all)")
            return
        try:
            ch = int(args[0])
            packet = HubFxCommands.slave_route(LightFxCommands.led_enable(ch, False))
            success, response = self.conn.send_expect_ack(packet)
            target = f"LED {ch}" if ch > 0 else "All LEDs"
            self._print_ack_nack(success, response, f"{target} disabled")
        except ValueError:
            self.print_error("Invalid channel number")

    # =========================================================================
    # GearControl Passthrough Commands
    # =========================================================================

    def cmd_gc_deploy(self, args: List[str]):
        """GearControl deploy (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.deploy <gear_id> | all")
            return
        if args[0].lower() == 'all':
            packet = HubFxCommands.slave_route(GearControlCommands.gear_all(1))  # ACTION_DEPLOY
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, "Deploy ALL gears")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_deploy(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                names = {0: "nose", 1: "left main", 2: "right main"}
                self._print_ack_nack(success, response, f"Deploy gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_retract(self, args: List[str]):
        """GearControl retract (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.retract <gear_id> | all")
            return
        if args[0].lower() == 'all':
            packet = HubFxCommands.slave_route(GearControlCommands.gear_all(0))  # ACTION_RETRACT
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, "Retract ALL gears")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_retract(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                names = {0: "nose", 1: "left main", 2: "right main"}
                self._print_ack_nack(success, response, f"Retract gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_stop(self, args: List[str]):
        """GearControl emergency stop (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.stop <gear_id> | all")
            return
        if args[0].lower() == 'all':
            packet = HubFxCommands.slave_route(GearControlCommands.gear_all(2))  # ACTION_STOP
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, "STOP ALL motors")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_stop(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"STOP motor {gear_id}")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_servo(self, args: List[str]):
        """GearControl servo control (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: gc.servo set <id> <pulse_us>")
            return
        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = HubFxCommands.slave_route(GearControlCommands.servo_set(servo_id, pulse))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Servo {servo_id} → {pulse}µs")
        except ValueError:
            self.print_error("Invalid servo parameters")

    def cmd_gc_servo_config(self, args: List[str]):
        """GearControl servo configuration (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 3:
            self.print_error("Usage: gc.servo.config <id> <min> <max> [speed] [accel] [decel]")
            return
        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000
            packet = HubFxCommands.slave_route(
                GearControlCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response,
                                 f"Servo {servo_id} configured: range {min_us}-{max_us}µs, speed {speed}µs/s")
        except ValueError:
            self.print_error("Invalid servo config parameters")

    def cmd_gc_gear_config(self, args: List[str]):
        """GearControl gear behavior configuration (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 2:
            self.print_error("Usage: gc.gear.config <id> <flags...> [stall_mA] [timeout_ms]")
            self.print_info("  Flags: yaw, none")
            self.print_info("  Defaults: stall=500mA, timeout=60000ms")
            return
        FLAG_MAP = {'yaw': 0x01}
        try:
            gear_id = int(args[0])
            flags = 0
            numeric_args = []
            has_flag_token = False
            for arg in args[1:]:
                lower = arg.lower()
                if lower in FLAG_MAP:
                    flags |= FLAG_MAP[lower]
                    has_flag_token = True
                elif lower == 'none':
                    flags = 0
                    has_flag_token = True
                else:
                    numeric_args.append(int(arg))
            if not has_flag_token and len(numeric_args) >= 1:
                flags = numeric_args.pop(0)
            stall_mA = numeric_args[0] if len(numeric_args) > 0 else 500
            timeout_ms = numeric_args[1] if len(numeric_args) > 1 else 60000
            packet = HubFxCommands.slave_route(
                GearControlCommands.gear_config(gear_id, flags, stall_mA, timeout_ms))
            success, response = self.conn.send_expect_ack(packet)
            flag_parts = []
            if flags & 0x01:
                flag_parts.append("yaw")
            flag_str = ', '.join(flag_parts) if flag_parts else "none"
            self._print_ack_nack(success, response,
                                 f"Gear {gear_id}: flags=[{flag_str}], stall={stall_mA}mA, timeout={timeout_ms}ms")
        except ValueError:
            self.print_error("Invalid gear config parameters")

    def cmd_gc_door_config(self, args: List[str]):
        """GearControl door servo positions (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 5:
            self.print_error("Usage: gc.door.config <gear_id> <open0_us> <close0_us> <open1_us> <close1_us>")
            return
        try:
            gear_id = int(args[0])
            open0 = int(args[1])
            close0 = int(args[2])
            open1 = int(args[3])
            close1 = int(args[4])
            packet = HubFxCommands.slave_route(
                GearControlCommands.door_config(gear_id, open0, close0, open1, close1))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response,
                                 f"Gear {gear_id} doors: A={close0}-{open0}µs, B={close1}-{open1}µs")
        except ValueError:
            self.print_error("Invalid door config parameters")

    def cmd_gc_door_mode(self, args: List[str]):
        """GearControl door activation modes (routed via hub)."""
        if not self._require_init():
            return
        if not args or len(args) < 2:
            self.print_error("Usage: gc.door.mode <gear_id> <pre_deploy> [post_deploy] [delay_ms]")
            self.print_info("  Modes: none, single, dual-sync, dual-delay, dual-seq")
            return
        MODE_MAP = {
            'none': 0, 'single': 1, 'dual-sync': 2, 'sync': 2,
            'dual-delay': 3, 'delay': 3, 'dual-seq': 4, 'seq': 4,
        }
        def _parse_mode(s: str) -> int:
            lower = s.lower()
            if lower in MODE_MAP:
                return MODE_MAP[lower]
            return int(s)
        try:
            gear_id = int(args[0])
            mode = _parse_mode(args[1])
            post_deploy = 0
            delay_ms = 500
            if len(args) > 2:
                try:
                    post_deploy = _parse_mode(args[2])
                    delay_ms = int(args[3]) if len(args) > 3 else 500
                except ValueError:
                    val = int(args[2])
                    if val > 4:
                        delay_ms = val
                    else:
                        post_deploy = val
            packet = HubFxCommands.slave_route(
                GearControlCommands.door_mode(gear_id, mode, post_deploy, delay_ms))
            success, response = self.conn.send_expect_ack(packet)
            mode_name = DoorMode.name(mode)
            post_name = DoorMode.name(post_deploy)
            msg = f"Gear {gear_id} pre-deploy={mode_name} post-deploy={post_name}"
            if mode == DoorMode.DUAL_DELAY or post_deploy == DoorMode.DUAL_DELAY:
                msg += f" (delay={delay_ms}ms)"
            self._print_ack_nack(success, response, msg)
        except ValueError:
            self.print_error("Invalid door mode parameters")

    def cmd_gc_yaw_config(self, args: List[str]):
        """GearControl yaw servo configuration (routed via hub)."""
        if not self._require_init():
            return
        if len(args) < 4:
            self.print_error("Usage: gc.yaw.config <gear_id> <neutral_us> <min_us> <max_us>")
            return
        try:
            gear_id = int(args[0])
            neutral = int(args[1])
            min_us = int(args[2])
            max_us = int(args[3])
            packet = HubFxCommands.slave_route(
                GearControlCommands.yaw_config(gear_id, neutral, min_us, max_us))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response,
                                 f"Yaw configured: gear={gear_id}, neutral={neutral}µs, range={min_us}-{max_us}µs")
        except ValueError:
            self.print_error("Invalid yaw config parameters")

    def cmd_gc_yaw(self, args: List[str]):
        """GearControl set yaw position (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.yaw <position_us>")
            return
        try:
            position = int(args[0])
            packet = HubFxCommands.slave_route(GearControlCommands.yaw_input(position))
            success, response = self.conn.send_expect_ack(packet)
            self._print_ack_nack(success, response, f"Yaw → {position}µs")
        except ValueError:
            self.print_error("Invalid yaw position")

    def cmd_gc_calibrate(self, args: List[str]):
        """GearControl stall current calibration (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.calibrate <gear_id> | all [timeout_s]")
            return
        names = {0: "nose", 1: "left main", 2: "right main"}
        if args[0].lower() == 'all':
            timeout_s = int(args[1]) if len(args) > 1 else 0
            for gear_id in range(3):
                packet = HubFxCommands.slave_route(
                    GearControlCommands.gear_calibrate(gear_id, timeout_s))
                success, response = self.conn.send_expect_ack(packet)
                timeout_str = f" (timeout={timeout_s}s)" if timeout_s > 0 else ""
                self._print_ack_nack(success, response,
                                     f"Calibrating gear {gear_id} ({names.get(gear_id, '?')}){timeout_str}")
        else:
            try:
                gear_id = int(args[0])
                timeout_s = int(args[1]) if len(args) > 1 else 0
                packet = HubFxCommands.slave_route(
                    GearControlCommands.gear_calibrate(gear_id, timeout_s))
                success, response = self.conn.send_expect_ack(packet)
                timeout_str = f" (timeout={timeout_s}s)" if timeout_s > 0 else ""
                self._print_ack_nack(success, response,
                                     f"Calibrating gear {gear_id} ({names.get(gear_id, '?')}){timeout_str}")
            except ValueError:
                self.print_error("Invalid gear ID or timeout")

    def cmd_gc_calibrate_cancel(self, args: List[str]):
        """Cancel GearControl calibration (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.calibrate.cancel <gear_id> | all")
            return
        names = {0: "nose", 1: "left main", 2: "right main"}
        if args[0].lower() == 'all':
            for gear_id in range(3):
                packet = HubFxCommands.slave_route(
                    GearControlCommands.gear_calib_cancel(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response,
                                     f"Cancelled calibration for gear {gear_id} ({names.get(gear_id, '?')})")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(
                    GearControlCommands.gear_calib_cancel(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response,
                                     f"Cancelled calibration for gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_reset(self, args: List[str]):
        """Clear GearControl error state (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.reset <gear_id> | all")
            return
        names = {0: "nose", 1: "left main", 2: "right main"}
        if args[0].lower() == 'all':
            for gear_id in range(3):
                packet = HubFxCommands.slave_route(GearControlCommands.gear_reset(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Reset gear {gear_id} ({names.get(gear_id, '?')})")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_reset(gear_id))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Reset gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_enable(self, args: List[str]):
        """Enable GearControl gear channel (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.enable <gear_id> | all")
            return
        names = {0: "nose", 1: "left main", 2: "right main"}
        if args[0].lower() == 'all':
            for gear_id in range(3):
                packet = HubFxCommands.slave_route(GearControlCommands.gear_enable(gear_id, True))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Enabled gear {gear_id} ({names.get(gear_id, '?')})")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_enable(gear_id, True))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Enabled gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_disable(self, args: List[str]):
        """Disable GearControl gear channel (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.disable <gear_id> | all")
            return
        names = {0: "nose", 1: "left main", 2: "right main"}
        if args[0].lower() == 'all':
            for gear_id in range(3):
                packet = HubFxCommands.slave_route(GearControlCommands.gear_enable(gear_id, False))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Disabled gear {gear_id} ({names.get(gear_id, '?')})")
        else:
            try:
                gear_id = int(args[0])
                packet = HubFxCommands.slave_route(GearControlCommands.gear_enable(gear_id, False))
                success, response = self.conn.send_expect_ack(packet)
                self._print_ack_nack(success, response, f"Disabled gear {gear_id} ({names.get(gear_id, '?')})")
            except ValueError:
                self.print_error("Invalid gear ID")

    def cmd_gc_battery(self, args: List[str]):
        """GearControl battery monitoring (routed via hub)."""
        if not self._require_init():
            return
        if not args:
            self.print_error("Usage: gc.battery <on|off> [autodeploy]")
            return
        value = args[0].lower()
        if value in ('on', '1', 'true', 'yes', 'enable'):
            enabled = True
        elif value in ('off', '0', 'false', 'no', 'disable'):
            enabled = False
        else:
            self.print_error(f"Invalid value '{args[0]}' — use 'on' or 'off'")
            return
        auto_deploy = len(args) > 1 and args[1].lower() == 'autodeploy'
        packet = HubFxCommands.slave_route(GearControlCommands.battery_config(enabled, auto_deploy))
        success, response = self.conn.send_expect_ack(packet)
        state = "ENABLED" + (" + auto-deploy" if auto_deploy else "") if enabled else "DISABLED"
        self._print_ack_nack(success, response, f"Battery monitoring: {state}")

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
