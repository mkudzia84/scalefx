"""
Pytest Configuration and Fixtures

Provides shared fixtures for all ScaleFX controller tests.

Fixture Scopes:
- serial_port: Session-scoped, returns port name string
- connection: Module-scoped, shared connection (most tests use this)
- exclusive_connection: Function-scoped, closes module connection first (for destructive tests)

Usage Guidelines:
- Use `noop`, `gunfx`, `lightfx` fixtures for normal protocol tests
- Use `exclusive_connection` for tests that need a fresh connection (INIT, SHUTDOWN, REBOOT)
- Use `serial_port` when you need to manage the connection manually
"""

import os
import time
import pytest
from typing import Generator, Optional

from .framework import ScaleFXConnection, GunFxCommands, LightFxCommands, CommandBuilder


# Module-level connection storage for coordination between fixtures
_module_connection: Optional[ScaleFXConnection] = None


def pytest_addoption(parser):
    """Add command line options."""
    parser.addoption(
        "--port",
        action="store",
        default=None,
        help="Serial port to use for testing"
    )
    parser.addoption(
        "--controller",
        action="store",
        default=None,
        help="Controller type (gunfx, lightfx, noop)"
    )


@pytest.fixture(scope="session")
def serial_port(request) -> str:
    """Get serial port from command line or environment."""
    port = request.config.getoption("--port")
    if port:
        return port
    return os.environ.get("SCALEFX_PORT", "COM3")


@pytest.fixture(scope="module")
def connection(serial_port) -> Generator[ScaleFXConnection, None, None]:
    """
    Module-scoped connection fixture.
    
    Creates a single connection shared across all tests in a module.
    Automatically initializes and cleans up.
    
    Use via controller-specific aliases: noop, gunfx, lightfx
    """
    global _module_connection
    
    conn = ScaleFXConnection(port=serial_port)
    
    # Retry connection a few times (device may still be initializing)
    for attempt in range(3):
        if conn.connect():
            break
        time.sleep(0.5)
    else:
        pytest.skip(f"Could not connect to {serial_port}")
    
    _module_connection = conn
    yield conn
    
    # Cleanup: send shutdown if still connected
    _module_connection = None
    if conn.is_connected:
        try:
            conn.send(CommandBuilder.shutdown())
        except:
            pass
        conn.close()


@pytest.fixture
def exclusive_connection(serial_port, connection) -> Generator[ScaleFXConnection, None, None]:
    """
    Function-scoped connection fixture that requires exclusive port access.
    
    Temporarily closes the module-scoped connection, creates a fresh one,
    then restores the module connection after the test.
    
    Use for tests that:
    - Test INIT handshake
    - Test SHUTDOWN behavior
    - Test REBOOT/BOOTSEL (destructive)
    """
    global _module_connection
    
    # Close module connection temporarily
    old_conn = _module_connection
    if old_conn and old_conn.is_connected:
        try:
            old_conn.close()
        except:
            pass
        time.sleep(0.3)  # Brief delay for port to be released
    
    # Create fresh connection (don't init yet - let test do that)
    conn = ScaleFXConnection(port=serial_port)
    if not conn.connect(init=False):
        pytest.skip(f"Could not open {serial_port}")
    
    yield conn
    
    # Cleanup
    if conn.is_connected:
        conn.close()
    
    # Restore module connection
    time.sleep(0.3)
    if old_conn:
        for attempt in range(3):
            if old_conn.connect():
                _module_connection = old_conn
                break
            time.sleep(0.5)


# Alias for exclusive_connection — used by system tests
fresh_connection = exclusive_connection


@pytest.fixture
def gunfx(connection) -> ScaleFXConnection:
    """GunFX connection fixture."""
    return connection


@pytest.fixture
def lightfx(connection) -> ScaleFXConnection:
    """LightFX connection fixture."""
    return connection


@pytest.fixture
def gearcontrol(connection) -> ScaleFXConnection:
    """GearControl connection fixture."""
    return connection


@pytest.fixture
def noop(connection) -> ScaleFXConnection:
    """NoOp connection fixture."""
    return connection


# Test markers
def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "hardware: requires connected hardware")
    config.addinivalue_line("markers", "gunfx: GunFX controller tests")
    config.addinivalue_line("markers", "lightfx: LightFX controller tests")
    config.addinivalue_line("markers", "gearcontrol: GearControl controller tests")
    config.addinivalue_line("markers", "noop: NoOp controller tests")
    config.addinivalue_line("markers", "slow: slow running tests")
    config.addinivalue_line("markers", "destructive: tests that reboot device")
