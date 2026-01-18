"""
Pytest Configuration and Fixtures

Provides shared fixtures for all ScaleFX controller tests.
"""

import os
import pytest
from typing import Generator

from .framework import ScaleFXConnection, GunFxCommands, LightFxCommands, CommandBuilder


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
    """
    conn = ScaleFXConnection(port=serial_port)
    if not conn.connect():
        pytest.skip(f"Could not connect to {serial_port}")
    
    yield conn
    
    # Cleanup: send shutdown if still connected
    if conn.is_connected:
        try:
            conn.send(CommandBuilder.shutdown())
        except:
            pass
        conn.close()


@pytest.fixture
def fresh_connection(serial_port) -> Generator[ScaleFXConnection, None, None]:
    """
    Function-scoped connection fixture.
    
    Creates a fresh connection for each test.
    """
    conn = ScaleFXConnection(port=serial_port)
    if not conn.connect():
        pytest.skip(f"Could not connect to {serial_port}")
    
    yield conn
    
    if conn.is_connected:
        try:
            conn.send(CommandBuilder.shutdown())
        except:
            pass
        conn.close()


@pytest.fixture
def gunfx(connection) -> ScaleFXConnection:
    """GunFX connection fixture."""
    return connection


@pytest.fixture
def lightfx(connection) -> ScaleFXConnection:
    """LightFX connection fixture."""
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
    config.addinivalue_line("markers", "noop: NoOp controller tests")
    config.addinivalue_line("markers", "slow: slow running tests")
    config.addinivalue_line("markers", "destructive: tests that reboot device")
