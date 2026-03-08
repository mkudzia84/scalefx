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

Architecture:
    This module has been refactored using:
    - Command Pattern: Each command is a registered handler
    - Strategy Pattern: Controller-specific command sets
    - Separation of Concerns: Parsing, handlers, and base classes split
    
    Module structure:
    - base.py: CommandInfo, OutputMixin, base classes
    - parsers.py: Response payload parsing
    - handlers/core.py: Connection and protocol commands
    - handlers/gunfx.py: GunFX-specific commands
    - handlers/lightfx.py: LightFX-specific commands
    - handlers/gearcontrol.py: GearControl-specific commands
"""

import sys
import os
import argparse
import threading
import time
from queue import Queue, Empty

# readline for command history (pyreadline3 on Windows)
try:
    import readline
except ImportError:
    try:
        import pyreadline3 as readline
    except ImportError:
        readline = None  # Command history won't work

from typing import Optional, List, Dict, Callable, Tuple

# Add parent to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from tests.framework import ScaleFXConnection, GearControlPacket, LightFxPacket, HubFxPacket, CorePacket
from tests.framework.protocol import parse_packet, build_packet
from tests.framework.connection import Response

from .base import (
    CommandInfo, OutputMixin, ControllerType,
    get_prompt, Fore, Style
)
from . import parsers
from .handlers import CoreCommandHandler, GearControlCommandHandler, GunFxCommandHandler, HubFxCommandHandler, LightFxCommandHandler


class InteractiveCLI(OutputMixin):
    """
    Interactive command-line interface for ScaleFX controllers.
    
    Uses the Command Pattern with handler classes for different command groups:
    - CoreCommandHandler: Connection, protocol commands (always available)
    - GearControlCommandHandler: GearControl-specific (after init on GearControl device)
    - GunFxCommandHandler: GunFX-specific (after init on GunFX device)
    - LightFxCommandHandler: LightFX-specific (after init on LightFX device)
    """
    
    def __init__(self, port: Optional[str] = None):
        self.conn: Optional[ScaleFXConnection] = None
        self.port = port
        self.running = True
        self.controller_type: Optional[str] = None
        self.controller_name: Optional[str] = None
        self.controller_version: Optional[str] = None
        
        # Async listener state
        self._listener_thread: Optional[threading.Thread] = None
        self._listener_stop = threading.Event()
        self._response_queue: Queue = Queue()  # Async messages received while idle
        
        # Keepalive interval (seconds) — sent automatically while idle
        self.KEEPALIVE_INTERVAL = 5.0
        
        # Initialize command handlers
        self._init_handlers()
    
    def _init_handlers(self):
        """Initialize command handler instances."""
        # Core handler - always active
        self.core_handler = CoreCommandHandler()
        self.core_handler.on_controller_detected(self._on_controller_detected)
        self.core_handler.on_quit(self._on_quit)
        
        # Controller-specific handlers
        self.gearcontrol_handler = GearControlCommandHandler()
        self.gunfx_handler = GunFxCommandHandler()
        self.hubfx_handler = HubFxCommandHandler()
        self.lightfx_handler = LightFxCommandHandler()
        
        # All handlers for connection sync
        self._handlers = [
            self.core_handler,
            self.gearcontrol_handler,
            self.gunfx_handler,
            self.hubfx_handler,
            self.lightfx_handler,
        ]
    
    def _sync_connection(self):
        """Sync connection state between CLI and all handlers.
        
        If the core handler has a connection that the CLI doesn't know about
        (e.g., after 'connect' command), pull it back to the CLI first.
        Similarly, if the core handler disconnected, clear the CLI's connection.
        """
        # Pull connection state from core handler back to CLI
        handler_conn = self.core_handler.conn
        if handler_conn is not self.conn:
            self.conn = handler_conn
        
        # Pull controller type from core handler
        if self.core_handler.controller_type and self.core_handler.controller_type != self.controller_type:
            self.controller_type = self.core_handler.controller_type
        elif handler_conn is None:
            self.controller_type = None
        
        # Push to all handlers
        for handler in self._handlers:
            handler.set_connection(self.conn)
            handler.set_controller_type(self.controller_type)
    
    def _on_controller_detected(self, ctrl_type: str, name: str, version: str):
        """Callback when controller type is detected."""
        self.controller_type = ctrl_type
        self.controller_name = name
        self.controller_version = version
        self._sync_connection()
    
    def _on_quit(self):
        """Callback for quit command."""
        self.running = False
        self._stop_listener()
    
    # =========================================================================
    # Async Listener
    # =========================================================================
    
    @property
    def _debug(self) -> bool:
        """Check if verbose/debug mode is enabled."""
        return os.environ.get('SCALEFX_VERBOSE', '').lower() in ('1', 'true', 'yes')

    def _start_listener(self):
        """Start background thread that reads async packets from serial."""
        if self._listener_thread and self._listener_thread.is_alive():
            return
        
        self._listener_stop.clear()
        self._listener_thread = threading.Thread(
            target=self._listener_loop, daemon=True, name="async-listener")
        self._listener_thread.start()
        if self._debug:
            buf_size = len(self.conn._rx_buffer) if self.conn else 0
            print(f"  {Fore.MAGENTA}[listener] started (conn._rx_buffer={buf_size}B){Style.RESET_ALL}")
    
    def _stop_listener(self):
        """Stop the background listener thread."""
        self._listener_stop.set()
        if self._listener_thread:
            self._listener_thread.join(timeout=2.0)
            if self._listener_thread.is_alive():
                if self._debug:
                    print(f"  {Fore.RED}[listener] WARNING: thread did not stop within 2s!{Style.RESET_ALL}")
            self._listener_thread = None
            if self._debug:
                buf_size = len(self.conn._rx_buffer) if self.conn else 0
                print(f"  {Fore.MAGENTA}[listener] stopped (conn._rx_buffer={buf_size}B){Style.RESET_ALL}")
    
    def _listener_loop(self):
        """Background thread: read async packets from serial and send keepalives."""
        debug = self._debug
        # Take any buffered data from the connection to maintain continuity.
        # Without this, partial packets left in conn._rx_buffer after a command
        # would corrupt the next command's response parsing (stale bytes get
        # prepended to the ACK, COBS decode fails, command times out).
        rx_buffer = bytearray()
        if self.conn and self.conn._rx_buffer:
            taken = len(self.conn._rx_buffer)
            rx_buffer.extend(self.conn._rx_buffer)
            self.conn._rx_buffer = bytearray()
            if debug and taken > 0:
                print(f"  {Fore.MAGENTA}[listener] took {taken}B from conn._rx_buffer: {rx_buffer.hex()}{Style.RESET_ALL}")
        
        last_keepalive = time.monotonic()
        keepalive_packet = build_packet(CorePacket.KEEPALIVE)
        
        while not self._listener_stop.is_set():
            if not self.conn or not self.conn.is_connected or not self.conn._serial:
                time.sleep(0.1)
                continue
            
            try:
                ser = self.conn._serial
                if ser.in_waiting:
                    data = ser.read(ser.in_waiting)
                    rx_buffer.extend(data)
                else:
                    # Send keepalive if interval elapsed and controller is initialized
                    now = time.monotonic()
                    if (self.conn.is_initialized
                            and now - last_keepalive >= self.KEEPALIVE_INTERVAL):
                        try:
                            with self.conn._lock:
                                ser.write(keepalive_packet)
                                ser.flush()
                            last_keepalive = now
                            if debug:
                                print(f"  {Fore.MAGENTA}[listener] keepalive sent{Style.RESET_ALL}")
                        except Exception:
                            pass  # Best-effort
                    time.sleep(0.02)
                    continue
            except Exception:
                time.sleep(0.1)
                continue
            
            # Process complete packets (0x00 delimited)
            while 0x00 in rx_buffer:
                idx = rx_buffer.index(0x00)
                packet_data = bytes(rx_buffer[:idx])
                rx_buffer = rx_buffer[idx + 1:]
                
                if not packet_data:
                    continue
                
                delimiter = b'\x00'
                parsed = parse_packet(packet_data + delimiter)
                if not parsed:
                    if debug:
                        print(f"  {Fore.RED}[listener] unparseable packet: {packet_data.hex()}{Style.RESET_ALL}")
                    continue
                
                ptype, tag, payload = parsed
                
                # Silently consume all ACKs in the listener — they are either
                # keepalive responses or stale command ACKs, not user-relevant
                if ptype == CorePacket.ACK:
                    if debug:
                        print(f"  {Fore.MAGENTA}[listener] ACK tag={tag} (suppressed){Style.RESET_ALL}")
                    continue
                
                if debug:
                    pname = parsers.packet_type_name(ptype)
                    print(f"  {Fore.MAGENTA}[listener] received {pname} tag={tag} len={len(payload)}{Style.RESET_ALL}")
                response = Response(ptype, tag, payload, packet_data)
                self._print_async_message(response)
        
        # Always save buffer back to connection (even if empty) to prevent
        # stale partial data from persisting across listener start/stop cycles
        if self.conn:
            self.conn._rx_buffer = rx_buffer
            if debug and len(rx_buffer) > 0:
                print(f"  {Fore.MAGENTA}[listener] saved {len(rx_buffer)}B to conn._rx_buffer: {rx_buffer.hex()}{Style.RESET_ALL}")
    
    def _print_async_message(self, response: Response):
        """Print an unsolicited async packet while at the prompt."""
        ptype = response.packet_type
        payload = response.payload
        
        # Clear current input line, print message, re-show prompt
        sys.stdout.write('\r\033[K')  # Clear line
        
        if ptype == GearControlPacket.GEAR_CALIB_STATUS:
            parsers.parse_gear_calib_status(payload)
        elif ptype == GearControlPacket.GEAR_SEQ_STATUS:
            parsers.parse_gear_seq_status(payload)
        elif ptype == GearControlPacket.GEAR_DOOR_STATUS:
            parsers.parse_gear_door_status(payload)
        elif ptype == LightFxPacket.LANDING_LIGHT_STATUS:
            parsers.parse_landing_light_status(payload)
        elif ptype == CorePacket.LOG_MESSAGE:
            parsers.parse_log_message(payload)
        elif ptype == HubFxPacket.SLAVE_LIST_RESP:
            self.print_info("Async: SLAVE_LIST_RESP")
            parsers.parse_generic_payload(payload)
        elif ptype == CorePacket.ERROR:
            self.print_error("Async ERROR")
            parsers.parse_error_payload(payload)
        elif ptype == CorePacket.STATUS:
            self.print_info("Async STATUS")
            parsers.parse_status_payload(payload, self.controller_type)
        else:
            pname = parsers.packet_type_name(ptype)
            self.print_info(f"Async: {pname}")
            if payload:
                parsers.parse_generic_payload(payload)
        
        # Re-display prompt
        sys.stdout.write(self.prompt)
        sys.stdout.flush()
    
    @property
    def prompt(self) -> str:
        """Dynamic prompt based on connection state."""
        is_connected = self.conn and self.conn.is_connected
        return get_prompt(self.controller_type, is_connected)
    
    def get_available_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Get commands available in current state."""
        commands = {}
        
        # Core commands always available
        commands.update(self.core_handler.get_commands())
        
        # Protocol commands when connected
        if self.conn and self.conn.is_connected:
            commands.update(self.core_handler.get_protocol_commands())
            
            # Controller-specific commands after init
            if self.controller_type == ControllerType.GEARCONTROL:
                commands.update(self.gearcontrol_handler.get_commands())
            elif self.controller_type == ControllerType.GUNFX:
                commands.update(self.gunfx_handler.get_commands())
            elif self.controller_type == ControllerType.HUBFX:
                commands.update(self.hubfx_handler.get_commands())
            elif self.controller_type == ControllerType.LIGHTFX:
                commands.update(self.lightfx_handler.get_commands())
            # NoOp has no additional commands
        
        return commands
    
    def run(self):
        """Main command loop."""
        print(f"\n{Fore.CYAN}╔══════════════════════════════════════════╗{Style.RESET_ALL}")
        print(f"{Fore.CYAN}║{Style.RESET_ALL}       ScaleFX Interactive CLI            {Fore.CYAN}║{Style.RESET_ALL}")
        print(f"{Fore.CYAN}╚══════════════════════════════════════════╝{Style.RESET_ALL}")
        print(f"Type 'help' for commands, 'quit' to exit")
        print(f"Commands are context-sensitive based on connected controller\n")
        
        if self.port:
            self._do_connect(self.port)
        
        while self.running:
            try:
                # Drain any queued responses that arrived between commands
                self._drain_async_queue()
                
                line = input(self.prompt).strip()
                if not line:
                    continue
                
                parts = line.split()
                cmd = parts[0].lower()
                args = parts[1:]
                
                available = self.get_available_commands()
                
                if cmd == 'help' or cmd == '?':
                    self._cmd_help(args, available)
                elif cmd in available:
                    handler, info = available[cmd]
                    
                    # Check requirements
                    if info.requires_init and (not self.conn or not self.conn.is_initialized):
                        self.print_error("This command requires initialization. Run 'init' first.")
                        continue
                    
                    # Pause listener while command uses serial directly
                    self._stop_listener()
                    handler(args)
                    
                    # Sync connection after command (may have changed)
                    self._sync_connection()
                    
                    # Restart listener if still connected
                    if self.conn and self.conn.is_connected:
                        self._start_listener()
                else:
                    self._suggest_command(cmd)
                    
            except KeyboardInterrupt:
                print()
                continue
            except EOFError:
                self._on_quit()
    
    def _do_connect(self, port: str):
        """Internal connect helper."""
        self._stop_listener()
        self.core_handler.set_connection(None)
        self.conn = ScaleFXConnection(port=port)
        if self.conn.connect(init=False):
            self.core_handler.set_connection(self.conn)
            self._sync_connection()
            self.print_ok(f"Connected to {port}")
            # Auto-discover controller type via IDENTIFY, then INIT if needed
            self.core_handler._identify_and_init()
            self._sync_connection()  # Re-sync after detection
            self._start_listener()
        else:
            self.print_error(f"Failed to connect to {port}")
            self.conn = None
    
    def _drain_async_queue(self):
        """Print any async responses that arrived while a command was running."""
        while not self._response_queue.empty():
            try:
                response = self._response_queue.get_nowait()
                self._print_async_message(response)
            except Empty:
                break
    
    def _suggest_command(self, cmd: str):
        """Suggest similar command or explain why it's not available."""
        # Check if it's a controller-specific command for wrong controller
        if cmd.startswith('gc.') and self.controller_type != ControllerType.GEARCONTROL:
            if self.controller_type == ControllerType.HUBFX:
                self.print_error(f"Use 'slave {cmd}' to route to GearControl via hub")
            elif self.controller_type:
                self.print_error(f"'{cmd}' is a GearControl command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires GearControl controller. Run 'init' first.")
            return
        
        if cmd.startswith('gunfx.') and self.controller_type != ControllerType.GUNFX:
            if self.controller_type:
                self.print_error(f"'{cmd}' is a GunFX command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires GunFX controller. Run 'init' first.")
            return
        
        if cmd.startswith(('lfx.', 'lightfx.')) and self.controller_type != ControllerType.LIGHTFX:
            if self.controller_type == ControllerType.HUBFX:
                self.print_error(f"Use 'slave {cmd}' to route to LightFX via hub")
            elif self.controller_type:
                self.print_error(f"'{cmd}' is a LightFX command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires LightFX controller. Run 'init' first.")
            return

        if cmd.startswith('gfx.'):
            if self.controller_type == ControllerType.HUBFX:
                self.print_error(f"Use 'slave {cmd}' to route to GunFX via hub")
            elif self.controller_type:
                self.print_error(f"'{cmd}' is a GunFX slave command (HubFX only)")
            else:
                self.print_error(f"'{cmd}' requires HubFX controller. Run 'init' first.")
            return
        
        hubfx_prefixes = ('hub.', 'audio.', 'engine.', 'config.', 'sd.')
        if any(cmd.startswith(p) for p in hubfx_prefixes) and self.controller_type != ControllerType.HUBFX:
            if self.controller_type:
                self.print_error(f"'{cmd}' is a HubFX command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires HubFX controller. Run 'init' first.")
            return
        
        # Check if command exists but needs init
        all_cmds = {}
        all_cmds.update(self.core_handler.get_protocol_commands())
        all_cmds.update(self.gearcontrol_handler.get_commands())
        all_cmds.update(self.gunfx_handler.get_commands())
        all_cmds.update(self.hubfx_handler.get_commands())
        all_cmds.update(self.lightfx_handler.get_commands())
        
        if cmd in all_cmds:
            _, info = all_cmds[cmd]
            if info.requires_init:
                self.print_error(f"'{cmd}' requires connection and initialization. Run 'connect' then 'init' first.")
                return
        
        self.print_error(f"Unknown command: {cmd}. Type 'help' for available commands.")
    
    def _cmd_help(self, args: List[str], available: Dict[str, Tuple[Callable, CommandInfo]]):
        """Show available commands based on current state.
        
        Usage:
            help          - Show overview (groups shown as summaries for HubFX)
            help <group>  - Show commands in a specific group (e.g. help audio)
            help gfx|lfx|gc - Show slave sub-commands for that controller
        """
        from collections import OrderedDict

        # Categorize all commands
        core_cmds = []
        protocol_cmds = []
        controller_cmds = []
        
        core_cmd_names = set(self.core_handler.get_commands().keys())
        protocol_cmd_names = set(self.core_handler.get_protocol_commands().keys())
        
        for name, (handler, info) in sorted(available.items()):
            if name in core_cmd_names:
                core_cmds.append(info)
            elif name in protocol_cmd_names:
                protocol_cmds.append(info)
            else:
                controller_cmds.append(info)

        # Build groups dict from controller commands (preserves insertion order)
        has_groups = any(info.group for info in controller_cmds)
        groups: OrderedDict[str, list] = OrderedDict()
        ungrouped = []
        if has_groups:
            for info in controller_cmds:
                if info.group:
                    groups.setdefault(info.group, []).append(info)
                else:
                    ungrouped.append(info)

        # Build slave sub-groups if HubFX is connected
        slave_groups: OrderedDict[str, list] = OrderedDict()
        if self.controller_type == ControllerType.HUBFX:
            slave_cmds = self.hubfx_handler.get_slave_commands()
            for _subcmd, (_handler, info) in slave_cmds.items():
                if info.group:
                    slave_groups.setdefault(info.group, []).append(info)

        # Build a lookup: lowercase alias → (source, group_name)
        # source = 'group' for controller groups, 'slave' for slave sub-groups
        group_aliases = {}
        for gname in groups:
            key = gname.lower()
            group_aliases[key] = ('group', gname)
            first_word = key.split()[0]
            group_aliases[first_word] = ('group', gname)
        for gname in slave_groups:
            key = gname.lower()
            group_aliases[key] = ('slave', gname)
            first_word = key.split()[0]
            group_aliases[first_word] = ('slave', gname)
        # Explicit short aliases
        _extra_groups = {
            'files': ('group', 'SD Card & Files'),
            'engine': ('group', 'Engine FX'),
            'slaves': ('group', 'Hub Management'),
            'hub': ('group', 'Hub Management'),
        }
        _extra_slave = {
            'gfx': ('slave', 'GunFX (gfx.*)'),
            'gunfx': ('slave', 'GunFX (gfx.*)'),
            'lfx': ('slave', 'LightFX (lfx.*)'),
            'lightfx': ('slave', 'LightFX (lfx.*)'),
            'gc': ('slave', 'GearControl (gc.*)'),
            'gearcontrol': ('slave', 'GearControl (gc.*)'),
            'gear': ('slave', 'GearControl (gc.*)'),
        }
        for alias, val in {**_extra_groups, **_extra_slave}.items():
            source, gname = val
            target = groups if source == 'group' else slave_groups
            if gname in target:
                group_aliases[alias] = val

        # ── "help <group>" — show a single group's or slave group's commands ──
        if args and (has_groups or slave_groups):
            query = args[0].lower()
            # Special case: "help slave" shows all slave sub-groups
            if query == 'slave' and slave_groups:
                color = Fore.CYAN
                print()
                print(f"{color}━━━ Slave Controllers ━━━{Style.RESET_ALL}")
                print(f"  Usage: {Fore.YELLOW}slave <subcmd> [args...]{Style.RESET_ALL}\n")
                for sg_name, sg_infos in slave_groups.items():
                    print(f"{color}  ── {sg_name} ──{Style.RESET_ALL}")
                    for info in sg_infos:
                        print(f"    {Fore.GREEN}{info.usage:<55}{Style.RESET_ALL} {info.description}")
                    print()
                return

            matched = group_aliases.get(query)
            if matched:
                source, gname = matched
                target = groups if source == 'group' else slave_groups
                if gname in target:
                    color = {
                        ControllerType.GEARCONTROL: Fore.GREEN,
                        ControllerType.GUNFX: Fore.RED,
                        ControllerType.HUBFX: Fore.CYAN,
                        ControllerType.LIGHTFX: Fore.BLUE,
                    }.get(self.controller_type, Fore.CYAN)
                    print()
                    if source == 'slave':
                        print(f"{color}━━━ {gname} ━━━{Style.RESET_ALL}")
                        print(f"  Usage: {Fore.YELLOW}slave <subcmd> [args...]{Style.RESET_ALL}\n")
                    else:
                        print(f"{color}━━━ {gname} ━━━{Style.RESET_ALL}")
                    for info in target[gname]:
                        print(f"  {Fore.GREEN}{info.usage:<55}{Style.RESET_ALL} {info.description}")
                    print()
                    return

            all_known = set()
            for _src, gn in group_aliases.values():
                all_known.add(gn)
            self.print_error(f"Unknown group '{args[0]}'. Available: {', '.join(sorted(all_known))}")
            return

        # ── Full help output ──
        # Connection header
        print()
        if self.controller_type:
            ctrl_color = {
                ControllerType.GEARCONTROL: Fore.GREEN,
                ControllerType.GUNFX: Fore.RED,
                ControllerType.HUBFX: Fore.CYAN,
                ControllerType.LIGHTFX: Fore.BLUE,
                ControllerType.NOOP: Fore.MAGENTA,
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
            color = {
                ControllerType.GEARCONTROL: Fore.GREEN,
                ControllerType.GUNFX: Fore.RED,
                ControllerType.HUBFX: Fore.CYAN,
                ControllerType.LIGHTFX: Fore.BLUE,
            }.get(self.controller_type, Fore.CYAN)
            
            if has_groups:
                # Show group summaries with command count and hint
                print(f"{color}━━━ Command Groups ━━━{Style.RESET_ALL}")
                print(f"  Type {Fore.YELLOW}help <group>{Style.RESET_ALL} to see commands in a group\n")
                for group_name, group_infos in groups.items():
                    if group_name == 'Slave Controllers':
                        continue  # Show separately below
                    cmd_names = ', '.join(info.name for info in group_infos[:4])
                    more = f", ... (+{len(group_infos)-4})" if len(group_infos) > 4 else ""
                    print(f"  {Fore.GREEN}{group_name:<22}{Style.RESET_ALL} {len(group_infos):>2} cmds  │  {cmd_names}{more}")

                # Slave controller summary
                if slave_groups:
                    print()
                    print(f"{color}━━━ Slave Controllers ━━━{Style.RESET_ALL}")
                    print(f"  Usage: {Fore.YELLOW}slave <subcmd> [args...]{Style.RESET_ALL}")
                    print(f"  Type {Fore.YELLOW}help gfx{Style.RESET_ALL}, {Fore.YELLOW}help lfx{Style.RESET_ALL}, {Fore.YELLOW}help gc{Style.RESET_ALL}, or {Fore.YELLOW}help slave{Style.RESET_ALL} for details\n")
                    for sg_name, sg_infos in slave_groups.items():
                        cmd_names = ', '.join(info.name for info in sg_infos[:4])
                        more = f", ... (+{len(sg_infos)-4})" if len(sg_infos) > 4 else ""
                        print(f"  {Fore.GREEN}{sg_name:<26}{Style.RESET_ALL} {len(sg_infos):>2} cmds  │  {cmd_names}{more}")
                print()
            else:
                # Flat list for simple controllers
                prefix = self.controller_type or "controller"
                print(f"{color}━━━ {prefix.upper()} Commands ━━━{Style.RESET_ALL}")
                for info in controller_cmds:
                    print(f"  {Fore.GREEN}{info.usage:<55}{Style.RESET_ALL}")
                    print(f"      {info.description}")
                print()
        
        # Show what's not available
        if not self.controller_type:
            print(f"{Fore.YELLOW}━━━ After Initialization ━━━{Style.RESET_ALL}")
            print(f"  Additional commands will be available based on detected controller type:")
            print(f"  - {Fore.GREEN}GearControl{Style.RESET_ALL}: gc.deploy, gc.retract, gc.servo, ...")
            print(f"  - {Fore.RED}GunFX{Style.RESET_ALL}: gunfx.trigger, gunfx.servo, gunfx.smoke, ...")
            print(f"  - {Fore.CYAN}HubFX{Style.RESET_ALL}: audio.*, engine.*, sd.*, config.* + slave gfx|lfx|gc.*")
            print(f"  - {Fore.BLUE}LightFX{Style.RESET_ALL}: lightfx.led, lightfx.servo, lightfx.power, ...")
            print(f"  - {Fore.MAGENTA}NoOp{Style.RESET_ALL}: Core commands only (protocol testing)")
            print()


def main():
    parser = argparse.ArgumentParser(description='ScaleFX Interactive CLI')
    parser.add_argument('--port', '-p', help='Serial port (e.g., COM3, /dev/ttyACM0)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Enable verbose TX/RX logging (shows all packets)')
    args = parser.parse_args()
    
    if args.verbose:
        os.environ['SCALEFX_VERBOSE'] = '1'
    
    cli = InteractiveCLI(port=args.port)
    cli.run()


if __name__ == '__main__':
    main()
