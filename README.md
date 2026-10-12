# UC_LOG

A great way to log your [kvasir-io project](https://github.com/kvasir-io) with an [jLink](https://www.segger.com/downloads/jlink/).
Looking for an example? Check out the [rp2040_example](https://github.com/kvasir-io/rp2040_example)

## Installing / Getting started

A quick guide to get the `uc_log` example GUI up and running:

```shell
git clone --recursive git@github.com:dominicpoeschko/uc_log.git
```

```shell
cd uc_log
```

```shell
mkdir build
```

```shell
cmake .. -DUC_LOG_BUILD_TEST_GUI=ON
```

```shell
cmake --build .
```

```shell
./uc_log_gui_test
```

## Features
- Terminal application
- Terminal GUI
- Enable/Disable log levels (trace, debug, info, warn. error, crit)
- Channel Selection
- Log filtering
- Display information
    - System Time
    - Function Name
    - Target Time
    - Source Location
    - Log Channel
    - Log Level
- Debug tools
    - Reset Target
    - Reset Debugger
    - Flash Target
- Build function
- Status tab

## Compile-time log level floor

`UC_LOG_MIN_LEVEL` (a `uc_log::LogLevel` enumerator name: `trace`, `debug`, `info`, `warn`, `error`, `crit`;
default `trace`) removes every `UC_LOG_*` call site below that level at compile time. Such a call site produces no code and
no entry in the string catalog, so the format strings do not end up in the firmware. The arguments are
still type-checked, so an unloggable argument is an error regardless of the floor.

```shell
-DUC_LOG_MIN_LEVEL=warn   # keep warn, error, crit
```

The current floor is available as `uc_log::minLevel`. An unknown name is a compile error.

With the Kvasir SDK, pass `MIN_LOG_LEVEL <trace|debug|info|warn|error|crit>` to
`target_configure_kvasir` or `kvasir_executable_variants` instead of setting the define by hand.

## Scoped configuration

The floor and the backend can be set per namespace, class, function or block without touching the call sites
(`uc_log/LogEnv.hpp`, after intel/compile-time-init-build's `CIB_LOG_ENV`). Every `UC_LOG_*` call uses the
`uc_log_env_t` name lookup finds; these macros redeclare it for their scope:

```cpp
namespace kvasir::i2c { UC_LOG_SCOPE_MIN_LEVEL(warn); }      // this driver logs warn and up

struct Motor {
    UC_LOG_SCOPE_BACKEND(MotorTag);                           // ComBackend<MotorTag>, LogClock<MotorTag>
};

void f() {
    UC_LOG_ENV(uc_log::setting::Backend<MotorTag>,            // several settings: one UC_LOG_ENV
               uc_log::setting::MinLevel<uc_log::LogLevel::info>);
    UC_LOG_WITH_ENV(uc_log::setting::MinLevel<uc_log::LogLevel::error>) { UC_LOG_W("dropped"); }
}
```

- A scope can raise the floor above `UC_LOG_MIN_LEVEL`, never lower it. Within that limit the innermost setting wins,
  so a function can lower its namespace's floor again.
- The backend defaults to `uc_log::Tag::User`; another tag needs its own `ComBackend<Tag>` and `LogClock<Tag>`.
- One `UC_LOG_ENV` / `UC_LOG_SCOPE_*` per scope: a second one, even from another header reopening the namespace, is a
  conflicting redeclaration of `uc_log_env_t`. Not at global scope, which holds the default.
- The macros compile without `USE_UC_LOG` too.

### Modules and signatures

Every log line knows the function it was written in, at no cost on the wire: its catalog id is the id of its **call
site** (remote_fmt `REMOTE_FMT_SITE`, `catalog.hpp`) - 0x8000 plus the address of a one-byte tag, one per call site and
per instantiation of the function it is in, in a section `remote_fmt_sites.<n>`. The tag's symbol is mangled with both
the line's format string and the function, so after the link remote_fmt's `tools/extract_sites.py` adds each site's
string and the demangled signature of its function to `<target>_string_constants.json` (`"Sites": {"<id>":
"<signature>"}`), and the printer looks a line's function up by the id it came with. The header is
`("file", line, level, {}, """""")` (`"module", ` before the `"""` for an explicit module). The compiler never reads
`__PRETTY_FUNCTION__` for this: in the constant evaluator that read is what makes compiles slow.

Plain strings (sub-format strings, enum names, units) keep the generator's constant ids (0x0000-0x7FFF): an address is
a literal load, a generated id an immediate. On the target the site section is `remote_fmt_sites 0 (INFO)` in
Kvasir_SDK's `linker/common.ld`, never in the image; a host executable gets remote_fmt's
`cmake/remote_fmt_sites_host.ld`. Without a catalog (`REMOTE_FMT_USE_CATALOG=false`) there are no tags and no
function names.

The printer derives the rest once per site (`detail/SignatureTable.hpp`, `detail/Signature.hpp`):

- the **module**: the scope's components lowercased and joined with `.`, without a leading `Kvasir`, `detail` /
  `Detail`, anonymous namespaces, template arguments and the function itself. `UC_LOG_SCOPE_MODULE("name");` overrides
  it (it travels in the header, `("file", line, level, {}, "usb", """""")`); names are 1-63 characters of
  `a-z 0-9 _ . : / -`.

  | function | module |
  |---|---|
  | `Kvasir::I2C::detail::logUp(...)` | `i2c` |
  | `Kvasir::I2C::Bus<...>::run()` | `i2c.bus` |
  | `Kvasir::USB::CDC::Mixin<...>::handleSetup(...)` | `usb.cdc.mixin` |
  | `WaterMix::Regulator::step(...)` | `watermix.regulator` |
  | `main()`, an `extern "C"` handler | none |

- the **function name**: the enclosing class with abbreviated template arguments, `::`, the function -
  `Device<FakeBusFor<>, FakeClockT<>, Tca9548a, DefaultConfig, NoReset, NoGate>::finishVerify_`; a lambda is
  `function::lambda`. Both compilers give the arguments (`Regulator<int>`), the demangled name has no return type.
- the **signature** itself: Settings, "📜 Full Signatures" shows the selected line's whole signature in a panel under
  the log; a copied line (`y`) and a `.txt` export take it too. The `.rttlog` and the control socket carry the short name only.

The printer shows the module as `[i2c.bus]` (Settings, "Modules") and lists the modules seen as a tree in the Filter
tab: unchecking `i2c` hides it and everything under it (`excludedModules` in `filter.json`). It is the 9th, last column
of the `.rttlog`. `kvasir_bench.py log` / `wait-for` take `--module NAME` / `--not-module NAME` with the same prefix rule.

### The filter file: compiled-out modules

`LogFilter.hpp` reads `uc_log_filter.txt` with `#embed` at compile time; call sites below its floor expand to nothing -
no code, no catalog string, no tag:

```
# uc_log_filter.txt
* = info          # every line: the global floor
i2c = warn        # i2c and everything below it (i2c.bus, i2c.device ...)
usb.cdc = off     # not one line
```

Levels `trace debug info warn error crit off`; a module matches itself and its children, the longest rule wins and
replaces the scope's `UC_LOG_SCOPE_MIN_LEVEL`; nothing goes below `*` or `UC_LOG_MIN_LEVEL`. A bad file is a compile
error (`logFilterFileIsInvalid`). The file is found in the directories given with `--embed-dir=` (not `-I`); with
Kvasir_SDK, pass `LOG_FILTER <file>` to `target_configure_kvasir` / `kvasir_executable_variants`, or put a
`uc_log_filter.txt` next to the project's CMakeLists. Without a file nothing is filtered. A line's module for the
filter is its explicit module or the one the same scan derives from `__PRETTY_FUNCTION__` - read only when the file
has module rules. The printer gets the file too (`--log_filter`) and marks the rules in the Filter tab's module tree
(`compiled ≥ warn`, `⛔ compiled out`), so a module that sends nothing says why.

## Backend transfer hooks

A `ComBackend` may optionally provide `initTransfer()` and `finalizeTransfer()`; the printer calls them
before the first and after the last `write` of one log entry, so a backend that assembles whole entries (for
example a queue that drops entries as a unit) can tell where an entry starts and ends. Like `write`, each may
be templated on the `LogLevel` (`template<LogLevel> static void initTransfer()`), in which case that form is
preferred over the plain one. A backend without them works unchanged.

An entry reaches `write` in one piece as long as it fits remote_fmt's staging bytes (`REMOTE_FMT_STAGING_BYTES`,
32 by default: an entry is gathered on the stack first), so an RTT ring in skip mode drops such an entry whole
rather than its middle. A longer one is written in pieces.

A log line carries its time as the clock's tick count; the clock's period is text in the cataloged header
(`123456[1/48000000]s` once the printer has put the count in). `LogClock<Tag>::now()` returns a `time_point` or a
`duration` with an integral `rep`.

## Building without `USE_UC_LOG`

Without `USE_UC_LOG` every `UC_LOG_*` call site still names its arguments, behind a constant-false condition: nothing
is evaluated and no code is emitted, but a value or helper that exists only to be logged counts as used, so it raises no
unused warning. Two things follow for code that has only ever been built without logging:

- every argument must compile in that build too: a variable declared under `#ifdef USE_UC_LOG` is an error;
- a `UC_LOG_*` call is a statement, not an expression, as it always was with `USE_UC_LOG`.

## ISR log rings

The RTT backends (`DefaultRttComBackend`, `MultiChannelRttComBackend`, `MulticoreRttComBackend`) take an ISR policy as
their last template argument. It decides how the records of interrupts at different priority levels are kept apart
(`src/uc_log/IsrPolicy.hpp`):

- `IsrPolicy::SingleLevel<SilentLevels<...>>`, the default: one ISR ring, and every interrupt that may log runs at one
  level.
- `IsrPolicy::PerLevel<ActivePriority, Levels<...>, SilentLevels<...>>`: one ISR ring per listed level.
- `IsrPolicy::MaskedRecord`: one ISR ring, and each record from an ISR is written with interrupts masked.

With the Kvasir SDK, Startup checks the policy against the priorities the init steps set and fails the build when they
do not fit, so an application whose logging interrupts use more than one level has to choose a policy.

## RTT throughput, ring sizes, and the two ways the printer reads RTT

A ring holds what the target logs while the printer is busy elsewhere: **its size divided by the log rate is the time
it bridges.** At 100 KB/s a KiB is ~10 ms; at 600 KB/s (about what one SWD probe carries) a KiB is ~1.7 ms. The
printer reads a whole ring in one pass when only one ring has bytes, and at most half of each when several have, so a
ring should bridge a few of its own passes: as a rule, at least 50 ms of the image's peak log rate (16 KiB for a
thread that logs 300 KB/s in bursts, 4 KiB for an interrupt that logs 80 KB/s). What does not fit is dropped whole and
announced ("⚠ N log records lost on this channel"), never torn.

The sizes are template arguments of the backend: `DefaultRttComBackend<..., MainBufferSize, IsrBufferSize, ...>`
(the thread ring, then each ISR ring), `MultiChannelRttComBackend` / `MulticoreRttComBackend` take
`ChannelSizes<thread, isr..., thread, isr...>` (per logical channel or core, the thread ring then its ISR rings). They
cost RAM one to one.

Measured with a J-Link at 15 MHz SWD on an RP2350, one producer logging as fast as asked (records of ~42 bytes),
nothing else on the probe's USB bus; "lossless up to" is the highest rate of the sweep (5k, 10k, 12.5k, 15k, 20k ...
records/s, each rate repeated) without a dropped record; near the edge single runs lose a few records now and then
(host scheduling), so the default reader's ISR figure is the rate where most runs lose nothing:

| | 16 KiB thread ring: lossless up to / saturated | 4 KiB ISR ring: lossless up to / saturated |
|---|---|---|
| the printer's own RTT reader (default) | ~430 KB/s / ~590 KB/s | ~275 KB/s (at ~340 KB/s 0.1-1.9 % lost) / ~400-450 KB/s |
| the J-Link DLL's RTT (`UC_LOG_RTT_VIA_DLL=1`) | ~540 KB/s / ~665 KB/s | ~580 KB/s / ~650 KB/s |

The raw probe reads ~710 KB/s. A small ring suffers most with the default reader (4 KiB bridge ~6 ms at that rate,
about one pass): give an interrupt that logs hard a bigger ring.

**The default reader** (the printer reads the rings through memory accesses itself) moves a ring's read offset only
past bytes the printer holds: a stop, a reset or a flash leaves every unread byte in the target for the next session,
and a reset nobody announced is fenced by the image's RTT epoch, so no line arrives twice. Its memory reads are
synchronous (read over SWD, then ship over USB), which costs ~10 % of throughput against the DLL.

**`UC_LOG_RTT_VIA_DLL=1`** in the printer's environment reads RTT through the J-Link DLL's own RTT, which polls from the
probe: the probe's full throughput, for measurements of very high log rates. Its known weaknesses: when a session ends
(the printer stops, resets or flashes the target) the DLL drops what it had read from the target and not yet handed
over, without a message; and after a reset nobody announced it can read part of the old ring a second time. Use it
for throughput, not where every line counts.

## Reboots, the RTT epoch and boots nobody read

The backends keep three words first in `.noInitLowRam` (`uc_log_boot_words`): an RTT epoch that is odd while a boot
re-initialises the control block and even once it is the new boot's (`preMemoryInit` / `postMemoryInit` in Startup),
and a count of boots that logged at least one record, with its complement as a check word
(`uc_log::bootsThatLogged()`). The printer reads the epoch after each ring's read-offset write: a boot since the
session began drops what that pass read and the next session reads the new boot from its first byte. From the count it
says how many boots passed unread between two sessions ("N boot(s) passed unread"), and when its own download wrote
over the count ("the boot count starts again").

## Chip settings the printer takes

`target_add_uc_log_rtt_jlink` writes the printer's arguments for the chip to `<build>/<target>.printer_args` (tools such as
`kvasir_bench.py` start the printer from it). Besides probe, device and speed the chip package can set:

| option | what |
|---|---|
| `--flash_ready_signature` | `<address>,<mask>,<value>[+...]`: register words that hold while the flash is mapped; the firmware check and control-socket reads in the flash window wait for them (an XIP read while a boot ROM drives the flash can hang it) |
| `--flash_window` | `<base>,<size>`: the address range those reads are gated in |
| `--park_signature`, `--stall_signature` | register words of a chip parked or stalled in its boot ROM, and the writes that undo a stall; the printer recovers such a chip by itself |
| `--pre_reset_command`, `--flash_uncached_offset` | J-Link commands before a reset; the uncached alias of the flash for the firmware check |

## The printer's control socket

`uc_log_printer` serves one socket for tools: `control.sock` in the log directory (`build/rtt_log/<target>/`; too
long a path hashes into `/tmp/uc_log_<uid>/`), or `CONTROL_PORT` with `--transport tcp`. JSON lines, every object
tagged by `"cmd"`:

| kind | request (`cmd`) | answer |
|---|---|---|
| request/answer | `ping` | the protocol version |
| | `status` | running, halted, flashing, sessions, firmware check (match / different / unchecked, build CRC), status errors, `log_seq` (the next log line's number), `started_us` |
| | `messages` | the newest n lines of the Status tab |
| | `read` | target memory while the core runs, hex per piece, with the host time it came back |
| | `wait` | read a value until a condition holds or a timeout runs out |
| | `reset`, `flash` | done through the printer's own probe connection; the log session survives |
| | `halt`, `go` | the core halted / resumed through the probe's own calls, answered with `halted`; `write` refuses DHCSR (a halt written there behind the J-Link DLL's back hangs the DLL for good) |
| stream | `subscribe` (`log`, `messages`, `metrics`; module and level filters) | after the answer only `log` / `message` / `metric` events, and `dropped` (`lines`) where events were dropped for this client |
| history | `subscribe` with `since_seq` or `last` (log stream) | the kept log lines from that seq on / the newest n through the filters, a `backlog_end` (`next_seq`, `lost`), then live lines - or, with `"follow":false`, the connection closes |

Every log line carries `seq`, counted from 0 at the printer's start. The printer keeps its log lines for the history,
oldest dropped past `--control_history_mb` (128 MiB); a `backlog_end` counts what was asked for but dropped. History
and live lines join without a gap or a repeat, and the history goes out as fast as the client reads it.

Clients do not hold each other up. A subscriber that stops reading has its own send queue (4 MiB), and the lines
past it are dropped for that subscriber only. The gap does not stay silent: once the queue has room again, the
client gets a `{"cmd":"dropped","lines":N}` event where the gap is, with N the exact number of events it missed.
The event comes ahead of the next line, or within a second when no line follows. `kvasir_bench wait-for` fails on
it with exit 5. `ping` and `subscribe` need no target, so the socket thread answers
them itself: they still answer when every worker waits in a probe call. A self-check notices when the socket thread
has not turned for 10 s, or when no worker has been free for that long. It writes one Status line saying so, with
each target request in flight and how long it has run, and writes another when it is over.

Failures answer `{"cmd":"error","error":"..."}`. The protocol is the glaze structs in
`src/uc_log/detail/ControlProtocol.hpp`. `doc/control_protocol/` holds what is generated from them - `schema.json`,
golden transcripts against a fake target, the stream of a fixed log, the socket path rule - and both ends are tested
against it: `uc_log_test_control_protocol` compares byte for byte, `tools/test_uc_log_client.py` checks the client
(`tools/uc_log_client.py`, used by Kvasir_SDK's `kvasir_bench.py`) and runs it against a real server. After a
deliberate change: `cmake --build <build> --target update_control_protocol`, raise `ProtocolVersion`, update the
client. The duplex channels keep their own sockets (`duplex.<n>.sock`).

## Contributing

"If you'd like to contribute, please fork the repository and use a feature
branch. Pull requests are warmly welcome."

## Links

- Repository: https://github.com/dominicpoeschko/uc_log
- Issue tracker: https://github.com/dominicpoeschko/uc_log/issues
- Related projects:
  - [kvasir-io](https://github.com/kvasir-io/Kvasir)
  - [rp2040_example](https://github.com/kvasir-io/rp2040_example)
  - [rtt](https://github.com/dominicpoeschko/rtt)
  - [remote_fmt](https://github.com/dominicpoeschko/remote_fmt)
  - [jlink connector](https://github.com/dominicpoeschko/jlink)
  - [cmake_helpers](https://github.com/dominicpoeschko/cmake_helpers)

## Licensing

"The code in this project is licensed under [MIT license](https://github.com/dominicpoeschko/uc_log/blob/master/LICENSE)."
