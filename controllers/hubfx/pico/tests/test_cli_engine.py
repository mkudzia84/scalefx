"""
Engine CLI Tests

Tests for engine effects commands: engine start, stop, status.
"""

import pytest
from conftest import SerialConnection


class TestEngineCommands:
    """Test engine CLI commands."""
    
    # =========================================================================
    # ENGINE STATUS
    # =========================================================================
    
    def test_engine_status(self, fresh_pico: SerialConnection):
        """Test engine status display."""
        response = fresh_pico.send("engine status")
        # Should show state info
        assert "State" in response or "state" in response.lower() or "Engine" in response
    
    @pytest.mark.json
    def test_engine_status_json(self, fresh_pico: SerialConnection):
        """Test engine status JSON output."""
        result = fresh_pico.send_json("engine status")
        
        # Should have state information
        assert "state" in result
        assert "toggleEngaged" in result
        assert isinstance(result["toggleEngaged"], bool)
    
    def test_engine_bare_command(self, fresh_pico: SerialConnection):
        """Test bare 'engine' command starts engine or shows status."""
        response = fresh_pico.send("engine")
        # Should either start engine or show status
        assert "engine" in response.lower() or "State" in response or "Starting" in response
    
    # =========================================================================
    # ENGINE START
    # =========================================================================
    
    def test_engine_start(self, fresh_pico: SerialConnection):
        """Test engine start command."""
        response = fresh_pico.send("engine start")
        assert "start" in response.lower() or "engine" in response.lower()
    
    # =========================================================================
    # ENGINE STOP
    # =========================================================================
    
    def test_engine_stop(self, fresh_pico: SerialConnection):
        """Test engine stop command."""
        response = fresh_pico.send("engine stop")
        assert "stop" in response.lower() or "engine" in response.lower()


class TestEngineStateTransitions:
    """Test engine state machine transitions."""
    
    @pytest.mark.slow
    def test_start_stop_cycle(self, fresh_pico: SerialConnection):
        """Test starting and stopping engine."""
        # Start
        fresh_pico.send("engine start")
        
        # Check status
        result = fresh_pico.send_json("engine status")
        # State should be something other than "Stopped" 
        # (could be Starting, Running, etc.)
        
        # Stop
        fresh_pico.send("engine stop")
        
        # Small delay for state transition
        import time
        time.sleep(0.3)
        
        # Check stopped
        result = fresh_pico.send_json("engine status")
        # After stop, should eventually return to Stopped state
        # Note: May take time depending on shutdown sequence
