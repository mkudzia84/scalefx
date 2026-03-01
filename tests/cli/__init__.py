"""
ScaleFX Interactive CLI Package

Refactored CLI with modular architecture:
- base.py: CommandInfo, OutputMixin, ControllerType
- parsers.py: Response packet parsing utilities
- handlers/: Command handler modules
  - core.py: Connection and protocol commands
  - gunfx.py: GunFX-specific commands
  - lightfx.py: LightFX-specific commands
- interactive.py: Main CLI class (original, maintained for compatibility)
- interactive_new.py: Refactored CLI using modular handlers
"""

from .base import CommandInfo, ControllerType, OutputMixin, CommandHandlerBase
from .handlers import CoreCommandHandler, GunFxCommandHandler, LightFxCommandHandler

__all__ = [
    'CommandInfo',
    'ControllerType',
    'OutputMixin',
    'CommandHandlerBase',
    'CoreCommandHandler',
    'GunFxCommandHandler',
    'LightFxCommandHandler',
]
