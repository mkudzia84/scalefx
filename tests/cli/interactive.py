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
"""

import sys
import os
import argparse

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

from tests.framework import ScaleFXConnection

from .base import (
    CommandInfo, OutputMixin, ControllerType,
    get_prompt, Fore, Style
)
from .handlers import CoreCommandHandler, GunFxCommandHandler, LightFxCommandHandler


class InteractiveCLI(OutputMixin):
    """
    Interactive command-line interface for ScaleFX controllers.
    
    Uses the Command Pattern with handler classes for different command groups:
    - CoreCommandHandler: Connection, protocol commands (always available)
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
        
        # Initialize command handlers
        self._init_handlers()
    
    def _init_handlers(self):
        """Initialize command handler instances."""
        # Core handler - always active
        self.core_handler = CoreCommandHandler()
        self.core_handler.on_controller_detected(self._on_controller_detected)
        self.core_handler.on_quit(self._on_quit)
        
        # Controller-specific handlers
        self.gunfx_handler = GunFxCommandHandler()
        self.lightfx_handler = LightFxCommandHandler()
        
        # All handlers for connection sync
        self._handlers = [
            self.core_handler,
            self.gunfx_handler,
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
            if self.controller_type == ControllerType.GUNFX:
                commands.update(self.gunfx_handler.get_commands())
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
                    
                    handler(args)
                    
                    # Sync connection after command (may have changed)
                    self._sync_connection()
                else:
                    self._suggest_command(cmd)
                    
            except KeyboardInterrupt:
                print()
                continue
            except EOFError:
                self._on_quit()
    
    def _do_connect(self, port: str):
        """Internal connect helper."""
        self.core_handler.set_connection(None)
        self.conn = ScaleFXConnection(port=port)
        if self.conn.connect(init=False):
            self._sync_connection()
            self.print_ok(f"Connected to {port}")
            self.print_info("Run 'init' to initialize and detect controller type")
        else:
            self.print_error(f"Failed to connect to {port}")
            self.conn = None
    
    def _suggest_command(self, cmd: str):
        """Suggest similar command or explain why it's not available."""
        # Check if it's a controller-specific command for wrong controller
        if cmd.startswith('gunfx.') and self.controller_type != ControllerType.GUNFX:
            if self.controller_type:
                self.print_error(f"'{cmd}' is a GunFX command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires GunFX controller. Run 'init' first.")
            return
        
        if cmd.startswith('lightfx.') and self.controller_type != ControllerType.LIGHTFX:
            if self.controller_type:
                self.print_error(f"'{cmd}' is a LightFX command, but you're connected to {self.controller_type}")
            else:
                self.print_error(f"'{cmd}' requires LightFX controller. Run 'init' first.")
            return
        
        # Check if command exists but needs init
        all_cmds = {}
        all_cmds.update(self.core_handler.get_protocol_commands())
        all_cmds.update(self.gunfx_handler.get_commands())
        all_cmds.update(self.lightfx_handler.get_commands())
        
        if cmd in all_cmds:
            _, info = all_cmds[cmd]
            if info.requires_init:
                self.print_error(f"'{cmd}' requires connection and initialization. Run 'connect' then 'init' first.")
                return
        
        self.print_error(f"Unknown command: {cmd}. Type 'help' for available commands.")
    
    def _cmd_help(self, args: List[str], available: Dict[str, Tuple[Callable, CommandInfo]]):
        """Show available commands based on current state."""
        # Group commands by category
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
        
        # Print header with connection status
        print()
        if self.controller_type:
            ctrl_color = {
                ControllerType.GUNFX: Fore.RED,
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
            prefix = self.controller_type or "controller"
            color = {
                ControllerType.GUNFX: Fore.RED,
                ControllerType.LIGHTFX: Fore.BLUE,
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


def main():
    parser = argparse.ArgumentParser(description='ScaleFX Interactive CLI')
    parser.add_argument('--port', '-p', help='Serial port (e.g., COM3, /dev/ttyACM0)')
    args = parser.parse_args()
    
    cli = InteractiveCLI(port=args.port)
    cli.run()


if __name__ == '__main__':
    main()
