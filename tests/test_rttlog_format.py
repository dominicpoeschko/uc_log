#!/usr/bin/env python3
"""Reads the .rttlog rows uc_log_test_log_format wrote with Python's csv module, the way the bench
scripts (smoke.py, hwtest.py, combo_check.py) do: every row has the 9 columns, and each text column
is exactly what the C++ side expects a CSV reader to see.

    test_rttlog_format.py <dir with format_sample.rttlog and format_sample.expected>
"""

import csv
import sys
from pathlib import Path


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    with open(d / "format_sample.rttlog", newline="", encoding="utf-8", errors="strict") as f:
        rows = list(csv.reader(f))
    expected = (
        d / "format_sample.expected").read_text(encoding="utf-8").split("\n")[:-1]
    failures = 0
    header = ["recv_time_utc", "channel", "file", "line", "function", "log_level", "uc_time",
              "message", "module"]
    if rows[0] != header:
        print(f"FAIL: header {rows[0]}")
        failures += 1
    if len(expected) != 4 * (len(rows) - 1):
        print(f"FAIL: {len(rows) - 1} rows, {len(expected) // 4} expected")
        failures += 1
    for i, row in enumerate(rows[1:]):
        if len(row) != 9:
            print(f"FAIL: row {i}: {len(row)} columns: {row}")
            failures += 1
            continue
        want = expected[4 * i:4 * i + 4]
        got = [row[2], row[4], row[7], row[8]]
        if got != want:
            print(f"FAIL: row {i}: csv read {got}, expected {want}")
            failures += 1
    print("all checks passed" if failures == 0 else f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
