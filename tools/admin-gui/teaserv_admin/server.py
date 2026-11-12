"""Runs the Teaserv server as a child process and talks to its shell.

The server is started with --force-shell, so its normal shell reads commands from a pipe. Stopping is the shell's own
`exit` command: the server ends its event loop, saves and closes by itself. This module never kills the process unless
the admin explicitly asks for a forced stop.
"""

import collections
import datetime
import json
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time

from .appsettings import IS_WINDOWS

STOPPED = "stopped"
STARTING = "starting"
RUNNING = "running"
STOPPING = "stopping"
FAILED = "failed"

STATE_TEXT = {
    STOPPED: "Server is stopped",
    STARTING: "Server is starting",
    RUNNING: "Server is running",
    STOPPING: "Server is stopping",
    FAILED: "Server stopped with an error",
}

PROMPT = "newserv> "
STATS_PREFIX = "TEASERV-STATS "
LOG_LINE = re.compile(r"^([DIWE]) (\d+) (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) - (.*)$")
LEVEL_ORDER = {"D": 0, "I": 1, "W": 2, "E": 3}
MAX_LINES = 20000


class Line:
    __slots__ = ("seq", "level", "text", "kind")

    def __init__(self, seq, level, text, kind):
        self.seq = seq
        self.level = level  # D, I, W, E
        self.text = text
        self.kind = kind  # log (server log), out (command reply), admin (this program), cmd (command sent)


def line_level(text, previous):
    """Level of a line. The shell reports a failed command as "FAILED: ..." without a level letter; other lines
    without a level (continuation lines, data dumps) take the level of the line above."""
    parsed = split_log_line(text)
    if parsed:
        return parsed[0]
    if text.startswith("FAILED:"):
        return "E"
    return previous


def split_log_line(text):
    """(level, time text, message) of a server log line, or None for other lines."""
    match = LOG_LINE.match(text)
    if not match:
        return None
    return match.group(1), match.group(3), match.group(4)


class ServerProcess:
    def __init__(self, settings):
        self.settings = settings
        self.state = STOPPED
        self.proc = None
        self.pid = None
        self.queue = queue.Queue()
        self.lines = collections.deque(maxlen=MAX_LINES)
        self.seq = 0
        self.line_listeners = []  # called with a list of new Line objects
        self.state_listeners = []  # called with (state, detail text)
        self.stats_listeners = []  # called with the latest stats dict
        self.shell_ready = False
        self.shell_missing_reported = False
        self.ready_at = None
        self.started_at = None
        self.stop_requested_at = None
        self.stop_timeout_reported = False
        self.restart_pending = False
        self.last_exit_code = None
        self.last_error_lines = collections.deque(maxlen=5)
        self.stats = None
        self.stats_at = 0.0
        self.stats_supported = None
        self.stats_waiting_since = None
        self.next_stats_poll = 0.0
        self.readers = []
        self.stdin_lock = threading.Lock()
        self.previous_level = "I"
        self.log_file = None
        self.log_file_date = None

    # ----- lines and the log file

    def _stamp(self):
        return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    def _add(self, text, kind, level=None):
        if kind == "log":
            level = line_level(text, self.previous_level)
            if split_log_line(text):
                self.previous_level = level
        elif level is None:
            level = "I"
        self.seq += 1
        line = Line(self.seq, level, text, kind)
        self.lines.append(line)
        if level in ("W", "E") and kind == "log":
            self.last_error_lines.append(text)
        return line

    def admin_line(self, message, level="I"):
        """A line written by this program, in the server's own log format."""
        text = "%s %d %s - [Admin] %s" % (level, os.getpid(), self._stamp(), message)
        line = self._add(text, "admin", level)
        self._write_log([line])
        for listener in list(self.line_listeners):
            listener([line])
        return line

    def _write_log(self, lines):
        if not self.settings.server_dir:
            return
        today = datetime.date.today().isoformat()
        try:
            if self.log_file is None or self.log_file_date != today:
                if self.log_file:
                    self.log_file.close()
                os.makedirs(self.settings.log_dir, exist_ok=True)
                path = os.path.join(self.settings.log_dir, "teaserv-%s.log" % today)
                self.log_file = open(path, "a", encoding="utf-8", newline="\n")
                self.log_file_date = today
            for line in lines:
                self.log_file.write(line.text + "\n")
            self.log_file.flush()
        except OSError:
            self.log_file = None  # try again at the next line

    def remove_old_logs(self):
        keep_days = self.settings["log_keep_days"]
        if keep_days <= 0 or not os.path.isdir(self.settings.log_dir):
            return
        limit = datetime.date.today() - datetime.timedelta(days=keep_days)
        for name in os.listdir(self.settings.log_dir):
            match = re.match(r"^teaserv-(\d{4})-(\d\d)-(\d\d)\.log$", name)
            if not match:
                continue
            try:
                if datetime.date(*map(int, match.groups())) < limit:
                    os.remove(os.path.join(self.settings.log_dir, name))
            except (ValueError, OSError):
                pass

    # ----- state

    def _set_state(self, state, detail=""):
        self.state = state
        for listener in list(self.state_listeners):
            listener(state, detail)

    def is_active(self):
        return self.state in (STARTING, RUNNING, STOPPING)

    # ----- start

    def start(self):
        """Start the server. Returns "" on success or the reason it could not be started."""
        if self.is_active():
            return "The server is already running."
        problems = self.settings.problems()
        if problems:
            return "\n".join(problems)
        busy = ports_in_use(self.settings.config_path)
        if busy:
            message = ("Ports already in use: %s%s. Another Teaserv (for example one started from a .bat file) or "
                       "another program is using them. Stop that program first." % (
                           ", ".join(str(p) for p in busy[:5]), " and more" if len(busy) > 5 else ""))
            self.admin_line("Not started: " + message, "E")
            return message

        env = dict(os.environ)
        extra = self.settings["extra_path"].strip()
        if extra:
            env["PATH"] = extra + os.pathsep + env.get("PATH", "")
        kwargs = {}
        if IS_WINDOWS:
            kwargs["creationflags"] = 0x08000000  # CREATE_NO_WINDOW: no console window appears
        try:
            self.proc = subprocess.Popen(
                [self.settings.server_exe, "--force-shell"],
                cwd=self.settings.server_dir, env=env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)
        except OSError as e:
            self.admin_line("Cannot start the server: %s" % e, "E")
            self._set_state(FAILED, str(e))
            return str(e)

        self.pid = self.proc.pid
        self.shell_ready = False
        self.shell_missing_reported = False
        self.ready_at = None
        self.started_at = time.time()
        self.stop_requested_at = None
        self.stop_timeout_reported = False
        self.last_exit_code = None
        self.last_error_lines.clear()
        self.stats = None
        self.stats_supported = None
        self.stats_waiting_since = None
        self.readers = [
            threading.Thread(target=self._read_stderr, args=(self.proc,), daemon=True),
            threading.Thread(target=self._read_stdout, args=(self.proc,), daemon=True),
        ]
        for reader in self.readers:
            reader.start()
        self.admin_line("Server started (process %d): %s" % (self.pid, self.settings.server_exe))
        self._set_state(STARTING)
        return ""

    def _read_stderr(self, proc):
        for raw in iter(proc.stderr.readline, b""):
            self.queue.put(("log", raw.decode("utf-8", "replace").rstrip("\r\n")))

    def _read_stdout(self, proc):
        # The shell prints its prompt without a line end, so this reads pieces, not lines
        pending = ""
        while True:
            try:
                chunk = os.read(proc.stdout.fileno(), 65536)
            except OSError:
                break
            if not chunk:
                break
            pending += chunk.decode("utf-8", "replace")
            while True:
                while pending.startswith(PROMPT):
                    pending = pending[len(PROMPT):]
                    self.queue.put(("prompt", ""))
                newline = pending.find("\n")
                if newline == -1:
                    break
                self.queue.put(("out", pending[:newline].rstrip("\r")))
                pending = pending[newline + 1:]
        if pending.strip():
            self.queue.put(("out", pending))

    # ----- commands

    def _write(self, text):
        proc = self.proc
        if not proc or proc.poll() is not None:
            return False
        try:
            with self.stdin_lock:
                proc.stdin.write((text + "\n").encode("utf-8"))
                proc.stdin.flush()
            return True
        except (OSError, ValueError):
            return False

    def send_command(self, text):
        """Send a shell command typed by the admin. Returns "" or the reason it was not sent."""
        text = text.strip()
        if not text:
            return ""
        if self.state != RUNNING:
            return "The server is not running."
        if not self.shell_ready:
            return "This server build does not accept commands from the admin program."
        if text in ("exit", "quit"):
            self.stop()
            return ""
        line = self._add("%s %d %s - [Admin] > %s" % ("I", os.getpid(), self._stamp(), text), "cmd", "I")
        self._write_log([line])
        for listener in list(self.line_listeners):
            listener([line])
        if not self._write(text):
            return "The command could not be sent."
        return ""

    def request_stats(self):
        if self.state == RUNNING and self.shell_ready and self.stats_supported is not False:
            if self._write("stats"):
                self.stats_waiting_since = time.time()

    # ----- stop

    def stop(self, restart=False):
        """Ask the server to shut down by itself. Returns "" or the reason nothing was done."""
        if self.state not in (STARTING, RUNNING):
            return "The server is not running."
        self.restart_pending = restart
        self.stop_requested_at = time.time()
        self.stop_timeout_reported = False
        self.admin_line("Restart requested" if restart else "Stop requested")
        # `exit` is the server's own shutdown command. While the server is still loading, the command waits in the
        # pipe and is read as soon as the shell starts.
        sent = self._write("exit")
        self._set_state(STOPPING)
        if not sent:
            self.admin_line("The stop command could not be sent to the server", "W")
        return ""

    def restart(self):
        if self.state in (STOPPED, FAILED):
            return self.start()
        return self.stop(restart=True)

    def force_stop(self):
        """Close the process without letting it finish. Only on the admin's explicit request."""
        if self.proc and self.proc.poll() is None:
            self.admin_line("Forced stop requested by the admin", "W")
            try:
                self.proc.kill()
            except OSError as e:
                self.admin_line("Forced stop failed: %s" % e, "E")

    def stop_and_wait(self, timeout):
        """For program or session end: ask for a clean stop and wait. May be called from another thread."""
        proc = self.proc
        if not proc or proc.poll() is not None:
            return True
        self._write("exit")
        try:
            proc.wait(timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    # ----- periodic work (Tk thread)

    def poll(self):
        new_lines = []
        handled = 0
        while handled < 3000:
            try:
                kind, text = self.queue.get_nowait()
            except queue.Empty:
                break
            handled += 1
            if kind == "prompt":
                if not self.shell_ready:
                    self.shell_ready = True
                    self.next_stats_poll = 0.0
                continue
            if kind == "out":
                if text.startswith(STATS_PREFIX):
                    self._take_stats(text[len(STATS_PREFIX):])
                    continue
                new_lines.append(self._add(text, "out", "I"))
                continue
            # Server log line
            if self.stats_waiting_since and text.startswith("FAILED:") and time.time() - self.stats_waiting_since < 3:
                # Reply to this program's own `stats` request from a server build without that command
                self.stats_waiting_since = None
                self.stats_supported = False
                new_lines.append(self._add(
                    "W %d %s - [Admin] This server build has no `stats` command; live numbers are not available"
                    % (os.getpid(), self._stamp()), "admin", "W"))
                continue
            line = self._add(text, "log")
            new_lines.append(line)
            if self.state == STARTING and text.endswith(" - Ready"):
                self.ready_at = time.time()
                self._set_state(RUNNING)

        if new_lines:
            self._write_log(new_lines)
            for listener in list(self.line_listeners):
                listener(new_lines)

        now = time.time()
        proc = self.proc
        if proc is not None and proc.poll() is not None and self.queue.empty() and \
                not any(reader.is_alive() for reader in self.readers):
            self._process_ended(proc.returncode)
            return

        if self.state == RUNNING:
            if not self.shell_ready and not self.shell_missing_reported and self.ready_at and \
                    now - self.ready_at > 5:
                self.shell_missing_reported = True
                self.admin_line(
                    "This server build does not read commands from the admin program (it needs a Teaserv build with "
                    "--force-shell). Commands, live numbers and a clean stop are not available.", "W")
            if self.shell_ready and now >= self.next_stats_poll:
                self.next_stats_poll = now + max(1, self.settings["stats_poll_seconds"])
                self.request_stats()
        elif self.state == STOPPING and not self.stop_timeout_reported and \
                now - self.stop_requested_at > self.settings["stop_timeout_seconds"]:
            self.stop_timeout_reported = True
            self.admin_line("The server has not stopped after %d seconds" % self.settings["stop_timeout_seconds"],
                            "W")
            self._set_state(STOPPING, "timeout")

    def _take_stats(self, text):
        try:
            stats = json.loads(text)
        except ValueError:
            return
        self.stats_waiting_since = None
        self.stats_supported = True
        self.stats = stats
        self.stats_at = time.time()
        for listener in list(self.stats_listeners):
            listener(stats)

    def _process_ended(self, code):
        was = self.state
        self.proc = None
        self.pid = None
        self.shell_ready = False
        self.stats = None
        self.last_exit_code = code
        for listener in list(self.stats_listeners):
            listener(None)
        if was == STOPPING and code == 0:
            self.admin_line("Server stopped")
            restart = self.restart_pending
            self.restart_pending = False
            self._set_state(STOPPED)
            if restart:
                self.start()
            return
        self.restart_pending = False
        if was == STOPPING:
            self.admin_line("Server stopped with exit code %s" % describe_exit_code(code), "W")
            self._set_state(STOPPED)
            return
        detail = "The server ended by itself with exit code %s." % describe_exit_code(code)
        if self.last_error_lines:
            parsed = split_log_line(self.last_error_lines[-1])
            detail += "\nLast message: " + (parsed[2] if parsed else self.last_error_lines[-1])
        self.admin_line(detail.replace("\n", " "), "E")
        self._set_state(FAILED, detail)

    def uptime_seconds(self):
        if self.is_active() and self.started_at:
            return int(time.time() - self.started_at)
        return 0

    def close(self):
        if self.log_file:
            try:
                self.log_file.close()
            except OSError:
                pass
            self.log_file = None


def configured_ports(config_path):
    """TCP ports from PortConfiguration in config.json; empty when the file cannot be read."""
    from . import jsonc
    try:
        with open(config_path, encoding="utf-8") as f:
            config = jsonc.parse(f.read()).to_python()
        entries = config.get("PortConfiguration", {})
        ports = []
        for entry in entries.values():
            spec = entry[0]
            port = spec[1] if isinstance(spec, list) else spec
            if isinstance(port, int) and 0 < port < 65536 and port not in ports:
                ports.append(port)
        return ports
    except (OSError, ValueError, AttributeError, IndexError, TypeError):
        return []


def ports_in_use(config_path):
    """Game ports of the configuration that some program already listens on.

    The server would not notice by itself on Windows: there a second program may open a port that is already open,
    and players would reach one of the two servers at random.
    """
    busy = []
    for port in configured_ports(config_path):
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            if IS_WINDOWS:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            probe.bind(("0.0.0.0", port))
        except OSError:
            busy.append(port)
        finally:
            probe.close()
    return busy


def describe_exit_code(code):
    if code is None:
        return "unknown"
    if IS_WINDOWS:
        code &= 0xFFFFFFFF
        if code in (0xC0000135, 0xC0000139):
            return ("0x%08X (a DLL the server needs was not found, or a wrong version of it was found first; set "
                    "\"Folders with DLLs\" in Settings > Admin program)" % code)
        if code >= 0x80000000:
            return "0x%08X" % code
    return str(code)


def format_duration(seconds):
    seconds = int(seconds)
    days, rest = divmod(seconds, 86400)
    hours, rest = divmod(rest, 3600)
    minutes, secs = divmod(rest, 60)
    if days:
        return "%d d %d h" % (days, hours)
    if hours:
        return "%d h %d min" % (hours, minutes)
    if minutes:
        return "%d min %d s" % (minutes, secs)
    return "%d s" % secs
