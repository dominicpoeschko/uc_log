# UC_LOG

A great way to log your [kvasir-io project](https://github.com/kvasir-io) with an (jLink)[https://www.segger.com/downloads/jlink/].
If you look for an example projet check this out: (rp2040_example)[https://github.com/kvasir-io/rp2040_example]

## Installing / Getting started

A quick introduction to get the uc_log example gui running.

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
- terminal programm
- terminal gui
- enable/disable log level(trace, debug, info, warn. error, crit)
- select different channels
- filter
- display information (system Time, Function Name, Target Time, Source Location, Log Channel, Log Level)
- debug functions (reset target, reset Debugger, Flash Target)
- build function
- status tab

## Contributing

"If you'd like to contribute, please fork the repository and use a feature
branch. Pull requests are warmly welcome."

## Links

- Repository: https://github.com/dominicpoeschko/uc_log
- Issue tracker: https://github.com/dominicpoeschko/uc_log/issues
- Related projects:
  - (kvasir-io)[https://github.com/kvasir-io/Kvasir]
  - (rp2040_example)[https://github.com/kvasir-io/rp2040_example]
  - (rtt)[https://github.com/dominicpoeschko/rtt]
  - (remote_fmt)[https://github.com/dominicpoeschko/remote_fmt]
  - (jlink connector)[https://github.com/dominicpoeschko/jlink]
  - (cmake_helpers)[https://github.com/dominicpoeschko/cmake_helpers]

## Licensing

"The code in this project is licensed under [MIT license](https://github.com/dominicpoeschko/uc_log/blob/master/LICENSE)."
