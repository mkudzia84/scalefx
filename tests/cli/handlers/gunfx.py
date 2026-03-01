"""
GunFX Command Handlers

GunFX-specific CLI commands:
- Trigger control (on/off with RPM)
- Servo control and configuration
- Smoke heater and fan control
"""

from typing import List, Dict, Tuple, Callable

from tests.framework import GunFxCommands
from ..base import CommandHandlerBase, CommandInfo, ControllerType


class GunFxCommandHandler(CommandHandlerBase):
    """
    Handler for GunFX-specific commands.
    
    These commands are only available when connected to a GunFX controller.
    """
    
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return GunFX command registry."""
        return {
            'gunfx.trigger': (self.cmd_trigger, CommandInfo(
                'gunfx.trigger', 'gunfx.trigger on <rpm> | gunfx.trigger off [delay_ms]',
                'Control firing (1-3000 RPM)', requires_init=True, controller=ControllerType.GUNFX)),
            'gunfx.servo': (self.cmd_servo, CommandInfo(
                'gunfx.servo', 'gunfx.servo set <id> <pulse_us>',
                'Set servo position (1-3, 500-2500µs)', requires_init=True, controller=ControllerType.GUNFX)),
            'gunfx.servo.config': (self.cmd_servo_config, CommandInfo(
                'gunfx.servo.config', 'gunfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo limits', requires_init=True, controller=ControllerType.GUNFX)),
            'gunfx.servo.recoil': (self.cmd_servo_recoil, CommandInfo(
                'gunfx.servo.recoil', 'gunfx.servo.recoil <id> <jerk_us> <variance_us>',
                'Configure recoil effect', requires_init=True, controller=ControllerType.GUNFX)),
            'gunfx.smoke': (self.cmd_smoke, CommandInfo(
                'gunfx.smoke', 'gunfx.smoke heat on|off',
                'Control smoke heater', requires_init=True, controller=ControllerType.GUNFX)),
            'gunfx.smoke.config': (self.cmd_smoke_config, CommandInfo(
                'gunfx.smoke.config', 'gunfx.smoke.config <pulsing> <speed> <high> <low> <pulse_ms> <spindown_ms>',
                'Configure smoke fan', requires_init=True, controller=ControllerType.GUNFX)),
        }
    
    # =========================================================================
    # Trigger Commands
    # =========================================================================
    
    def cmd_trigger(self, args: List[str]):
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
                if success:
                    self.print_ok(f"Trigger ON at {rpm} RPM")
                else:
                    self._print_ack_response(response)
            except ValueError:
                self.print_error("Invalid RPM value")
                
        elif subcmd == 'off':
            delay = int(args[1]) if len(args) > 1 else 3000
            packet = GunFxCommands.trigger_off(delay)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Trigger OFF (spin-down: {delay}ms)")
            else:
                self._print_ack_response(response)
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'on' or 'off'")
    
    # =========================================================================
    # Servo Commands
    # =========================================================================
    
    def cmd_servo(self, args: List[str]):
        """GunFX servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: gunfx.servo set <id> <pulse_us>")
            return
        
        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = GunFxCommands.servo_set(servo_id, pulse)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} → {pulse}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo parameters")
    
    def cmd_servo_config(self, args: List[str]):
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
            if success:
                self.print_ok(f"Servo {servo_id} configured: range {min_us}-{max_us}µs, speed {speed}µs/s")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo config parameters")
    
    def cmd_servo_recoil(self, args: List[str]):
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
            if success:
                self.print_ok(f"Servo {servo_id} recoil: jerk {jerk}µs, variance ±{variance}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid recoil parameters")
    
    # =========================================================================
    # Smoke Commands
    # =========================================================================
    
    def cmd_smoke(self, args: List[str]):
        """GunFX smoke heater control."""
        if len(args) < 2 or args[0].lower() != 'heat':
            self.print_error("Usage: gunfx.smoke heat on|off")
            return
        
        on = args[1].lower() in ('on', '1', 'true', 'yes')
        packet = GunFxCommands.smoke_heat(on)
        success, response = self.conn.send_expect_ack(packet)
        if success:
            self.print_ok(f"Smoke heater {'ON' if on else 'OFF'}")
        else:
            self._print_ack_response(response)
    
    def cmd_smoke_config(self, args: List[str]):
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
            if success:
                mode = "pulsing" if pulsing else "constant"
                self.print_ok(f"Smoke fan configured: {mode}, speed {speed}, high/low {high}/{low}")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid smoke config parameters")
    
    # =========================================================================
    # Response Handling
    # =========================================================================
    
    def _print_ack_response(self, response):
        """Print ACK/NACK response."""
        from .. import parsers
        
        if response is None:
            self.print_error("No response (timeout)")
        elif response.is_nack:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
