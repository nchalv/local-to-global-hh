#!/usr/bin/env python3
import argparse
import csv
from collections import Counter
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate artifact smoke-test CSV output.")
    parser.add_argument("csv", type=Path)
    parser.add_argument("--expected-windows", type=int, required=True)
    parser.add_argument("--expected-methods", type=int, required=True)
    args = parser.parse_args()

    with args.csv.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit("smoke CSV is empty")

    required = {"window", "method", "hh_precision", "hh_recall", "hh_f1"}
    missing = required.difference(rows[0])
    if missing:
        raise SystemExit("smoke CSV is missing columns: " + ", ".join(sorted(missing)))

    counts = Counter(int(row["window"]) for row in rows)
    expected_windows = set(range(args.expected_windows))
    if set(counts) != expected_windows:
        raise SystemExit(f"unexpected windows: {sorted(counts)}")
    if any(count != args.expected_methods for count in counts.values()):
        raise SystemExit(f"unexpected method counts per window: {dict(sorted(counts.items()))}")

    methods = sorted({row["method"] for row in rows})
    if len(methods) != args.expected_methods:
        raise SystemExit(f"expected {args.expected_methods} methods, found {len(methods)}")

    for row in rows:
        for field in ("hh_precision", "hh_recall", "hh_f1"):
            value = float(row[field])
            if not 0.0 <= value <= 1.0:
                raise SystemExit(f"invalid {field}={value} in window {row['window']}")

    print(f"validated {len(rows)} rows across {args.expected_windows} windows")
    print("methods:")
    for method in methods:
        print(f"  {method}")


if __name__ == "__main__":
    main()
