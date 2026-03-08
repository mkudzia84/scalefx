"""
LightFX Command Handlers

LightFX-specific CLI commands (lfx.* prefix):
- LED direct control and sequences
- Servo control and configuration
- Landing light control
- Channel enable/disable and reset
"""

from typing import List, Dict, Tuple, Callable

from tests.framework import LightFxCommands, LightFxPacket
from ..base import CommandHandlerBase, CommandInfo, ControllerType
from .. import parsers


class LightFxCommandHandler(CommandHandlerBase):
    """
    Handler for LightFX-specific commands.
    
    These commands are only available when connected to a LightFX controller.
    """
    
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        """Return LightFX command registry."""
        return {
            'lfx.led': (self.cmd_led, CommandInfo(
                'lfx.led', 'lfx.led set <ch> <brightness> | lfx.led off [ch]',
                'Control LED (1-8, 0-100%)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.led.seq': (self.cmd_led_seq, CommandInfo(
                'lfx.led.seq', 'lfx.led.seq clear|start|stop|restart <ch>',
                'Control LED sequences', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.led.seq.add': (self.cmd_led_seq_add, CommandInfo(
                'lfx.led.seq.add', 'lfx.led.seq.add <ch> <event> <params...>',
                'Add sequence event (on/off/flash/fadein/fadeout)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.led.seq.status': (self.cmd_led_seq_status, CommandInfo(
                'lfx.led.seq.status', 'lfx.led.seq.status <ch>',
                'Get LED sequence status', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.led.seq.queue': (self.cmd_led_seq_queue, CommandInfo(
                'lfx.led.seq.queue', 'lfx.led.seq.queue <ch>',
                'List LED sequence event queue', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.led.status': (self.cmd_led_status, CommandInfo(
                'lfx.led.status', 'lfx.led.status',
                'Get all LED channel statuses', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.brightness': (self.cmd_brightness, CommandInfo(
                'lfx.brightness', 'lfx.brightness <0-100>',
                'Set master LED brightness (0-100%)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.servo': (self.cmd_servo, CommandInfo(
                'lfx.servo', 'lfx.servo set <id> <pulse_us>',
                'Set servo position (1-3)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.servo.config': (self.cmd_servo_config, CommandInfo(
                'lfx.servo.config', 'lfx.servo.config <id> <min> <max> [speed] [accel] [decel]',
                'Configure servo', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.ll.bind': (self.cmd_ll_bind, CommandInfo(
                'lfx.ll.bind', 'lfx.ll.bind <slot> <servo> <led_ch> <deploy_us> <retract_us> [brightness]',
                'Bind landing light (slot 1-3)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.ll.unbind': (self.cmd_ll_unbind, CommandInfo(
                'lfx.ll.unbind', 'lfx.ll.unbind [slot]',
                'Unbind landing light (0=all)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.ll.deploy': (self.cmd_ll_deploy, CommandInfo(
                'lfx.ll.deploy', 'lfx.ll.deploy [slot]',
                'Deploy landing gear + light on (0=all)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.ll.retract': (self.cmd_ll_retract, CommandInfo(
                'lfx.ll.retract', 'lfx.ll.retract [slot]',
                'Retract landing gear + light off (0=all)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.reset': (self.cmd_reset, CommandInfo(
                'lfx.reset', 'lfx.reset [ch]',
                'Reset LED channel (stop seq, off, re-enable; 0=all)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.enable': (self.cmd_enable, CommandInfo(
                'lfx.enable', 'lfx.enable <ch>',
                'Enable LED channel (1-8, 0=all)', requires_init=True, controller=ControllerType.LIGHTFX)),
            'lfx.disable': (self.cmd_disable, CommandInfo(
                'lfx.disable', 'lfx.disable <ch>',
                'Disable LED channel (1-8, 0=all)', requires_init=True, controller=ControllerType.LIGHTFX)),
        }
    
    # =========================================================================
    # LED Direct Control
    # =========================================================================
    
    def cmd_led(self, args: List[str]):
        """LightFX LED control."""
        if not args:
            self.print_error("Usage: lfx.led set <ch> <brightness> | lfx.led off [ch]")
            return
        
        subcmd = args[0].lower()
        
        if subcmd == 'set':
            if len(args) < 3:
                self.print_error("Usage: lfx.led set <channel> <brightness>")
                return
            try:
                ch = int(args[1])
                brightness = int(args[2])
                packet = LightFxCommands.led_set(ch, brightness)
                success, response = self.conn.send_expect_ack(packet)
                if success:
                    self.print_ok(f"LED {ch} → {brightness}%")
                else:
                    self._print_ack_response(response)
            except ValueError:
                self.print_error("Invalid LED parameters")
                
        elif subcmd == 'off':
            ch = int(args[1]) if len(args) > 1 else 0
            packet = LightFxCommands.led_off(ch)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"LED {ch}" if ch > 0 else "All LEDs"
                self.print_ok(f"{target} OFF")
            else:
                self._print_ack_response(response)
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'set' or 'off'")
    
    # =========================================================================
    # LED Sequence Control
    # =========================================================================
    
    def cmd_led_seq(self, args: List[str]):
        """LightFX LED sequence control."""
        if not args:
            self.print_error("Usage: lfx.led.seq clear|start|stop|restart <ch>")
            return
        
        subcmd = args[0].lower()
        ch = int(args[1]) if len(args) > 1 else 0
        
        if subcmd == 'clear':
            packet = LightFxCommands.led_seq_clear(ch)
            msg = f"LED {ch} sequence cleared"
        elif subcmd == 'start':
            packet = LightFxCommands.led_seq_start(ch)
            msg = f"LED {ch} sequence started"
        elif subcmd == 'stop':
            packet = LightFxCommands.led_seq_stop(ch)
            msg = f"LED {ch} sequence stopped"
        elif subcmd == 'restart':
            packet = LightFxCommands.led_seq_restart(ch)
            msg = f"LED {ch} sequence restarted"
        else:
            self.print_error(f"Unknown: {subcmd}. Use 'clear', 'start', 'stop', or 'restart'")
            return
        
        success, response = self.conn.send_expect_ack(packet)
        if success:
            self.print_ok(msg)
        else:
            self._print_ack_response(response)
    
    def cmd_led_seq_add(self, args: List[str]):
        """LightFX LED sequence add event."""
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
                packet = LightFxCommands.led_seq_add_on(ch, duration, brightness)
                msg = f"LED {ch}: ON for {duration}ms at brightness {brightness}"
                
            elif event == 'off':
                if len(args) < 3:
                    self.print_error("Usage: lfx.led.seq.add <ch> off <duration_ms>")
                    return
                duration = int(args[2])
                packet = LightFxCommands.led_seq_add_off(ch, duration)
                msg = f"LED {ch}: OFF for {duration}ms"
                
            elif event == 'flash':
                if len(args) < 5:
                    self.print_error("Usage: lfx.led.seq.add <ch> flash <interval_ms> <duration_ms> <brightness> [duty]")
                    return
                interval = int(args[2])
                duration = int(args[3])
                brightness = int(args[4])
                duty = int(args[5]) if len(args) > 5 else 50
                packet = LightFxCommands.led_seq_add_flash(ch, interval, duration, brightness, duty)
                msg = f"LED {ch}: FLASH {interval}ms for {duration}ms, {duty}% duty"
                
            elif event == 'fadein':
                if len(args) < 4:
                    self.print_error("Usage: lfx.led.seq.add <ch> fadein <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_fade_in(ch, duration, brightness)
                msg = f"LED {ch}: FADE IN over {duration}ms to {brightness}"
                
            elif event == 'fadeout':
                if len(args) < 4:
                    self.print_error("Usage: lfx.led.seq.add <ch> fadeout <duration_ms> <brightness>")
                    return
                duration = int(args[2])
                brightness = int(args[3])
                packet = LightFxCommands.led_seq_add_fade_out(ch, duration, brightness)
                msg = f"LED {ch}: FADE OUT over {duration}ms from {brightness}"
            else:
                self.print_error(f"Unknown event: {event}")
                self.print_info("Available: on, off, flash, fadein, fadeout")
                return
            
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Sequence event added: {msg}")
            else:
                self._print_ack_response(response)
            
        except (ValueError, IndexError) as e:
            self.print_error(f"Invalid parameters: {e}")
    
    def cmd_led_seq_status(self, args: List[str]):
        """LightFX LED sequence status."""
        if not args:
            self.print_error("Usage: lfx.led.seq.status <ch>")
            return
        
        try:
            ch = int(args[0])
            packet = LightFxCommands.led_seq_status(ch)
            response = self.conn.send_and_receive(packet)
            
            if response is None:
                self.print_error("No response (timeout)")
                return
            
            if response.packet_type == LightFxPacket.LED_SEQ_STATUS_RESP:
                status = parsers.parse_led_seq_status(response.payload)
                if status:
                    status_str = "PLAYING" if status['playing'] else "STOPPED"
                    self.print_info(f"LED {status['channel']} Sequence Status:")
                    print(f"  Status:        {status_str}")
                    print(f"  Event Count:   {status['event_count']}")
                    print(f"  Current Index: {status['current_index']}")
                    print(f"  Loop Count:    {status['loop_count']}")
                else:
                    self.print_error("Invalid response payload")
            else:
                self._print_ack_response(response)
                
        except ValueError:
            self.print_error("Invalid channel number")
    
    def cmd_led_seq_queue(self, args: List[str]):
        """LightFX LED sequence queue listing."""
        if not args:
            self.print_error("Usage: lfx.led.seq.queue <ch>")
            return
        
        try:
            ch = int(args[0])
            packet = LightFxCommands.led_seq_queue(ch)
            response = self.conn.send_and_receive(packet)
            
            if response is None:
                self.print_error("No response (timeout)")
                return
            
            if response.packet_type == LightFxPacket.LED_SEQ_QUEUE_RESP:
                queue = parsers.parse_led_seq_queue(response.payload)
                if queue:
                    status_str = "PLAYING" if queue['playing'] else "STOPPED"
                    self.print_info(f"LED {queue['channel']} Sequence Queue ({status_str}, {queue['count']} events, @ index {queue['current_index']}):")
                    
                    for event in queue['events']:
                        marker = " ← current" if event['index'] == queue['current_index'] else ""
                        print(f"  [{event['index']}] {event['type_name']}: {event['duration']}ms (param={event['param1']}){marker}")
                    
                    if queue['count'] == 0:
                        print("  (empty)")
                else:
                    self.print_error("Invalid response payload")
            else:
                self._print_ack_response(response)
                
        except ValueError:
            self.print_error("Invalid channel number")
    
    def cmd_led_status(self, args: List[str]):
        """LightFX all LED channel status."""
        packet = LightFxCommands.led_status()
        response = self.conn.send_and_receive(packet)
        
        if response is None:
            self.print_error("No response (timeout)")
            return
        
        if response.packet_type == LightFxPacket.LED_STATUS_RESP:
            channels = parsers.parse_led_status(response.payload)
            self.print_info("LED Channel Status:")
            
            for ch in channels:
                brightness = ch['brightness']
                status = "▶" if ch['seq_playing'] else "■"
                filled = brightness * 8 // 100 if brightness > 0 else 0
                bar = "█" * filled + "░" * (8 - filled)
                print(f"  CH{ch['channel']}: {bar} {brightness:3d}% | Seq: {status} ({ch['seq_count']} events)")
        else:
            self._print_ack_response(response)
    
    # =========================================================================
    # Master Brightness
    # =========================================================================
    
    def cmd_brightness(self, args: List[str]):
        """Set master LED brightness (0-100%)."""
        if not args:
            self.print_error("Usage: lfx.brightness <0-100>")
            return
        
        try:
            pct = int(args[0])
            if pct < 0 or pct > 100:
                self.print_error("Brightness must be 0-100%")
                return
            packet = LightFxCommands.led_master_brightness(pct)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Master brightness → {pct}%")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid brightness value")
    
    # =========================================================================
    # Servo Commands
    # =========================================================================
    
    def cmd_servo(self, args: List[str]):
        """LightFX servo control."""
        if len(args) < 3 or args[0].lower() != 'set':
            self.print_error("Usage: lfx.servo set <id> <pulse_us>")
            return
        
        try:
            servo_id = int(args[1])
            pulse = int(args[2])
            packet = LightFxCommands.servo_set(servo_id, pulse)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} → {pulse}µs")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo parameters")
    
    def cmd_servo_config(self, args: List[str]):
        """LightFX servo configuration."""
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
            
            packet = LightFxCommands.servo_settings(servo_id, min_us, max_us, speed, accel, decel)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(f"Servo {servo_id} configured: range {min_us}-{max_us}µs, speed {speed}µs/s")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid servo config parameters")
    
    # =========================================================================
    # Landing Light Commands
    # =========================================================================
    
    def cmd_ll_bind(self, args: List[str]):
        """Bind landing light: servo + LED channel."""
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
            
            packet = LightFxCommands.landing_light_bind(
                slot, servo_id, led_ch, deploy_us, retract_us, brightness)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                self.print_ok(
                    f"Landing light {slot}: servo {servo_id} + LED {led_ch}, "
                    f"deploy {deploy_us}µs, retract {retract_us}µs, brightness {brightness}%")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid parameters")
    
    def cmd_ll_unbind(self, args: List[str]):
        """Unbind landing light slot."""
        slot = int(args[0]) if args else 0
        try:
            packet = LightFxCommands.landing_light_unbind(slot)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"slot {slot}" if slot > 0 else "all slots"
                self.print_ok(f"Landing light {target} unbound")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid slot number")
    
    def cmd_ll_deploy(self, args: List[str]):
        """Deploy landing gear + activate light."""
        slot = int(args[0]) if args else 0
        try:
            packet = LightFxCommands.landing_light_deploy(slot)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"slot {slot}" if slot > 0 else "all"
                self.print_ok(f"Landing light {target} deploying")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid slot number")
    
    def cmd_ll_retract(self, args: List[str]):
        """Retract landing gear + deactivate light."""
        slot = int(args[0]) if args else 0
        try:
            packet = LightFxCommands.landing_light_retract(slot)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"slot {slot}" if slot > 0 else "all"
                self.print_ok(f"Landing light {target} retracting")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid slot number")
    
    # =========================================================================
    # Channel Management
    # =========================================================================
    
    def cmd_reset(self, args: List[str]):
        """Reset LED channel(s) — stop seq, turn off, re-enable."""
        ch = int(args[0]) if args else 0
        try:
            packet = LightFxCommands.led_reset(ch)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"LED {ch}" if ch > 0 else "All LEDs"
                self.print_ok(f"{target} reset")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid channel number")
    
    def cmd_enable(self, args: List[str]):
        """Enable LED channel."""
        if not args:
            self.print_error("Usage: lfx.enable <ch> (1-8, 0=all)")
            return
        try:
            ch = int(args[0])
            packet = LightFxCommands.led_enable(ch, True)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"LED {ch}" if ch > 0 else "All LEDs"
                self.print_ok(f"{target} enabled")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid channel number")
    
    def cmd_disable(self, args: List[str]):
        """Disable LED channel."""
        if not args:
            self.print_error("Usage: lfx.disable <ch> (1-8, 0=all)")
            return
        try:
            ch = int(args[0])
            packet = LightFxCommands.led_enable(ch, False)
            success, response = self.conn.send_expect_ack(packet)
            if success:
                target = f"LED {ch}" if ch > 0 else "All LEDs"
                self.print_ok(f"{target} disabled")
            else:
                self._print_ack_response(response)
        except ValueError:
            self.print_error("Invalid channel number")
    
    # =========================================================================
    # Response Handling
    # =========================================================================
    
    def _print_ack_response(self, response):
        """Print ACK/NACK response."""
        if response is None:
            self.print_error("No response (timeout)")
        elif response.is_nack:
            code = response.error_code
            name = parsers.error_name(code)
            msg = response.error_message
            self.print_error(f"NACK: {name} (0x{code:02X})" + (f" - {msg}" if msg else ""))
