"""
ScaleFX CLI Terminal UI

Split-screen terminal interface using prompt_toolkit:

    ┌──────────────────────────────────────────┐
    │  Scrolling output area                   │  ← All print() output
    │  (auto-scrolls to bottom)                │     from any thread
    ├──────────────────────────────────────────┤
    │  hubfx> _                                │  ← Fixed input prompt
    └──────────────────────────────────────────┘

All print() calls are captured via sys.stdout replacement and displayed
in the output area. The input prompt is never overwritten by async output.

Architecture:
    - _OutputBuffer: Thread-safe line buffer with ANSI caching
    - _StdoutCapture: sys.stdout replacement → _OutputBuffer
    - TerminalUI: Full-screen prompt_toolkit Application with HSplit layout

Requires: prompt_toolkit >= 3.0.0
"""

import sys
import threading
from typing import Optional, Callable, List

from prompt_toolkit import Application
from prompt_toolkit.buffer import Buffer
from prompt_toolkit.data_structures import Point
from prompt_toolkit.formatted_text import ANSI
from prompt_toolkit.history import InMemoryHistory
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import Layout, HSplit, Window
from prompt_toolkit.layout.controls import BufferControl, FormattedTextControl
from prompt_toolkit.layout.processors import BeforeInput
from prompt_toolkit.styles import Style as PTStyle


# =============================================================================
# Output Buffer
# =============================================================================

class _OutputBuffer:
    """Thread-safe output buffer with ANSI color support and caching.

    Stores lines of text (with ANSI escape codes) and provides
    prompt_toolkit-compatible formatted text output. The formatted text
    is cached and only re-parsed when the buffer content changes.
    """

    MAX_LINES = 2000

    def __init__(self):
        self._lines: List[str] = []
        self._lock = threading.Lock()
        self._version = 0
        self._cached: Optional[ANSI] = None
        self._cached_ver = -1

    def append(self, line: str):
        """Add a line of text (may contain ANSI codes). Thread-safe."""
        with self._lock:
            self._lines.append(line)
            if len(self._lines) > self.MAX_LINES:
                self._lines = self._lines[-self.MAX_LINES:]
            self._version += 1

    @property
    def line_count(self) -> int:
        """Current number of lines. Thread-safe."""
        with self._lock:
            return len(self._lines)

    def get_formatted(self):
        """Return prompt_toolkit formatted text (ANSI-aware). Cached."""
        with self._lock:
            ver = self._version
            if self._cached_ver != ver:
                self._cached = ANSI('\n'.join(self._lines))
                self._cached_ver = ver
            return self._cached if self._cached else ANSI('')


# =============================================================================
# Stdout Capture
# =============================================================================

class _StdoutCapture:
    """Replacement for sys.stdout that redirects all writes to _OutputBuffer.

    Buffers incomplete lines (text without trailing newline) and flushes
    complete lines to the output buffer. This captures all print() calls
    from any thread, including parsers and command handlers.
    """

    def __init__(self, buf: _OutputBuffer, invalidate_fn: Callable):
        self._buf = buf
        self._invalidate = invalidate_fn
        self._partial = ''
        self._lock = threading.Lock()
        self.encoding = 'utf-8'
        self.errors = 'replace'

    def write(self, text: str) -> int:
        if not text:
            return 0
        with self._lock:
            self._partial += text
            while '\n' in self._partial:
                line, self._partial = self._partial.split('\n', 1)
                self._buf.append(line)
        self._invalidate()
        return len(text)

    def flush(self):
        with self._lock:
            if self._partial:
                self._buf.append(self._partial)
                self._partial = ''
        self._invalidate()

    def isatty(self) -> bool:
        return False

    @property
    def closed(self) -> bool:
        return False


# =============================================================================
# Terminal UI
# =============================================================================

class TerminalUI:
    """Full-screen split terminal for ScaleFX CLI.

    Provides a scrolling output area (top) and fixed input prompt (bottom),
    separated by a horizontal line. All print() calls from any thread are
    captured and displayed in the output area without disturbing the input.

    Usage::

        ui = TerminalUI()
        ui.on_command(my_handler)       # Set command callback
        ui.write("Welcome!")            # Pre-fill output
        ui.install_capture()            # Start capturing print()
        ui.run()                        # Block until exit
        ui.restore_stdout()             # Cleanup
    """

    def __init__(self):
        self._output = _OutputBuffer()
        self._prompt_text = 'scalefx> '
        self._command_callback: Optional[Callable] = None
        self._exit_callback: Optional[Callable] = None
        self._busy = False
        self._cancel_event = threading.Event()
        self._app: Optional[Application] = None
        self._output_window: Optional[Window] = None

        # Input buffer with command history
        self._input_buffer = Buffer(
            history=InMemoryHistory(),
            accept_handler=self._handle_enter,
            multiline=False,
            name='cli-input',
        )

        self._build_app()

    def _build_app(self):
        """Construct the prompt_toolkit Application with split layout."""

        # Top: scrolling output area with ANSI color support.
        # show_cursor=True + get_cursor_position pins a virtual cursor to the
        # last line. Window._scroll() follows it, giving us auto-scroll.
        # The cursor is invisible because this window is never focused.
        self._output_window = Window(
            content=FormattedTextControl(
                text=self._output.get_formatted,
                focusable=False,
                show_cursor=True,
                get_cursor_position=lambda: Point(
                    x=0, y=max(0, self._output.line_count - 1)),
            ),
        )

        # Middle: separator bar
        separator = Window(
            height=1,
            content=FormattedTextControl(
                text=lambda: [('class:separator', '─' * 300)],
            ),
        )

        # Bottom: single-line input with dynamic prompt
        input_window = Window(
            content=BufferControl(
                buffer=self._input_buffer,
                input_processors=[BeforeInput(self._get_prompt_tokens)],
            ),
            height=1,
            dont_extend_height=True,
        )

        # Key bindings
        kb = KeyBindings()

        @kb.add('c-c')
        def _(event):
            """Ctrl-C: cancel running command, or clear input."""
            if self._busy:
                self._cancel_event.set()
                self.write('\033[33m⚠\033[0m Cancelling...')
            else:
                self._input_buffer.reset()

        @kb.add('c-d')
        def _(event):
            """Ctrl-D on empty input: exit."""
            if not self._input_buffer.text:
                self._do_exit()

        self._app = Application(
            layout=Layout(
                HSplit([self._output_window, separator, input_window]),
                focused_element=input_window,
            ),
            key_bindings=kb,
            style=PTStyle.from_dict({'separator': '#444444'}),
            full_screen=True,
        )

    # =========================================================================
    # Rendering
    # =========================================================================

    def _get_prompt_tokens(self):
        """Return prompt as ANSI-formatted text for BeforeInput processor."""
        return ANSI(self._prompt_text)

    # =========================================================================
    # Input Handling
    # =========================================================================

    def _handle_enter(self, buffer: Buffer):
        """Called when user presses Enter in input area."""
        text = buffer.text.strip()
        if not text:
            return

        if self._busy:
            self.write('\033[33m⚠\033[0m Command in progress, please wait...')
            return

        # Echo the command to output area
        self.write(f'{self._prompt_text}{text}')

        if self._command_callback:
            self._busy = True
            t = threading.Thread(
                target=self._exec_command,
                args=(text,),
                daemon=True,
                name='cmd-runner',
            )
            t.start()

    def _exec_command(self, text: str):
        """Execute command in background thread."""
        self._cancel_event.clear()
        try:
            if self._command_callback:
                self._command_callback(text)
        except Exception as e:
            self.write(f'\033[31m✗\033[0m Error: {e}')
        finally:
            self._busy = False
            self._invalidate()

    # =========================================================================
    # Internal Helpers
    # =========================================================================

    def _invalidate(self):
        """Thread-safe UI refresh request."""
        if self._app:
            try:
                self._app.invalidate()
            except Exception:
                pass

    def _do_exit(self):
        """Trigger application exit."""
        if self._exit_callback:
            self._exit_callback()
        if self._app:
            self._app.exit()

    # =========================================================================
    # Public API
    # =========================================================================

    def write(self, text: str):
        """Write a line to the output area. Thread-safe.

        Args:
            text: Line of text (may contain ANSI color codes).
        """
        self._output.append(text)
        self._invalidate()

    def set_prompt(self, prompt: str):
        """Update the input prompt text.

        Args:
            prompt: Prompt string (may contain ANSI color codes).
        """
        self._prompt_text = prompt
        self._invalidate()

    def on_command(self, callback: Callable[[str], None]):
        """Set the command handler callback.

        The callback receives the command text (stripped) and runs
        in a background thread. All print() calls within it are
        captured to the output area.
        """
        self._command_callback = callback

    def on_exit(self, callback: Callable):
        """Set the exit cleanup callback (called on quit/Ctrl-D)."""
        self._exit_callback = callback

    def exit(self):
        """Programmatically exit the application (e.g., from 'quit' command)."""
        if self._app:
            self._app.exit()

    def install_capture(self):
        """Replace sys.stdout/stderr to capture all print() output.

        Call this before run() to capture setup output (banner, connect).
        """
        capture = _StdoutCapture(self._output, self._invalidate)
        sys.stdout = capture
        sys.stderr = capture

    def restore_stdout(self):
        """Restore original sys.stdout/stderr."""
        sys.stdout = sys.__stdout__
        sys.stderr = sys.__stderr__

    def run(self):
        """Start the full-screen UI. Blocks until exit."""
        if self._app:
            self._app.run()


# =============================================================================
# Simple Terminal (traditional mode)
# =============================================================================

class SimpleTerminal:
    """Traditional line-by-line terminal for ScaleFX CLI.

    Uses standard input()/print() — no full-screen layout, no stdout
    capture. Async output prints directly to the terminal and may
    occasionally disrupt the prompt (press Enter for a clean prompt).

    Same public API as TerminalUI so InteractiveCLI can swap freely.

    Usage::

        ui = SimpleTerminal()
        ui.on_command(my_handler)
        ui.run()                        # Block until exit
    """

    def __init__(self):
        self._prompt_text = 'scalefx> '
        self._command_callback: Optional[Callable] = None
        self._exit_callback: Optional[Callable] = None
        self._cancel_event = threading.Event()
        self._running = False

    # =========================================================================
    # Public API (matches TerminalUI interface)
    # =========================================================================

    def write(self, text: str):
        """Write a line to the terminal. Thread-safe enough for print().

        Args:
            text: Line of text (may contain ANSI color codes).
        """
        print(text)

    def set_prompt(self, prompt: str):
        """Update the input prompt text.

        Args:
            prompt: Prompt string (may contain ANSI color codes).
        """
        self._prompt_text = prompt

    def on_command(self, callback: Callable[[str], None]):
        """Set the command handler callback.

        The callback receives the command text (stripped) and runs
        synchronously in the main thread.
        """
        self._command_callback = callback

    def on_exit(self, callback: Callable):
        """Set the exit cleanup callback (called on quit/Ctrl-D)."""
        self._exit_callback = callback

    @property
    def cancel_event(self) -> threading.Event:
        """Event that is set when the user presses Ctrl+C during a command."""
        return self._cancel_event

    def exit(self):
        """Programmatically exit the run loop (e.g., from 'quit' command)."""
        self._running = False

    def install_capture(self):
        """No-op — traditional mode writes directly to stdout."""
        pass

    def restore_stdout(self):
        """No-op — traditional mode never replaces stdout."""
        pass

    def run(self):
        """Simple read-eval loop. Blocks until exit or Ctrl-C/Ctrl-D."""
        self._running = True
        while self._running:
            try:
                text = input(self._prompt_text).strip()
            except (EOFError, KeyboardInterrupt):
                print()
                if self._exit_callback:
                    self._exit_callback()
                break

            if not text:
                continue

            if self._command_callback:
                try:
                    self._command_callback(text)
                except Exception as e:
                    print(f'\033[31m✗\033[0m Error: {e}')
