"""Core protocol command builders (shared base class)."""

from ._common import build_packet, CorePacket


class CommandBuilder:
    """Base command builder with core protocol commands."""

    @staticmethod
    def init() -> bytes:
        """Send INIT command to begin communication."""
        return build_packet(CorePacket.INIT)

    @staticmethod
    def shutdown() -> bytes:
        """Send SHUTDOWN to end communication."""
        return build_packet(CorePacket.SHUTDOWN)

    @staticmethod
    def keepalive() -> bytes:
        """Send keepalive to prevent connection timeout."""
        return build_packet(CorePacket.KEEPALIVE)

    @staticmethod
    def reboot() -> bytes:
        """Reboot the controller."""
        return build_packet(CorePacket.REBOOT)

    @staticmethod
    def bootsel() -> bytes:
        """Enter BOOTSEL mode for firmware update."""
        return build_packet(CorePacket.BOOTSEL)

    @staticmethod
    def status_req() -> bytes:
        """Request STATUS report with core + module data."""
        return build_packet(CorePacket.STATUS_REQ)

    @staticmethod
    def i2c_scan() -> bytes:
        """Scan I2C bus (returns scan results as response)."""
        return build_packet(CorePacket.I2C_SCAN)

    @staticmethod
    def identify() -> bytes:
        """
        Identify the connected board without triggering init.

        Returns the same payload as INIT_READY but without side effects.
        """
        return build_packet(CorePacket.IDENTIFY)
