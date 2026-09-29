#!/usr/bin/env python3
"""uc_log_client: a uc_log_printer seen from a script.

The control socket (src/uc_log/detail/ControlProtocol.hpp). Knows no build system or firmware:
that is Kvasir_SDK's tools/kvasir_bench.py.
"""
import json
import os
import socket
from pathlib import Path

# ---- the printer's control socket -------------------------------------------------------------

PROTOCOL = 1   # ControlProtocol.hpp ProtocolVersion


class ControlError(Exception):
    """The printer answered {"cmd":"error"}."""


def control_socket_path(log_dir: Path) -> Path:
    """TcpServerCommon.hpp unixSocketPath: control.sock in the log directory, or
    /tmp/uc_log_<uid>/<FNV-1a of the directory>.control.sock when that exceeds sun_path's 108 bytes."""
    log_dir = Path(os.path.abspath(log_dir))
    direct = log_dir / "control.sock"
    if len(os.fsencode(direct)) < 108:
        return direct
    h = 0xcbf29ce484222325
    for c in os.fsencode(log_dir):
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return Path(f"/tmp/uc_log_{os.getuid()}") / f"{h:016x}.control.sock"


def connect(control: Path, timeout: float) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(str(control))
    except OSError:
        sock.close()
        raise
    return sock


def encode(req: dict) -> str:
    return json.dumps(req, separators=(",", ":"), ensure_ascii=False) + "\n"


def check_answer(answer: dict) -> dict:
    """The answer, or ControlError with the printer's reason."""
    if answer.get("cmd") == "error":
        raise ControlError(answer.get("error", "?"))
    return answer


class Connection:
    """One connection to a printer's control socket: request after request, one answer each."""

    def __init__(self, control: Path, timeout: float = 5):
        self.sock = connect(control, timeout)
        # not sock.makefile(): a file object refuses every read after one timeout
        self.pending = b""

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self) -> None:
        self.sock.close()

    def send(self, req: dict) -> None:
        self.sock.sendall(encode(req).encode())

    def next(self) -> dict:
        """The next line, decoded; socket.timeout leaves the connection usable."""
        while b"\n" not in self.pending:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ControlError("the printer closed the connection")
            self.pending += chunk
        raw, self.pending = self.pending.split(b"\n", 1)
        line = raw.decode("utf-8", errors="replace")
        try:
            return json.loads(line)
        except ValueError:
            raise ControlError(f"not a line of protocol {PROTOCOL}: {line.strip()[:80]!r} - an "
                               "older uc_log_printer? rebuild it in this tree "
                               "(cmake --build <build> --target uc_log_printer) and restart it")

    def ask(self, req: dict, timeout: float | None = None) -> dict:
        if timeout is not None:
            self.sock.settimeout(timeout)
        self.send(req)
        return check_answer(self.next())


def request(control: Path, req: dict, timeout: float = 5) -> dict:
    """One request on a connection of its own."""
    with Connection(control, timeout) as c:
        return c.ask(req)


# ---- requests

def req_ping() -> dict:
    return {"cmd": "ping"}


def req_status() -> dict:
    return {"cmd": "status"}


def req_messages(count: int | None = None) -> dict:
    return {"cmd": "messages"} | ({"count": count} if count is not None else {})


def req_read(pieces: list[tuple[int, int]]) -> dict:
    return {"cmd": "read", "pieces": [{"address": a, "size": n} for a, n in pieces]}


def req_write(words: list[tuple[int, int]]) -> dict:
    """(address, value) words written in order, each read back; read_bytes() decodes the answer."""
    return {"cmd": "write", "words": [{"address": a, "value": v} for a, v in words]}


def req_wait(address: int, size: int, condition: str, timeout_ms: int,
             value: int | None = None, mask: int | None = None) -> dict:
    req = {"cmd": "wait", "piece": {"address": address,
                                    "size": size}, "condition": condition}
    if value is not None:
        req["value"] = value
    if mask is not None:
        req["mask"] = mask
    return req | {"timeout_ms": timeout_ms}


def req_reset() -> dict:
    return {"cmd": "reset"}


def req_flash() -> dict:
    return {"cmd": "flash"}


def req_subscribe(streams: list[str], min_level: str | None = None,
                  modules: list[str] | None = None, not_modules: list[str] | None = None,
                  since_seq: int | None = None, last: int | None = None,
                  follow: bool = True) -> dict:
    """since_seq / last: the printer's log history first, ended by a backlog_end event."""
    req = {"cmd": "subscribe", "streams": list(streams)}
    if min_level:
        req["min_level"] = min_level
    if modules:
        req["modules"] = list(modules)
    if not_modules:
        req["not_modules"] = list(not_modules)
    if since_seq is not None:
        req["since_seq"] = since_seq
    if last is not None:
        req["last"] = last
    if not follow:
        req["follow"] = False
    return req


def history(control: Path, timeout: float = 30, **filters) -> tuple[list[dict], dict]:
    """The printer's log history (req_subscribe's filters, since_seq or last): the log events
    and the backlog_end event."""
    with Connection(control, timeout) as c:
        c.ask(req_subscribe(["log"], follow=False, **filters))
        lines = []
        while True:
            event = c.next()
            if event.get("cmd") == "backlog_end":
                return lines, event
            lines.append(event)


# ---- answers and events as text

def read_bytes(answer: dict) -> tuple[int, list[bytes]]:
    """(unix µs on the log's recv_time clock, the bytes of each piece)."""
    return answer["unix_us"], [bytes.fromhex(d) for d in answer["data"]]


def message_line(m: dict) -> str:
    """A Status tab line as <stamp>.status.log holds it."""
    return f"{m['time']} [{m['level']}] {m['text']}"


def firmware_text(firmware: dict) -> str:
    build = firmware.get("build")
    return f"firmware {firmware['state']}" + (f" build {build:08x}" if build is not None else "")


def status_text(answer: dict) -> str:
    return (f"{'HALTED' if answer['halted'] else 'running' if answer['running'] else 'not connected'}"
            f"{', flashing' if answer['flashing'] else ''}, {firmware_text(answer['firmware'])}, "
            f"{answer['sessions']} session(s), {answer['errors']} status error(s)")


def log_line_text(e: dict) -> str:
    """A log event as `log` shows it."""
    return format_fields(e["uc_time_ns"] / 1e6, e["level"], e.get("module", ""), e["file"], e["line"],
                         e["message"])


def metric_key(sample: dict) -> str:
    return (sample["scope"] + "::" if sample["scope"] else "") + sample["name"]


# ---- log lines as text -----------------------------------------------------------------------

LEVELS = ["trace", "debug", "info", "warn", "error", "crit"]


def format_fields(ms: float, level: str, module: str, file: str, line, message: str) -> str:
    tag = f"[{module}] " if module else ""
    return f"{ms:12.3f} ms {level:5} {tag}{file}:{line} {message}"
