#!/usr/bin/env python3
"""uc_log_client.py against the control protocol's golden files (doc/control_protocol/), and,
with UC_LOG_CONTROL_PROTOCOL_TOOL set (ctest does), against a real server with a fake target.

    python3 tools/test_uc_log_client.py -v
"""
import importlib.util
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE.parent / "doc" / "control_protocol"

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools

spec = importlib.util.spec_from_file_location(
    "uc_log_client", HERE / "uc_log_client.py")
assert spec is not None and spec.loader is not None
kb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kb)


def jsonl(name: str) -> list[dict]:
    return [json.loads(line) for line in (FIXTURES / name).read_text(encoding="utf-8").splitlines()]


TRANSCRIPT = {x["name"]: x for x in jsonl("transcript.jsonl")}
STREAMS = {x["name"]: x for x in jsonl("stream.jsonl")}
SCHEMA = json.loads((FIXTURES / "schema.json").read_text(encoding="utf-8"))
# the fixture target of tests/data/records through `records` and `raise_panic` (the SDK's kvasir_bench texts read it)
RECORDS = {x["name"]: x for x in jsonl("records_fixture.jsonl")}

# The transcript's malformed requests are missing: no builder makes them.
BUILT = {
    "ping": kb.req_ping(),
    "status": kb.req_status(),
    "messages": kb.req_messages(),
    "messages_2": kb.req_messages(2),
    "read_one": kb.req_read([(0x20000010, 4)]),
    "read_two": kb.req_read([(0x200000FE, 3), (0x10, 1)]),
    "read_nothing": kb.req_read([]),
    "read_zero": kb.req_read([(0, 0)]),
    "read_too_much": kb.req_read([(0, 5000)]),
    "read_disconnected": kb.req_read([(0xE0000000, 4)]),
    "wait_changed": kb.req_wait(0x30000000, 4, "changed", 1000),
    "wait_eq": kb.req_wait(0x30000000, 4, "eq", 1000, value=20),
    "wait_mask": kb.req_wait(0x30000000, 4, "eq", 1000, value=0, mask=3),
    "wait_timeout": kb.req_wait(0x20000000, 4, "ne", 30, value=0x03020100),
    "wait_too_wide": kb.req_wait(0x20000000, 9, "changed", 10),
    "wait_too_long": kb.req_wait(0x20000000, 4, "changed", 600001),
    "reset": kb.req_reset(),
    "reset_line": kb.req_reset(line=True),
    "halt": kb.req_halt(),
    "go": kb.req_go(),
    "registers": kb.req_registers(),
    "write_registers": kb.req_write_registers([(0, 7), (15, 0x10002000)]),
    "write_registers_bad": kb.req_write_registers([(19, 0)]),
    "flash": kb.req_flash(),
    "write_two": kb.req_write([(0x5000000C, 0), (0x50000010, 0x12345678)]),
    "write_nothing": kb.req_write([]),
    "write_unaligned": kb.req_write([(0x5000000E, 0)]),
    "write_disconnected": kb.req_write([(0xE0000000, 1)]),
    "records_no_map": kb.req_records(["crash", "trace"]),
    "raise_panic_no_map": kb.req_raise_panic("user"),
}

RECORDS_BUILT = {
    "records_lld": kb.req_records(kb.RECORD_KINDS),
    "records_gnu": kb.req_records(kb.RECORD_KINDS),
    "records_crash": kb.req_records(["crash"]),
    "records_crash_health_empty": kb.req_records(["crash", "health"]),
    "records_crash_health_no_symbols": kb.req_records(["crash", "health"]),
    "records_stacks_not_painted": kb.req_records(["stacks"]),
    "records_stacks_no_symbols": kb.req_records(["stacks"]),
    "records_ubsan_none": kb.req_records(["ubsan"]),
    "records_ubsan_not_sanitize": kb.req_records(["ubsan"]),
    "records_trace_written_while_reading": kb.req_records(["trace"]),
    "records_trace_written_while_reading_drops": kb.req_records(["trace"]),
    "records_trace_no_rings": kb.req_records(["trace"]),
    "records_trace_a_storage_without_its_descriptor": kb.req_records(["trace"]),
    "records_no_kinds": kb.req_records([]),
    "crash_golden_1": kb.req_records(["crash"]),
    "crash_golden_2": kb.req_records(["crash"]),
    "crash_golden_3": kb.req_records(["crash"]),
    "crash_golden_4": kb.req_records(["crash"]),
    "health_a_starvation": kb.req_records(["health"]),
    "health_a_reset_inside_on_starved": kb.req_records(["health"]),
    "health_an_index_without_a_hash": kb.req_records(["health"]),
    "health_an_external_reset": kb.req_records(["health"]),
    "health_a_panic_in_the_guard": kb.req_records(["health"]),
    "raise_panic_check_failed": kb.req_raise_panic("check-failed"),
    "raise_panic_user": kb.req_raise_panic("user"),
    "raise_panic_record_name": kb.req_raise_panic("stack overflow"),
    "raise_panic_unknown_cause": kb.req_raise_panic("nope"),
    "raise_panic_no_raise": kb.req_raise_panic("user"),
}

SUBSCRIBED = {
    "log": kb.req_subscribe(["log"]),
    "log_warn_i2c": kb.req_subscribe(["log"], min_level="warn", modules=["i2c"]),
    "log_metrics_no_usb": kb.req_subscribe(["log", "metrics"], not_modules=["usb"]),
    "messages_metrics": kb.req_subscribe(["messages", "metrics"]),
    "history_last_2_warn": kb.req_subscribe(["log"], min_level="warn", last=2, follow=False),
    "history_since_3": kb.req_subscribe(["log"], since_seq=3, follow=False),
    "history_without_log": kb.req_subscribe(["messages"], since_seq=0),
}


def without_seq(events: list[dict]) -> list[dict]:
    return [{k: v for k, v in e.items() if k not in ("seq", "next_seq")} for e in events]


# ---- a JSON schema check of the subset glaze writes

def schema_errors(value, schema: dict, root: dict, path: str = "$") -> list[str]:
    if "$ref" in schema:
        name = schema["$ref"].removeprefix(
            "#/$defs/").replace("~1", "/").replace("~0", "~")
        return schema_errors(value, root["$defs"][name], root, path)
    errors = []
    if "const" in schema and value != schema["const"]:
        errors.append(f"{path}: {value!r} is not {schema['const']!r}")
    for key in ("oneOf", "anyOf"):
        if key in schema:
            fits = [not schema_errors(value, s, root, path)
                    for s in schema[key]]
            if (key == "oneOf" and fits.count(True) != 1) or (key == "anyOf" and not any(fits)):
                errors.append(
                    f"{path}: {key} matched {fits.count(True)} of {len(fits)}")
    kind = schema.get("type")
    kinds = kind if isinstance(kind, list) else [kind] if kind else []
    checks = {
        "object": lambda v: isinstance(v, dict),
        "array": lambda v: isinstance(v, list),
        "string": lambda v: isinstance(v, str),
        "boolean": lambda v: isinstance(v, bool),
        "null": lambda v: v is None,
        "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
        "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    }
    if kinds and not any(checks[k](value) for k in kinds):
        return errors + [f"{path}: {value!r} is not {kind}"]
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: {value} < {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: {value} > {schema['maximum']}")
    if isinstance(value, dict):
        props = schema.get("properties", {})
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path}: {key} missing")
        for key, v in value.items():
            if key in props:
                errors += schema_errors(v, props[key], root, f"{path}.{key}")
            elif schema.get("additionalProperties") is False:
                errors.append(f"{path}: unexpected {key}")
    if isinstance(value, list) and "items" in schema:
        for i, v in enumerate(value):
            errors += schema_errors(v, schema["items"], root, f"{path}[{i}]")
    return errors


class Fixtures(unittest.TestCase):
    def test_requests_are_the_transcripts(self):
        for name, built in BUILT.items():
            with self.subTest(name):
                self.assertEqual(json.loads(
                    TRANSCRIPT[name]["request"]), built)
                line = kb.encode(built)
                self.assertTrue(line.endswith("\n") and "\n" not in line[:-1])
                self.assertEqual(json.loads(line), built)

    def test_records_requests_are_the_fixture_s(self):
        self.assertEqual(set(RECORDS_BUILT), set(RECORDS),
                         "every records_fixture.jsonl case has its builder")
        for name, built in RECORDS_BUILT.items():
            with self.subTest(name):
                self.assertEqual(json.loads(RECORDS[name]["request"]), built)
                self.assertEqual(schema_errors(
                    built, SCHEMA["request"], SCHEMA["request"]), [])
                self.assertEqual(schema_errors(
                    RECORDS[name]["answer"], SCHEMA["answer"], SCHEMA["answer"]), [])

    def test_records_answers_need_no_layout(self):
        # what kvasir_bench reads: decoded keys, no offsets or shifts
        answer = kb.check_answer(RECORDS["records_lld"]["answer"])
        fault = answer["fault"]
        self.assertEqual((fault["core"], fault["arch"], fault["cfsr_names"]), (0, 8, [
                         "PRECISERR", "BFARVALID"]))
        self.assertEqual({w["name"]: w["value"]
                         for w in fault["words"]}["pc"], fault["record"]["pc"])
        self.assertEqual(answer["panic"]["cause_name"], "fault")
        self.assertEqual(answer["panic"]["site"],
                         (answer["panic"]["record"]["pc"] & ~1) - 2)
        self.assertEqual(answer["exception_names"][3], "HardFault")
        self.assertNotIn(
            "fault", RECORDS["records_crash_health_no_symbols"]["answer"])
        self.assertFalse(
            RECORDS["records_crash_health_empty"]["answer"]["fault"]["valid"])
        with self.assertRaises(kb.ControlError):
            kb.check_answer(RECORDS["raise_panic_unknown_cause"]["answer"])

    def test_subscriptions_are_the_streams(self):
        for name, built in SUBSCRIBED.items():
            with self.subTest(name):
                self.assertEqual(json.loads(STREAMS[name]["request"]), built)

    def test_every_golden_line_fits_the_schema(self):
        for name, x in TRANSCRIPT.items():
            with self.subTest(name):
                self.assertEqual(schema_errors(
                    x["answer"], SCHEMA["answer"], SCHEMA["answer"]), [])
                if name in BUILT:
                    self.assertEqual(
                        schema_errors(json.loads(x["request"]), SCHEMA["request"], SCHEMA["request"]), [])
        for name, x in STREAMS.items():
            with self.subTest(name):
                self.assertEqual(
                    schema_errors(json.loads(x["request"]), SCHEMA["request"], SCHEMA["request"]), [])
                self.assertEqual(schema_errors(
                    x["answer"], SCHEMA["answer"], SCHEMA["answer"]), [])
                for e in x["events"]:
                    self.assertEqual(schema_errors(
                        e, SCHEMA["event"], SCHEMA["event"]), [])

    def test_the_schema_check_can_fail(self):
        self.assertNotEqual(
            schema_errors({"cmd": "read", "unix_us": "soon", "data": []}, SCHEMA["answer"], SCHEMA["answer"]), [])
        self.assertNotEqual(
            schema_errors({"cmd": "status", "running": True}, SCHEMA["answer"], SCHEMA["answer"]) +
            schema_errors({"cmd": "ping", "verbose": True}, SCHEMA["request"], SCHEMA["request"]), [])

    def test_answers_decode(self):
        stamp, data = kb.read_bytes(TRANSCRIPT["read_two"]["answer"])
        self.assertEqual(stamp, 1758448800000000)
        self.assertEqual(data, [bytes([0xFE, 0xFF, 0x00]), bytes([0x10])])
        status = TRANSCRIPT["status"]["answer"]
        self.assertEqual(kb.firmware_text(
            status["firmware"]), "firmware match build 1234abcd")
        self.assertTrue(kb.status_text(status).startswith(
            "running, firmware match build 1234abcd"))
        self.assertEqual([kb.message_line(m) for m in TRANSCRIPT["messages_2"]["answer"]["messages"]],
                         ["2025-09-21T10:00:01.000Z [error] RTT buffer 0 overflow",
                          "2025-09-21T10:00:02.000Z [tool] control socket up"])
        _, words = kb.read_bytes(TRANSCRIPT["write_two"]["answer"])
        self.assertEqual([int.from_bytes(w, "little")
                         for w in words], [0, 0x12345678])
        wait = TRANSCRIPT["wait_timeout"]["answer"]
        self.assertEqual((wait["hit"], wait["first"],
                         wait["last"]), (False, 0x03020100, 0x03020100))

    def test_errors_raise(self):
        for name, x in TRANSCRIPT.items():
            with self.subTest(name):
                if x["answer"]["cmd"] == "error":
                    with self.assertRaises(kb.ControlError):
                        kb.check_answer(x["answer"])
                else:
                    self.assertIs(kb.check_answer(x["answer"]), x["answer"])

    def test_events_decode(self):
        events = STREAMS["log_metrics_no_usb"]["events"]
        logs = [kb.log_line_text(e) for e in events if e["cmd"] == "log"]
        self.assertEqual(
            logs[1], "    1600.000 ms warn  [i2c.bus] Bus.hpp:42 nack from 0x40")
        self.assertEqual(logs[0], "    1500.000 ms info  main.cpp:10 boot")
        metrics = [(kb.metric_key(e), e["unit"], e["value"])
                   for e in events if e["cmd"] == "metric"]
        self.assertEqual(metrics, [("valve::temperature", "m℃", 58300), ("regulator::flow", "", 0),
                                   ("main.cpp:20::rate", "Hz", 1.5)])
        warn_i2c = [e["module"] for e in STREAMS["log_warn_i2c"]["events"]]
        self.assertEqual(warn_i2c, ["i2c.bus", "i2c"])
        kinds = [kb.message_kind(
            e) for e in STREAMS["messages_metrics"]["events"] if e["cmd"] == "message"]
        self.assertEqual(kinds, ["plain", "halt"],
                         "the key only on the halt, absent = plain")

    def test_history_goldens(self):
        log = STREAMS["log"]["events"]
        self.assertEqual([e["seq"] for e in log], list(range(len(log))))
        warn = [e for e in log if kb.LEVELS.index(
            e["level"]) >= kb.LEVELS.index("warn")]
        last2 = STREAMS["history_last_2_warn"]["events"]
        self.assertEqual(last2[:-1], warn[-2:])
        self.assertEqual(
            last2[-1], {"cmd": "backlog_end", "next_seq": len(log), "lost": 0})
        since3 = STREAMS["history_since_3"]["events"]
        self.assertEqual(since3[:-1], log[3:])
        self.assertEqual(STREAMS["history_without_log"]
                         ["answer"]["cmd"], "error")

    def test_socket_paths(self):
        for case in json.loads((FIXTURES / "socket_paths.json").read_text(encoding="utf-8")):
            with self.subTest(case["log_dir"]):
                expected = case["path"].replace("{uid}", str(os.getuid()))
                self.assertEqual(str(kb.control_socket_path(
                    Path(case["log_dir"]))), expected)

    @unittest.skipUnless(shutil.which("jq"), "jq not installed")
    def test_golden_json_survives_the_formatter(self):
        # the formatter (`formatting .`) runs `jq .` over every *.json: the tool writes that form
        for path in sorted(FIXTURES.glob("*.json")):
            with self.subTest(path.name):
                formatted = subprocess.run(["jq", "."], stdin=path.open("rb"), capture_output=True,
                                           check=True).stdout
                self.assertEqual(formatted, path.read_bytes())


class Conveniences(unittest.TestCase):
    """The helpers beside the protocol: words of a read answer, the liveness check, the .rttlog reader."""

    def test_words_are_little_endian(self):
        answer = {"cmd": "read", "unix_us": 7,
                  "data": ["78563412", "ffffffff", "01"]}
        self.assertEqual(kb.words(answer), (7, [0x12345678, 0xFFFFFFFF, 1]))
        self.assertEqual(kb.req_read_words([0x20000000, 0x20000004]),
                         kb.req_read([(0x20000000, 4), (0x20000004, 4)]))

    def test_alive_without_a_printer(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "control.sock"
            self.assertFalse(kb.alive(path))   # no file
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(path))
            # a socket file nobody accepts on: a killed printer's
            listener.close()
            self.assertFalse(kb.alive(path))

    def test_alive_answering_wrongly_raises(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "control.sock"
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(path))
            listener.listen(1)

            def serve():
                conn, _ = listener.accept()
                with conn:
                    conn.recv(100)
                    conn.sendall(b'{"cmd":"error","error":"bad line"}\n')

            t = threading.Thread(target=serve)
            t.start()
            with self.assertRaises(kb.ControlError):
                kb.alive(path)
            t.join()
            listener.close()

    def test_rttlog_rows(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "x.rttlog"
            path.write_text(",".join(kb.RTTLOG_COLUMNS) + "\n"
                            + '1970-01-01T00:00:00Z,1,"a, b.cpp",3,"f",info,0.5,"say ""hi""\\n",mod\n',
                            encoding="utf-8")
            rows = list(kb.rttlog_rows(path))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0][2], "a, b.cpp")
            # the {:?} escape stays as the printer wrote it
            self.assertEqual(rows[0][7], 'say "hi"\\n')
            path.write_text("1,2,3\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                list(kb.rttlog_rows(path))


@unittest.skipUnless(os.environ.get("UC_LOG_CONTROL_PROTOCOL_TOOL"),
                     "UC_LOG_CONTROL_PROTOCOL_TOOL: the fake server (ctest sets it)")
class AgainstTheServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory(prefix="kb_protocol_")
        cls.path = Path(cls.dir.name) / "control.sock"
        cls.server = subprocess.Popen([os.environ["UC_LOG_CONTROL_PROTOCOL_TOOL"], "serve", str(cls.path)],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        ready = cls.server.stdout.readline()
        if not ready.startswith("ready"):
            cls.server.kill()
            raise RuntimeError(f"the fake server did not come up: {ready!r}")

    @classmethod
    def tearDownClass(cls):
        cls.server.stdin.close()
        try:
            cls.server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()
            raise
        finally:
            cls.dir.cleanup()

    def test_exchanges(self):
        with kb.Connection(self.path) as c:
            for name, built in BUILT.items():
                golden = TRANSCRIPT[name]["answer"]
                with self.subTest(name):
                    c.send(built)
                    got = c.next()
                    if TRANSCRIPT[name]["stable"]:
                        self.assertEqual(got, golden)
                    else:
                        self.assertEqual(got["cmd"], golden["cmd"])

    def test_request_helper(self):
        self.assertEqual(kb.request(self.path, kb.req_ping()), {"cmd": "ping"})
        with self.assertRaises(kb.ControlError):
            kb.request(self.path, kb.req_flash())

    # The fake server publishes the golden events on every reset, so its seqs depend on how many
    # resets came before: they are compared for order, not value.
    def test_stream(self):
        for name, built in SUBSCRIBED.items():
            if name.startswith("history"):
                continue
            with self.subTest(name), kb.Connection(self.path) as stream:
                self.assertEqual(stream.ask(built), STREAMS[name]["answer"])
                kb.request(self.path, kb.req_reset())
                expected = STREAMS[name]["events"]
                got = [stream.next() for _ in expected]
                self.assertEqual(without_seq(got), without_seq(expected))
                seqs = [e["seq"] for e in got if e["cmd"] == "log"]
                self.assertEqual(seqs, sorted(seqs))
                # and nothing else: the next reset's events come next (the fake publishes the same ones on every
                # reset) - no quiet time decides it
                kb.request(self.path, kb.req_reset())
                self.assertEqual(without_seq(
                    [stream.next()]), without_seq(expected[:1]))

    def test_history(self):
        kb.request(self.path, kb.req_reset())
        name = "history_last_2_warn"
        with kb.Connection(self.path) as stream:
            self.assertEqual(stream.ask(
                SUBSCRIBED[name]), STREAMS[name]["answer"])
            expected = STREAMS[name]["events"]
            got = [stream.next() for _ in expected]
            self.assertEqual(without_seq(got), without_seq(expected))
            # follow false: closed after backlog_end
            with self.assertRaises(kb.ControlError):
                stream.next()
        lines, end = kb.history(self.path, min_level="warn", last=2)
        self.assertEqual(without_seq(lines), without_seq(expected[:-1]))
        self.assertEqual(end["lost"], 0)
        with self.assertRaises(kb.ControlError):
            kb.request(self.path, SUBSCRIBED["history_without_log"])

    def test_history_then_live(self):
        """A line published after the history is sent live, once."""
        kb.request(self.path, kb.req_reset())
        with kb.Connection(self.path) as stream:
            stream.ask(kb.req_subscribe(["log"], last=1))
            last = stream.next()
            end = stream.next()
            self.assertEqual(end["cmd"], "backlog_end")
            self.assertEqual(end["next_seq"], last["seq"] + 1)
            kb.request(self.path, kb.req_reset())
            first_live = stream.next()
            self.assertEqual(first_live["seq"], end["next_seq"])


if __name__ == "__main__":
    unittest.main()
