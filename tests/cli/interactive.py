#!/usr/bin/env python3
"""
ScaleFX Interactive CLI

Human-readable text command interface for ScaleFX controllers.
Translates text commands to binary COBS protocol.

Features:
- Dynamic command detection based on connected controller type
- Board-prefixed commands (gunfx.*, lightfx.*)
- Core commands always available
- Controller-specific commands available after INIT

Usage:
    python -m tests.cli.interactive
    python -m tests.cli.interactive --port COM3
    python -m tests.cli.interactive --port /dev/ttyACM0
"""

import sys
import os
import argparse
import readline  # For command history
from typing import Optional, List, Tuple, Dict, Callable
from dataclasses import dataclass

# Add parent to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

try:
    from colorama import init, Fore, Style
    init()
    HAS_COLOR = True
except ImportError:
    HAS_COLOR = False
    class Fore:
        RED = GREEN = YELLOW = CYAN = MAGENTA = BLUE = RESET = ""
    class Style:
        BRIGHT = RESET_ALL = ""

from tests.framework import (
    ScaleFXConnection, CommandBuilder, GunFxCommands, LightFxCommands,
    CorePacket, CoreError, GunFxError, LightFxError,
    find_ports, find_scalefx_ports
)
from tests.framework.protocol import read_u16_le, read_i16_le, read_u32_le


@dataclass
class CommandInfo:
    """Command metadata for help display."""
    name: str
    usage: str
    description: str
    requires_init: bool = False
    controller: Optional[str] = None  # None = all, 'gunfx', 'lightfx', 'noop'


class InteractiveCLI:
    """Interactive command-line interface for ScaleFX controllers."""
    
    # Controller type constants
    CTRL_GUNFX = 'gunfx'
    CTRL_LIGHTFX = 'lightfx'
    CTRL_NOOP = 'noop'
    
    def __init__(self, port: Optional[str] = None):
        self.conn: Optional[ScaleFXConnection] = None
        self.port = port
        self.running = True
        self.controller_type: Optional[str] = None
        self.controller_name: Optional[str] = None
        self.controller_version: Optional[str] = None
        
        # Build command registry with metadata
        self._build_command_registry()
    
    def _build_command_registry(self):
        """Build command handlers and help info."""
        # Core commands - always available
        self.core_commands: Dict[str, Tuple[Callable, CommandInfo]] = {
            'help': (self.cmd_help, CommandInfo('help', 'help [command]', 'Show available commands')),
            '?': (self.cmd_help, CommandInfo('?', '?', 'Show available commands')),
            'quit': (self.cmd_quit, CommandInfo('quit', 'quit', 'Exit the CLI')),
            'exit': (self.cmd_quit, CommandInfo('exit', 'exit', 'Exit the CLI')),
            'ports': (self.cmd_ports, CommandInfo('ports', 'ports', 'List available serial ports')),
            'connect': (self.cmd_connect, CommandInfo('connect', 'connect [port]', 'Connect to controller')),
            'disconnect': (self.cmd_disconnect, CommandInfo('disconnect', 'disconnect', 'Disconnect from controller')),
            'raw': (self.cmd_raw, CommandInfo('raw', 'raw <hex>', 'Send raw hex bytes')),
        }
        
        # Protocol commands - require connection
        self.protocol_commands: Dict[str, Tuple[Callable, CommandInfo]] = {
            'init': (self.cmd_init, CommandInfo('init', 'init', 'Initialize connection (sends INIT packet)')),
            'shutdown': (self.cmd_shutdown, CommandInfo('shutdown', 'shutdown', 'Safe shutdown', requires_init=True)),
            'reboot': (self.cmd_reboot, CommandInfo('reboot', 'reboot', 'Reboot device', requires_init=True)),
            'bootsel': (self.cmd_bootsel, CommandInfo('bootsel', 'bootsel', 'Enter USB bootloader', requires_init=True)),
            'status': (self.cmd_status, CommandInfo('status', 'status', 'Request device status', requires_init=True)),
            'keepalive': (self.cmd_keepalive, CommandInfo('keepalive', 'keepalive', 'Send keepalive ping', requires_init=True)),
        }
        
        # GunFX-specific commands
        self.gunfx_commands: Dict[str, Tuple[Callable, CommandInfo]] = {
            'gunfx.trigger': (self.cmd_gunfx_trigger, CommandInfo(
                'gunfx.trigger', 'gunfx.trigger on <rpm> | gunfx.trigger off [delay_ms]',
                'Control firing (1-3000 RPM)', requires_init=True, controller=self.CTRL_GUNFX)),
            'gunfx.servo': (self.cmd_gunfx_servo, CommandInfo(
                'gunfx.servo', 'gunfx.servo set <id> <pulse_us>',
                'Set servo position (1-3, 500-2500µs)', requires_init=True, controller=self.CTRL_GUNFX)),
            'gunfx.servo.config': (self.cmd_gunfx_servo_config, CommandInfo(
                'gunfx.servo.config', 'gunfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits', requires_init=True, controller=self.CTRL_GUNFX)),
            'gunfx.servo.recoil': (self.cmd_gunfx_servo_recoil, CommandInfo(
                'gunfx.servo.recoil', 'gunfx.servo.recoil <id> <jerk_us> <variance_us>',
                'Configure recoil effect', requires_init=True, controller=self.CTRL_GUNFX)),
            'gunfx.smoke': (self.cmd_gunfx_smoke, CommandInfo(
                'gunfx.smoke', 'gunfx.smoke heat on|off',
                'Control smoke heater', requires_init=True, controller=self.CTRL_GUNFX)),
            'gunfx.smoke.config': (self.cmd_gunfx_smoke_config, CommandInfo(
                'gunfx.smoke.config', 'gunfx.smoke.config <pulsing> <speed> <high> <low> <pulse_ms> <spindown_ms>',
                'Configure smoke fan', requires_init=True, controller=self.CTRL_GUNFX)),
        }
        
        # LightFX-specific commands
        self.lightfx_commands: Dict[str, Tuple[Callable, CommandInfo]] = {
            'lightfx.led': (self.cmd_lightfx_led, CommandInfo(
                'lightfx.led', 'lightfx.led set <ch> <brightness> | lightfx.led off [ch]',
                'Control LED (1-8, 0-255)', requires_init=True, controller=self.CTRL_LIGHTFX)),
            'lightfx.led.seq': (self.cmd_lightfx_led_seq, CommandInfo(
                'lightfx.led.seq', 'lightfx.led.seq clear|start|stop <ch>',
                'Control LED sequences', requires_init=True, controller=self.CTRL_LIGHTFX)),
            'lightfx.led.seq.add': (self.cmd_lightfx_led_seq_add, CommandInfo(
                'lightfx.led.seq.add', 'lightfx.led.seq.add <ch> <event> <params...>',
                'Add sequence event (on/off/flash/fadein/fadeout)', requires_init=True, controller=self.CTRL_LIGHTFX)),
            'lightfx.servo': (self.cmd_lightfx_servo, CommandInfo(
                'lightfx.servo', 'lightfx.servo set <id> <pulse_us>',
                'Set servo position (1-3)', requires_init=True, controller=self.CTRL_LIGHTFX)),
            'lightfx.servo.config': (self.cmd_lightfx_servo_config, CommandInfo(
                'lightfx.servo.config', 'lightfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo', requires_init=True, controller=self.CTRL_LIGHTFX)),
            'lightfx.power': (self.cmd_lightfx_power, CommandInfo(
                'lightfx.power', 'lightfx.power',
                'Request power status (INA226)', requires_init=True, controller=self.CTRL_LIGHTFX)),
        }
    
    @property
    def prompt(self) -> str:
        """Dynamic prompt based on connection state."""
        if self.controller_type:
            prefix = {
                self.CTRL_GUNFX: f"{Fore.RED}gunfx",
                self.CTRL_LIGHTFX: f"{Fore.BLUE}lightfx",
                self.CTRL_NOOP: f"{Fore.MAGENTA}noop",
            }.get(self.controller_type, f"{Fore.CYAN}scalefx")
            return f"{prefix}>{Style.RESET_ALL} "
        elif self.conn and self.conn.is_connected:
            return f"{Fore.YELLOW}connected>{Style.RESET_ALL} "
        return f"{Fore.CYAN}scalefx>{Style.RESET_ALL} "
    
    def get_available_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Get commands available in current state."""
        commands = dict(self.core_commands)
        
        if self.conn and self.conn.is_connected:
            commands.update(self.protocol_commands)
            
            if self.controller_type == self.CTRL_GUNFX:
                commands.update(self.gunfx_commands)
            elif self.controller_type == self.CTRL_LIGHTFX:
                commands.update(self.lightfx_commands)
            # NoOp has no additional commands
        
        return commands
    
    def print_ok(self, msg: str):
        print(f"{Fore.GREEN}✓{Style.RESET_ALL} {msg}")
    
    def print_error(self, msg: str):
        print(f"{Fore.RED}✗{Style.RESET_ALL} {msg}")
    
    def print_info(self, msg: str):
        print(f"{Fore.YELLOW}ℹ{Style.RESET_ALL} {msg}")
    
    def print_response(self, response):
        """Pretty print a response packet."""
        if response is None:
            self.print_error("No response (timeout)")
            return
        
        if response.is_ack:
            self.print_ok("ACK")
        elif response.is_nack:
            code = response.error_code
            name = GunFxError.name(code)
            if "UNKNOWN" in name:
                name = LightFxError.name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
        elif response.is_init_ready:
            self.print_ok("INIT_READY")
            self._parse_init_ready(response.payload)
        elif response.packet_type == CorePacket.STATUS:
            self.print_ok("STATUS")
            self._parse_status(response.payload)
        else:
            self.print_info(f"Response: 0x{response.packet_type:02X}, {len(response.payload)} bytes")
            if response.payload:
                print(f"  Payload: {response.payload.hex()}")
    
    def _parse_init_ready(self, payload: bytes):
        """Parse and display INIT_READY payload, detect controller type."""
        try:
            offset = 0
            
            # Device name
            name_len = payload[offset]
            offset += 1
            name = payload[offset:offset+name_len].decode('utf-8', errors='replace')
            offset += name_len
            
            # Version
            ver_len = payload[offset]
            offset += 1
            version = payload[offset:offset+ver_len].decode('utf-8', errors='replace')
            offset += ver_len
            
            # Platform
            plat_len = payload[offset]
            offset += 1
            platform = payload[offset:offset+plat_len].decode('utf-8', errors='replace')
            offset += plat_len
            
            # CPU MHz (u16)
            cpu_mhz = read_u16_le(payload, offset)
            offset += 2
            
            # Free RAM (u32)
            free_ram = read_u32_le(payload, offset)
            offset += 4
            
            # Build number (u32)
            build = read_u32_le(payload, offset)
            
            print(f"  Device:   {name}")
            print(f"  Version:  {version} (build {build})")
            print(f"  Platform: {platform} @ {cpu_mhz}MHz")
            print(f"  Free RAM: {free_ram} bytes")
            
            # Store controller info
            self.controller_name = name
            self.controller_version = version
            
            # Detect controller type from name
            name_lower = name.lower()
            if 'gunfx' in name_lower or 'gun' in name_lower:
                self.controller_type = self.CTRL_GUNFX
                self.print_info(f"Detected GunFX controller - gunfx.* commands now available")
            elif 'lightfx' in name_lower or 'light' in name_lower:
                self.controller_type = self.CTRL_LIGHTFX
                self.print_info(f"Detected LightFX controller - lightfx.* commands now available")
            elif 'noop' in name_lower:
                self.controller_type = self.CTRL_NOOP
                self.print_info(f"Detected NoOp controller - core commands only")
            else:
                self.print_info(f"Unknown controller type: {name}")
                
            print(f"  Type 'help' to see available commands")
                
        except (IndexError, KeyError):
            print(f"  Raw: {payload.hex()}")
    
    def _parse_status(self, payload: bytes):
        """Parse and display STATUS payload."""
        if len(payload) == 0:
            return
        print(f"  Raw: {payload.hex()}")
    
    def run(self):
        """Main command loop."""
        print(f"\n{Fore.CYAN}╔══════════════════════════════════════════╗{Style.RESET_ALL}")
        print(f"{Fore.CYAN}║{Style.RESET_ALL}       ScaleFX Interactive CLI            {Fore.CYAN}║{Style.RESET_ALL}")
        print(f"{Fore.CYAN}╚══════════════════════════════════════════╝{Style.RESET_ALL}")
        print(f"Type 'help' for commands, 'quit' to exit")
        print(f"Commands are context-sensitive based on connected controller\n")
        
        if self.port:
            self.cmd_connect([self.port])
        
        while self.running:
            try:
                line = input(self.prompt).strip()
                if not line:
                    continue
                
                parts = line.split()
                cmd = parts[0].lower()
                args = parts[1:]
                
                available = self.get_available_commands()
                
                if cmd in available:
                    handler, info = available[cmd]
                    
                    # Check requirements
                    if info.requires_init and (not self.conn or not self.conn.is_initialized):
                        self.print_error("This command requires initialization. Run 'init' first.")
                        continue
                    
                    handler(args)
                else:
                    # Try to give helpful suggestion
                    self._suggest_command(cmd)
                    
            except KeyboardInterrupt:
                print()
                continue
            except EOFError:
                self.cmd_quit([])
    
    def _suggest_command(self, cmd: str):
        """Suggest similar command or explain why it's not available."""
        # Check if it's a controller-specific command for wrong controller
        if cmd.startswith('gunfx.') and self.controller_type != self.CTRL_GUNFX:
            if self.controller_type:
                self.print_error(f"'{cmd}' is a GunFX command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires GunFX controller. Run 'init' first.")
            return
        
        if cmd.startswith('lightfx.') and self.controller_type != self.CTRL_LIGHTFX:
            if self.controller_type:
                self.print_error(f"'{cmd}' is a LightFX command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires LightFX controller. Run 'init' first.")
            return
        
        # Check if command exists but needs init
        all_cmds = {}
        all_cmds.update(self.protocol_commands)
        all_cmds.update(self.gunfx_commands)
        all_cmds.update(self.lightfx_commands)
        
        if cmd in all_cmds:
            _, info = all_cmds[cmd]
            if info.requires_init:
                self.print_error(f"'{cmd}' requires connection and initialization. Run 'connect' then 'init' first.")
                return
        
        self.print_error(f"Unknown command: {cmd}. Type 'help' for available commands.")
    
    # =========================================================================
    # Help Command
    # =========================================================================
    
    def cmd_help(self, args: List[str]):
        """Show available commands based on current state."""
        available = self.get_available_commands()
        
        # Group commands by category
        core_cmds = []
        protocol_cmds = []
        controller_cmds = []
        
        for name, (handler, info) in sorted(available.items()):
            if name in self.core_commands:
                core_cmds.append(info)
            elif name in self.protocol_commands:
                protocol_cmds.append(info)
            else:
                controller_cmds.append(info)
        
        # Print header with connection status
        print()
        if self.controller_type:
            ctrl_color = {
                self.CTRL_GUNFX: Fore.RED,
                self.CTRL_LIGHTFX: Fore.BLUE,
                self.CTRL_NOOP: Fore.MAGENTA,
            }.get(self.controller_type, Fore.CYAN)
            print(f"Connected to: {ctrl_color}{self.controller_name}{Style.RESET_ALL} v{self.controller_version}")
        elif self.conn and self.conn.is_connected:
            print(f"Connected (not initialized) - run 'init' to detect controller")
        else:
            print("Not connected - run 'connect' to connect to a controller")
        print()
        
        # Core commands
        print(f"{Fore.YELLOW}━━━ Core Commands ━━━{Style.RESET_ALL}")
        for info in core_cmds:
            if info.name != '?':  # Skip alias
                print(f"  {Fore.GREEN}{info.usage:<40}{Style.RESET_ALL} {info.description}")
        print()
        
        # Protocol commands
        if protocol_cmds:
            print(f"{Fore.YELLOW}━━━ Protocol Commands ━━━{Style.RESET_ALL}")
            for info in protocol_cmds:
                status = ""
                if info.requires_init and (not self.conn or not self.conn.is_initialized):
                    status = f" {Fore.RED}(requires init){Style.RESET_ALL}"
                print(f"  {Fore.GREEN}{info.usage:<40}{Style.RESET_ALL} {info.description}{status}")
            print()
        
        # Controller-specific commands
        if controller_cmds:
            prefix = self.controller_type or "controller"
            color = {
                self.CTRL_GUNFX: Fore.RED,
                self.CTRL_LIGHTFX: Fore.BLUE,
            }.get(self.controller_type, Fore.CYAN)
            
            print(f"{color}━━━ {prefix.upper()} Commands ━━━{Style.RESET_ALL}")
            for info in controller_cmds:
                print(f"  {Fore.GREEN}{info.usage:<55}{Style.RESET_ALL}")
                print(f"      {info.description}")
            print()
        
        # Show what's not available
        if not self.controller_type:
            print(f"{Fore.YELLOW}━━━ After Initialization ━━━{Style.RESET_ALL}")
            print(f"  Additional commands will be available based on detected controller type:")
            print(f"  - {Fore.RED}GunFX{Style.RESET_ALL}: gunfx.trigger, gunfx.servo, gunfx.smoke, ...")
            print(f"  - {Fore.BLUE}LightFX{Style.RESET_ALL}: lightfx.led, lightfx.servo, lightfx.power, ...")
            print(f"  - {Fore.MAGENTA}NoOp{Style.RESET_ALL}: Core commands only (protocol testing)")
            print()
    
    # =========================================================================
    # Core Commands
    # =========================================================================
    
    def cmd_quit(self, args: List[str]):
        """Exit CLI."""
        if self.conn and self.conn.is_connected:
            self.print_info("Disconnecting...")
            self.conn.close()
        self.running = False
        print("Goodbye!")
    
    def cmd_ports(self, args: List[str]):
        """List serial ports."""
        print(f"\n{Fore.YELLOW}Available Ports:{Style.RESET_ALL}")
        
        scalefx = find_scalefx_ports()
        if scalefx:
            print(f"\n  {Fore.GREEN}Detected ScaleFX devices:{Style.RESET_ALL}")
            for port, desc in scalefx:
                print(f"    {port} - {desc}")
        
        all_ports = find_ports()
        if all_ports:
            print(f"\n  All ports:")
            for port in all_ports:
                print(f"    {port}")
        else:
            print("  No serial ports found")
        print()
    
    def cmd_connect(self, args: List[str]):
        """Connect to a controller."""
        if self.conn and self.conn.is_connected:
            self.print_info("Already connected. Use 'disconnect' first.")
            return
        
        port = args[0] if args else None
        if not port:
            scalefx = find_scalefx_ports()
            if scalefx:
                port = scalefx[0][0]
                self.print_info(f"Auto-detected: {port}")
            else:
                self.print_error("No port specified and none detected. Use 'ports' to list.")
                return
        
        self.conn = ScaleFXConnection(port=port)
        self.print_info(f"Connecting to {port}...")
        
        if self.conn.connect(init=False):
            self.print_ok(f"Connected to {port}")
            self.print_info("Run 'init' to initialize and detect controller type")
        else:
            self.print_error(f"Failed to connect to {port}")
            self.conn = None
    
    def cmd_disconnect(self, args: List[str]):
        """Disconnect from controller."""
        if not self.conn:
            self.print_info("Not connected")
            return
        
        self.conn.close()
        self.conn = None
        self.controller_type = None
        self.controller_name = None
        self.controller_version = None
        self.print_ok("Disconnected")
    
    def cmd_raw(self, args: List[str]):
        """Send raw hex bytes."""
        if not self.conn or not self.conn.is_connected:
            self.print_error("Not connected")
            return
        
        if not args:
            self.print_error("Usage: raw <hex_bytes>")
            return
        
        try:
            hex_str = ''.join(args)
            data = bytes.fromhex(hex_str)
            self.print_info(f"Sending {len(data)} bytes: {data.hex()}")
            self.conn.send(data)
            response = self.conn.receive()
            self.print_response(response)
        except ValueError:
            self.print_error("Invalid hex string")
    
    # =========================================================================
    # Protocol Commands
    # =========================================================================
    
    def _require_connection(self) -> bool:
        if not self.conn or not self.conn.is_connected:
            self.print_error("Not connected. Use 'connect' first.")
            return False
        return True
    
    def _require_init(self) -> bool:
        if not self._require_connection():
            return False
        if not self.conn.is_initialized:
            self.print_error("Not initialized. Use 'init' first.")
            return False
        return True
    
    def cmd_init(self, args: List[str]):
        """Send INIT command."""
        if not self._require_connection():
            return
        
        response = self.conn.send_and_wait(CommandBuilder.init())
        self.print_response(response)
        
        if response and response.is_init_ready:
            self.conn._initialized = True
    
    def cmd_shutdown(self, args: List[str]):
        """Send SHUTDOWN command."""
        if not self._require_init():
            return
        success, response = self.conn.send_expect_ack(CommandBuilder.shutdown())
        self.print_response(response)
    
    def cmd_reboot(self, args: List[str]):
        """Send REBOOT command."""
        if not self._require_init():
            return
        self.print_info("Sending REBOOT (no response expected)...")
        self.conn.send(CommandBuilder.reboot())
        self.conn._initialized = False
        self.controller_type = None
    
    def cmd_bootsel(self, args: List[str]):
        """Send BOOTSEL command."""
        if not self._require_init():
            return
        self.print_info("Sending BOOTSEL (device will become mass storage)...")
        self.conn.send(CommandBuilder.bootsel())
        self.conn._initialized = False
        self.controller_type = None
    
    def cmd_status(self, args: List[str]):
        """Request status."""
        if not self._require_init():
            return
        response = self.conn.send_and_wait(CommandBuilder.status_req())
        self.print_response(response)
    
    def cmd_keepalive(self, args: List[str]):
        """Send keepalive."""
        if not self._require_init():
            return
        success, response = self.conn.send_expect_ack(CommandBuilder.keepalive())
        self.print_response(response)
    
    # =========================================================================
    # GunFX Commands
    # =========================================================================
    
    def cmd_gunfx_trigger(self, args: List[str]):
        """GunFX trigger control."""
        if not args:
            self.print_error("Usage: gunfx.trigger on <rpm> | gunfx.trigger off [delay_ms]")
            return
        
        subcmd = args[0].lower()
        
        if subcmd == 'on':
            if len(args) < 2:
                self.print_error("Usage: gunfx.trigger on <rpm>")
                return
            try:
                rpm = int(args[1])
                packet = GunFxCommands.trigger_on(rpm)
                success, response = self.conn.send_expect_ack(packet)
                self.print_response(response)
            except ValueError:
                self.print_error("Invalid RPM value")
                
        elif subcmd == 'off':
            delay = int(args[1]) if len(args) > 1 else 3000
            packet = GunFxCommands.trigger_off(delay)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'on' or 'off'")
    
    def cmd_gunfx_servo(self, args: List[str]):
        """GunFX servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: gunfx.servo set <id> <pulse_us>")
            return
        
        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = GunFxCommands.servo_set(servo_id, pulse)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        except ValueError:
            self.print_error("Invalid servo parameters")
    
    def cmd_gunfx_servo_config(self, args: List[str]):
        """GunFX servo configuration."""
        if len(args) < 3:
            self.print_error("Usage: gunfx.servo.config <id> <min> <max> [speed] [accel] [decel]")
            return
        
        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000
            
            packet = GunFxCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        except ValueError:
            self.print_error("Invalid servo config parameters")
    
    def cmd_gunfx_servo_recoil(self, args: List[str]):
        """GunFX servo recoil configuration."""
        if len(args) < 3:
            self.print_error("Usage: gunfx.servo.recoil <id> <jerk_us> <variance_us>")
            return
        
        try:
            servo_id = int(args[0])
            jerk = int(args[1])
            variance = int(args[2])
            packet = GunFxCommands.servo_recoil(servo_id, jerk, variance)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        except ValueError:
            self.print_error("Invalid recoil parameters")
    
    def cmd_gunfx_smoke(self, args: List[str]):
        """GunFX smoke heater control."""
        if len(args) < 2 or args[0].lower() != 'heat':
            self.print_error("Usage: gunfx.smoke heat on|off")
            return
        
        on = args[1].lower() in ('on', '1', 'true', 'yes')
        packet = GunFxCommands.smoke_heat(on)
        success, response = self.conn.send_expect_ack(packet)
        self.print_response(response)
    
    def cmd_gunfx_smoke_config(self, args: List[str]):
        """GunFX smoke fan configuration."""
        if len(args) < 6:
            self.print_error("Usage: gunfx.smoke.config <pulsing:0|1> <speed> <high> <low> <pulse_ms> <spindown_ms>")
            return
        
        try:
            pulsing = args[0] in ('1', 'true', 'on')
            speed = int(args[1])
            high = int(args[2])
            low = int(args[3])
            pulse_ms = int(args[4])
            spindown_ms = int(args[5])
            
            packet = GunFxCommands.smoke_settings(pulsing, speed, high, low, pulse_ms, spindown_ms)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        except ValueError:
            self.print_error("Invalid smoke config parameters")
    
    # =========================================================================
    # LightFX Commands
    # =========================================================================
    
    def cmd_lightfx_led(self, args: List[str]):
        """LightFX LED control."""
        if not args:
            self.print_error("Usage: lightfx.led set <ch> <brightness> | lightfx.led off [ch]")
            return
        
        subcmd = args[0].lower()
        
        if subcmd == 'set':
            if len(args) < 3:
                self.print_error("Usage: lightfx.led set <channel> <brightness>")
                return
            try:
                ch = int(args[1])
                brightness = int(args[2])
                packet = LightFxCommands.led_set(ch, brightness)
                success, response = self.conn.send_expect_ack(packet)
                self.print_response(response)
            except ValueError:
                self.print_error("Invalid LED parameters")
                
        elif subcmd == 'off':
            ch = int(args[1]) if len(args) > 1 else 0
            packet = LightFxCommands.led_off(ch)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'set' or 'off'")
    
    def cmd_lightfx_led_seq(self, args: List[str]):
        """LightFX LED sequence control."""
        if not args:
            self.print_error("Usage: lightfx.led.seq clear|start|stop <ch>")
            return
        
        subcmd = args[0].lower()
        ch = int(args[1]) if len(args) > 1 else 0
        
        if subcmd == 'clear':
            packet = LightFxCommands.led_seq_clear(ch)
        elif subcmd == 'start':
            packet = LightFxCommands.led_seq_start(ch)
        elif subcmd == 'stop':
            packet = LightFxCommands.led_seq_stop(ch)
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'clear', 'start', or 'stop'")
            return
        
        success, response = self.conn.send_expect_ack(packet)
        self.print_response(response)
    
    def cmd_lightfx_led_seq_add(self, args: List[str]):
        """LightFX LED sequence add event."""
        if len(args) < 2:
            self.print_error("Usage: lightfx.led.seq.add <ch> <event> <params...>")
            self.print_info("Events: on, off, flash, fadein, fadeout")
            return
        
        try:
            ch = int(args[0])
            event = args[1].lower()
            
            if event == 'on':
                if len(args) < 4:
                    self.print_error("Usage: lightfx.led.seq.add <ch> on <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_on(ch, duration, brightness)
                
            elif event == 'off':
                if len(args) < 3:
                    self.print_error("Usage: lightfx.led.seq.add <ch> off <duration_ms>")
                    return
                duration = int(args[2])
                packet = LightFxCommands.led_seq_add_off(ch, duration)
                
            elif event == 'flash':
                if len(args) < 5:
                    self.print_error("Usage: lightfx.led.seq.add <ch> flash <interval_ms> <duration_ms> <brightness> [duty]")
                    return
                interval = int(args[2])
                duration = int(args[3])
                brightness = int(args[4])
                duty = int(args[5]) if len(args) > 5 else 50
                packet = LightFxCommands.led_seq_add_flash(ch, interval, duration, brightness, duty)
                
            elif event == 'fadein':
                if len(args) < 4:
                    self.print_error("Usage: lightfx.led.seq.add <ch> fadein <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_fade_in(ch, duration, brightness)
                
            elif event == 'fadeout':
                if len(args) < 4:
                    self.print_error("Usage: lightfx.led.seq.add <ch> fadeout <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_fade_out(ch, duration, brightness)
            else:
                self.print_error(f"Unknown event: {event}")
                self.print_info("Available: on, off, flash, fadein, fadeout")
                return
            
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
            
        except (ValueError, IndexError) as e:
            self.print_error(f"Invalid parameters: {e}")
    
    def cmd_lightfx_servo(self, args: List[str]):
        """LightFX servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: lightfx.servo set <id> <pulse_us>")
            return
        
        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = LightFxCommands.servo_set(servo_id, pulse)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        except ValueError:
            self.print_error("Invalid servo parameters")
    
    def cmd_lightfx_servo_config(self, args: List[str]):
        """LightFX servo configuration."""
        if len(args) < 3:
            self.print_error("Usage: lightfx.servo.config <id> <min> <max> [speed] [accel] [decel]")
            return
        
        try:
            servo_id = int(args[0])
            min_us = int(args[1])
            max_us = int(args[2])
            speed = int(args[3]) if len(args) > 3 else 4000
            accel = int(args[4]) if len(args) > 4 else 8000
            decel = int(args[5]) if len(args) > 5 else 8000
            
            packet = LightFxCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel)
            success, response = self.conn.send_expect_ack(packet)
            self.print_response(response)
        except ValueError:
            self.print_error("Invalid servo config parameters")
    
    def cmd_lightfx_power(self, args: List[str]):
        """Request power status (LightFX)."""
        response = self.conn.send_and_wait(LightFxCommands.power_status())
        
        if response is None:
            self.print_error("No response (timeout)")
            return
        
        if response.is_nack:
            self.print_response(response)
            return
        
        # Parse power status response
        if len(response.payload) >= 7:
            voltage_mv = read_u16_le(response.payload, 0)
            current_ma = read_i16_le(response.payload, 2)
            power_mw = read_u16_le(response.payload, 4)
            available = response.payload[6]
            
            self.print_ok("Power Status:")
            print(f"  Voltage: {voltage_mv/1000:.2f}V ({voltage_mv}mV)")
            print(f"  Current: {current_ma}mA")
            print(f"  Power:   {power_mw/1000:.2f}W ({power_mw}mW)")
            print(f"  INA226:  {'Available' if available else 'Not detected'}")
        else:
            self.print_info(f"Raw: {response.payload.hex()}")


def main():
    parser = argparse.ArgumentParser(description='ScaleFX Interactive CLI')
    parser.add_argument('--port', '-p', help='Serial port (e.g., COM3, /dev/ttyACM0)')
    args = parser.parse_args()
    
    cli = InteractiveCLI(port=args.port)
    cli.run()


if __name__ == '__main__':
    main()
