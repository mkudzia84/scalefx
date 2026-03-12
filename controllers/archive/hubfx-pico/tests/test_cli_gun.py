"""
Gun FX CLI Tests

Tests for gun effects commands: gun status, fire, ceasefire, heater, servo.
"""

import pytest
from conftest import SerialConnection


class TestGunCommands:
    """Test gun CLI commands."""
    
    # =========================================================================
    # GUN STATUS
    # =========================================================================
    
    def test_gun_status(self, fresh_pico: SerialConnection):
        """Test gun status display."""
        response = fresh_pico.send("gun status")
        # Should show connection and firing state
        assert "Gun" in response or "Connected" in response or "Firing" in response
    
    @pytest.mark.json
    def test_gun_status_json(self, fresh_pico: SerialConnection):
        """Test gun status JSON output."""
        result = fresh_pico.send_json("gun status")
        
        # Should have connection info
        assert "connected" in result
        assert isinstance(result["connected"], bool)
        
        # Should have firing state
        assert "firing" in result
        assert isinstance(result["firing"], bool)
        
        # Should have RPM
        assert "rpm" in result
        assert isinstance(result["rpm"], int)
        
        # Should have slave status
        assert "slave" in result
        slave = result["slave"]
        assert "flashActive" in slave
        assert "fanOn" in slave
        assert "heaterOn" in slave
    
    def test_gun_bare_command(self, fresh_pico: SerialConnection):
        """Test bare 'gun' command shows status."""
        response = fresh_pico.send("gun")
        assert "Gun" in response or "Status" in response or "Connected" in response
    
    # =========================================================================
    # GUN FIRE
    # =========================================================================
    
    def test_gun_fire_default(self, fresh_pico: SerialConnection):
        """Test gun fire with default RPM."""
        response = fresh_pico.send("gun fire")
        assert "Firing" in response or "fire" in response.lower() or "rpm" in response.lower()
    
    def test_gun_fire_with_rpm(self, fresh_pico: SerialConnection):
        """Test gun fire with specific RPM."""
        response = fresh_pico.send("gun fire 800")
        assert "800" in response or "Firing" in response
    
    @pytest.mark.json
    def test_gun_fire_json(self, fresh_pico: SerialConnection):
        """Test gun fire JSON output."""
        result = fresh_pico.send_json("gun fire 600")
        
        assert "command" in result
        assert result["command"] == "fire"
        assert "rpm" in result
        assert result["rpm"] == 600
    
    # =========================================================================
    # GUN CEASEFIRE
    # =========================================================================
    
    def test_gun_ceasefire(self, fresh_pico: SerialConnection):
        """Test gun ceasefire command."""
        response = fresh_pico.send("gun ceasefire")
        assert "cease" in response.lower() or "fire" in response.lower()
    
    def test_gun_stop_alias(self, fresh_pico: SerialConnection):
        """Test 'gun stop' alias for ceasefire."""
        response = fresh_pico.send("gun stop")
        assert "cease" in response.lower() or "fire" in response.lower()
    
    @pytest.mark.json
    def test_gun_ceasefire_json(self, fresh_pico: SerialConnection):
        """Test gun ceasefire JSON output."""
        result = fresh_pico.send_json("gun ceasefire")
        
        assert "command" in result
        assert result["command"] == "ceasefire"
    
    # =========================================================================
    # GUN HEATER
    # =========================================================================
    
    def test_gun_heater_on(self, fresh_pico: SerialConnection):
        """Test gun heater on command."""
        response = fresh_pico.send("gun heater on")
        assert "heater" in response.lower() or "ON" in response
    
    def test_gun_heater_off(self, fresh_pico: SerialConnection):
        """Test gun heater off command."""
        response = fresh_pico.send("gun heater off")
        assert "heater" in response.lower() or "OFF" in response
    
    @pytest.mark.json
    def test_gun_heater_json(self, fresh_pico: SerialConnection):
        """Test gun heater JSON output."""
        result = fresh_pico.send_json("gun heater on")
        
        assert "command" in result
        assert result["command"] == "heater"
        assert "state" in result
        assert result["state"] == "on"
    
    # =========================================================================
    # GUN SERVO
    # =========================================================================
    
    def test_gun_servo_valid(self, fresh_pico: SerialConnection):
        """Test gun servo with valid parameters."""
        response = fresh_pico.send("gun servo 0 1500")
        assert "servo" in response.lower() or "1500" in response
    
    def test_gun_servo_invalid_id(self, fresh_pico: SerialConnection):
        """Test gun servo rejects invalid servo ID."""
        response = fresh_pico.send("gun servo 5 1500")
        assert "error" in response.lower() or "invalid" in response.lower()
    
    def test_gun_servo_invalid_pulse(self, fresh_pico: SerialConnection):
        """Test gun servo rejects invalid pulse width."""
        response = fresh_pico.send("gun servo 0 100")
        assert "error" in response.lower() or "invalid" in response.lower()
    
    @pytest.mark.json
    def test_gun_servo_json(self, fresh_pico: SerialConnection):
        """Test gun servo JSON output."""
        result = fresh_pico.send_json("gun servo 1 1500")
        
        assert "command" in result
        assert result["command"] == "servo"
        assert "id" in result
        assert result["id"] == 1
        assert "pulseUs" in result
        assert result["pulseUs"] == 1500
    
    @pytest.mark.json
    def test_gun_servo_invalid_json(self, fresh_pico: SerialConnection):
        """Test gun servo invalid params return JSON error."""
        result = fresh_pico.send_json("gun servo 10 1500")
        
        assert "error" in result


class TestGunFireSequence:
    """Test gun firing sequence."""
    
    @pytest.mark.slow
    def test_fire_ceasefire_cycle(self, fresh_pico: SerialConnection):
        """Test fire and ceasefire sequence."""
        # Fire
        fresh_pico.send("gun fire 600")
        
        # Check status
        result = fresh_pico.send_json("gun status")
        # Note: May or may not be firing depending on hardware
        
        # Ceasefire
        fresh_pico.send("gun ceasefire")
        
        import time
        time.sleep(0.2)
        
        # Check stopped
        result = fresh_pico.send_json("gun status")
        assert result["firing"] == False
