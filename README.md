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

## Backend transfer hooks

A `ComBackend` may optionally provide `initTransfer()` and `finalizeTransfer()`; the printer calls them
before the first and after the last `write` of one log entry, so a backend that assembles whole entries (for
example a queue that drops entries as a unit) can tell where an entry starts and ends. Like `write`, each may
be templated on the `LogLevel` (`template<LogLevel> static void initTransfer()`), in which case that form is
preferred over the plain one. A backend without them works unchanged.

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
