# Fuzzers of the printer's control socket

Everything reaching `control.sock` is untrusted: a client's request lines, and the target's log
text through the streams. Three libFuzzer harnesses (ASan + UBSan;
`UC_LOG_BUILD_FUZZERS`, on by default when the compiler ships libFuzzer):

| fuzzer | input | holds |
|---|---|---|
| `request` | a client's line | `controlAnswerLine` (golden transcript's fake target) answers one line of valid JSON (UTF-8, no raw control characters) that reads back as an `Answer` and writes out as itself |
| `event` | a message as RTT delivers it | the same for every event line from `LogEntry`'s parser, `toLogLine`, `toMetricSamples`, `toStatusMessage` |
| `session` | first byte: chunk size and whether the client reads back; rest: bytes | a real `ControlServer` on a unix socket, fed in chunks, the client hanging up or half-closing: every line back is JSON, and a fresh client's `ping` is answered within 10 s afterwards |

A longer run (`request`, `event` or `session`; a clang build, here `build/clang`):

```sh
b=build/clang/tests
cmake --build build/clang
mkdir -p $b/fuzz_corpus/request
$b/uc_log_control_protocol_tool seeds $b/fuzz_seeds
$b/uc_log_fuzz_control_request -max_total_time=300 -timeout=30 -dict=tests/fuzz/control_protocol.dict \
    $b/fuzz_corpus/request $b/fuzz_seeds/request tests/fuzz/regressions/request
```

ctest runs each for 20 000 inputs or 30 s, whichever ends first, with `control_protocol.dict` over two read-only corpora:

- the seeds, generated, not committed: `uc_log_control_protocol_tool seeds <dir>` writes the golden
  transcript's requests (and a few log messages) to `build/<config>/tests/fuzz_seeds/<name>/`; the
  ctest fixture `uc_log_fuzz_seeds` runs it, so the seeds follow the protocol;
- `regressions/<name>/`, committed: one input per bug found, named for what broke.

New corpus entries go to `build/<config>/tests/fuzz_corpus/<name>/`. A finding lands in
`./crash-<hash>` (git-ignored): replay it with `build/clang/tests/uc_log_fuzz_control_<name>
crash-<hash>`, fix, add a unit test (control_protocol_tests.cpp / tcp_server_tests.cpp), and move
the input to `regressions/<name>/<what-broke>`.

What the files in `regressions/` guard against:

- **event** `metric-nan-value`: `@METRIC(x::y[]=nan)` made glaze write `"value":null`; non-finite
  values are left out of the metrics stream (the log line keeps the text).
- **event** `log-level-7`: a garbled frame with log level 7 - only 0-5 exist.
- **request** `unterminated-json`: glaze reads up to a `'\0'`, which a view does not have.
- **session** `half-close-subscriber`: a half-closed subscriber on a quiet stream leaked its client
  and socket.
