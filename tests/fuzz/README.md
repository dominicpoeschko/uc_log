# Fuzzers of the printer's control socket and its RTT stream

Everything reaching `control.sock` is untrusted: a client's request lines, and the target's log
text through the streams; so is every byte the printer reads over RTT. Four libFuzzer harnesses
(ASan + UBSan; `UC_LOG_BUILD_FUZZERS`, on by default when the compiler ships libFuzzer):

| fuzzer | input | holds |
|---|---|---|
| `request` | a client's line | `controlAnswerLine` (golden transcript's fake target) answers one line of valid JSON (UTF-8, no raw control characters) that reads back as an `Answer` and writes out as itself |
| `event` | a message as RTT delivers it | the same for every event line from `LogEntry`'s parser, `toLogLine`, `toMetricSamples`, `toStatusMessage` |
| `session` | first byte: chunk size and whether the client reads back; rest: bytes | a real `ControlServer` on a unix socket, fed in chunks, the client hanging up or half-closing: every line back is JSON, and a fresh client's `ping` is answered within 10 s afterwards |
| `rtt_stream` | first byte: seeds the piece sizes, odd puts a run of valid frames first; rest: the stream | `RttChannel` + remote_fmt's parser over the stream in arbitrary pieces: every decoded message fits a line, the buffer stays within `maxBufferSize`, and after the stream ends no frame start survives the resync |

A longer run: `just fuzz <name> [seconds]` (`request`, `event`, `session` or `rtt_stream`; it builds the clang tree
`build/clang` first), or by hand:

```sh
b=build/clang/tests
cmake --build build/clang --target uc_log_control_protocol_tool uc_log_fuzz_control_request
mkdir -p $b/fuzz_corpus/request
$b/uc_log_control_protocol_tool seeds $b/fuzz_seeds
$b/uc_log_fuzz_control_request -max_total_time=300 -timeout=30 -dict=tests/fuzz/control_protocol.dict \
    $b/fuzz_corpus/request $b/fuzz_seeds/request tests/fuzz/regressions/request
```

`rtt_stream` takes no dictionary (its input is a byte stream), and its `regressions/rtt_stream/` exists only once a
run has found something (ctest and `just fuzz` leave a missing one out).

ctest runs each for a fixed number of inputs (20 000) with `-seed=1 -use_cmp=0`, never for a time - the session
fuzzer only replays its corpus there (`-runs=0`: its coverage includes the server's threads, whose interleaving
differs between runs, so its new inputs would too; it runs on the server's virtual clock, uc_log/printer/Clock.hpp) - and its writable corpus `fuzz_new/<name>` is emptied first: every run on every machine tries the same inputs
(the fuzzers are built with `-fno-sanitize-coverage=stack-depth` - that feature depends on where ASLR put the stack,
and `-use_cmp` feeds pointer values into the mutations; either made a seeded run take another path each time). With
`control_protocol.dict`, over two read-only corpora:

- the seeds, generated, not committed: `uc_log_control_protocol_tool seeds <dir>` writes the golden
  transcript's requests (and a few log messages, and rtt_stream's frame-start edge cases) to
  `build/<config>/tests/fuzz_seeds/<name>/`; the ctest fixture `uc_log_fuzz_seeds` runs it, so the seeds follow the
  protocol, and ctest `uc_log_fuzz_dict` fails when `control_protocol.dict` lacks a word of the golden requests (a new
  command or key: add its quoted token);
- `regressions/<name>/`, committed: one input per bug found, named for what broke.

New corpus entries of a long run go to `build/<config>/tests/fuzz_corpus/<name>/`. A finding lands in
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
