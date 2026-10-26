#!/usr/bin/env python3
"""Reads the .rttlog rows uc_log_test_log_format wrote with uc_log_client.rttlog_rows (Python's csv module),
the way a script that reads a .rttlog does: valid UTF-8, the header, every row has the 9 columns, and each
text column is exactly what the C++ side expects a CSV reader to see.

    test_rttlog_format.py <dir with format_sample.rttlog and format_sample.expected>
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import uc_log_client  # noqa: E402


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    path = d / "format_sample.rttlog"
    # the printer writes UTF-8 only
    text = path.read_text(encoding="utf-8", errors="strict")
    failures = 0
    header = text.split("\n", 1)[0]
    if header != ",".join(uc_log_client.RTTLOG_COLUMNS):
        print(f"FAIL: header {header!r}")
        failures += 1
    try:
        rows = list(uc_log_client.rttlog_rows(path))
    except ValueError as e:
        print(f"FAIL: {e}")
        return 1
    expected = (
        d / "format_sample.expected").read_text(encoding="utf-8").split("\n")[:-1]
    if len(expected) != 4 * len(rows):
        print(f"FAIL: {len(rows)} rows, {len(expected) // 4} expected")
        failures += 1
    for i, row in enumerate(rows):
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
