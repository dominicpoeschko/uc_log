# Local development helpers for uc_log.
#
# Builds live in build/<config>/ - one Ninja tree per configuration, so
# switching configs never throws away another config's objects. The configs
# mirror the CI matrix in .github/workflows/ci.yml.
#
#   just                    # build + run the tests with the default config
#   just test clang         # same, with clang
#   just test gcc-tsan rtt  # only tests matching "rtt", tsan config
#   just test-all           # every config, like CI does
#   just printer            # just the uc_log_printer host tool
#
# CI has no SEGGER J-Link DLL, so it configures with -DJLINK_USE_FAKE_DLL=ON and
# the in-process fake stands in. The recipes here do the same by default: the
# tests must not depend on a probe being plugged in. `just real_dll=ON test` to
# build against the actual DLL.
#
# `just --list` shows everything.

set shell := ["bash", "-euo", "pipefail", "-c"]

root := source_directory()
self := source_file()   # not justfile(): that names the ROOT justfile when loaded as a module
build_root := root / "build"
jobs := num_cpus()

# ON builds against the real SEGGER DLL instead of the in-process fake.
real_dll := "OFF"

default_config := "gcc"

# All configurations, in CI order. Used by test-all / build-all.
configs := "gcc gcc-asan gcc-tsan clang clang-asan clang-tsan"

default: test

# Print the compilers and cmake flags for a configuration (see the CI matrix)
[private]
flags config:
    #!/usr/bin/env bash
    set -euo pipefail
    libcxx="-DUSE_STD_LIB=libc++ -DUSE_STDLIB_DEBUG=true"
    case "{{ config }}" in
        gcc)        echo "gcc g++ " ;;
        gcc-asan)   echo "gcc g++ -DUSE_SANITIZER=address -DUSE_STDLIB_DEBUG=true" ;;
        gcc-tsan)   echo "gcc g++ -DUSE_SANITIZER=thread -DUSE_STDLIB_DEBUG=true" ;;
        clang)      echo "clang clang++ " ;;
        clang-asan) echo "clang clang++ $libcxx -DUSE_SANITIZER=address" ;;
        clang-tsan) echo "clang clang++ $libcxx -DUSE_SANITIZER=thread" ;;
        *)
            echo "unknown config '{{ config }}'; known: {{ configs }}" >&2
            exit 1 ;;
    esac

# Wipe a build tree whose recorded source directory is not this one any more -
# what a moved checkout leaves behind. Without this CMake refuses outright:
# "the current CMakeCache.txt directory ... is different than the directory".
#
# Also wipes a tree configured with another generator: every recipe here drives
# Ninja, and CMake refuses to switch a tree's generator in place.
[private]
guard dir src:
    #!/usr/bin/env bash
    set -euo pipefail
    cache="{{ dir }}/CMakeCache.txt"
    [ -f "$cache" ] || exit 0
    why=""
    home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")
    [ "$home" = "{{ src }}" ] || why="source moved ($home)"
    gen=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache")
    [ "$gen" = Ninja ] || why="generator is '$gen', not Ninja"
    if [ -n "$why" ]; then
        echo "==> wiping {{ dir }}: $why"
        rm -rf "{{ dir }}"
    fi

# Configure build/<config> if it does not exist yet (no-op afterwards). Warnings are errors, as in CI
# (.github/workflows/ci.yml): a tree configured before that gets the switch in place, once.
configure config=default_config: (guard (build_root / config) root)
    #!/usr/bin/env bash
    set -euo pipefail
    dir="{{ build_root }}/{{ config }}"
    if [ -f "$dir/build.ninja" ]; then
        grep -q '^USE_WARNINGS_AS_ERRORS:[A-Z]*=ON$' "$dir/CMakeCache.txt" \
            || cmake -B "$dir" -DUSE_WARNINGS_AS_ERRORS=ON > /dev/null
        exit 0
    fi
    read -r cc cxx rest < <(just --justfile "{{ self }}" flags "{{ config }}")
    launcher=""
    command -v ccache >/dev/null && launcher="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    env CC="$cc" CXX="$cxx" cmake -S "{{ root }}" -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DUC_LOG_BUILD_TESTS=ON -DUC_LOG_BUILD_TEST_GUI=ON \
        -DJLINK_USE_FAKE_DLL={{ if real_dll == "ON" { "OFF" } else { "ON" } }} \
        -DUSE_FORCE_FETCH=ON -DUSE_WARNINGS_AS_ERRORS=ON $launcher $rest

# Wipe build/<config> and configure it again
reconfigure config=default_config: (clean config) (configure config)

# Build everything of a configuration (incremental)
build config=default_config: (configure config)
    cmake --build "{{ build_root }}/{{ config }}" -j{{ jobs }}

# Build and run the tests; FILTER is a ctest -R regex
test config=default_config filter="": (build config)
    ctest --test-dir "{{ build_root }}/{{ config }}" --output-on-failure -j{{ jobs }} \
        {{ if filter == "" { "" } else { "-R '" + filter + "'" } }}

# Rerun only the tests that failed last time
retest config=default_config:
    ctest --test-dir "{{ build_root }}/{{ config }}" --output-on-failure -j{{ jobs }} --rerun-failed

# List the tests of a configuration
list config=default_config: (configure config)
    ctest --test-dir "{{ build_root }}/{{ config }}" -N

# request (a client's line), event (the target's log text on its way to the stream), session (bytes
# into a real server), rtt_stream (the printer's frame extraction over a torn byte stream); details
# in tests/fuzz/README.md
# Fuzz request|event|session|rtt_stream for SECONDS (corpus: build/<config>/tests/fuzz_corpus, findings: ./crash-*)
fuzz name="request" seconds="120" config="clang": (build config)
    #!/usr/bin/env bash
    set -euo pipefail
    tests="{{ build_root }}/{{ config }}/tests"
    case "{{ name }}" in
        request|event|session) fuzzer="$tests/uc_log_fuzz_control_{{ name }}"
                               extra=(-dict="{{ root }}/tests/fuzz/control_protocol.dict") ;;
        rtt_stream) fuzzer="$tests/uc_log_fuzz_rtt_stream"; extra=() ;;
        *) echo "just fuzz: no fuzzer {{ name }} (request, event, session, rtt_stream)" >&2; exit 2 ;;
    esac
    mkdir -p "$tests/fuzz_corpus/{{ name }}"
    "$tests/uc_log_control_protocol_tool" seeds "$tests/fuzz_seeds"
    corpora=("$tests/fuzz_corpus/{{ name }}" "$tests/fuzz_seeds/{{ name }}")
    [ -d "{{ root }}/tests/fuzz/regressions/{{ name }}" ] && corpora+=("{{ root }}/tests/fuzz/regressions/{{ name }}")
    "$fuzzer" -max_total_time={{ seconds }} -timeout=30 "${extra[@]}" "${corpora[@]}"

# The host tool the firmware repos' devcycle.sh runs
printer config=default_config: (configure config)
    cmake --build "{{ build_root }}/{{ config }}" -j{{ jobs }} --target uc_log_printer
    @echo "{{ build_root }}/{{ config }}/uc_log_printer"

# Build every configuration
build-all:
    #!/usr/bin/env bash
    set -euo pipefail
    for c in {{ configs }}; do
        echo "==> build $c"
        just --justfile "{{ self }}" build "$c"
    done

# Run the tests in every configuration, like CI (continues after failures)
test-all filter="":
    #!/usr/bin/env bash
    set -uo pipefail
    failed=()
    for c in {{ configs }}; do
        echo "==> test $c"
        just --justfile "{{ self }}" test "$c" "{{ filter }}" || failed+=("$c")
    done
    if [ ${#failed[@]} -ne 0 ]; then
        echo "FAILED: ${failed[*]}" >&2
        exit 1
    fi
    echo "all configurations passed"

# Remove build/<config>
clean config=default_config:
    rm -rf "{{ build_root }}/{{ config }}"

# Remove every build directory
clean-all:
    #!/usr/bin/env bash
    set -euo pipefail
    rm -rf "{{ build_root }}"
    echo "build trees removed"

# --- clang-tidy ----------------------------------------------------------------

# Kvasir_SDK/tools/tidy.py with the SDK's .clang-tidy over build/clang (rtt, remote_fmt and string_constant count as this repository): findings in this repository fail, the
# other libraries' count as foreign. `just tidy --changed`, `just tidy --fix`, `--audit <check>`, `--only '<glob>'`.
# clang-tidy over the clang host tree (exit 0 clean, 1 findings)
[positional-arguments]
tidy *args: (configure "clang")
    python3 -B "{{ root }}/../tools/tidy.py" --tree "{{ build_root }}/clang" --repo "{{ root }}" --nested "$@"

# The configured clang tree `tidy` reads, if there is one (the workspace's `just tidy` collects them)
tidy-trees:
    @[ ! -f "{{ build_root }}/clang/compile_commands.json" ] || echo "{{ build_root }}/clang"
