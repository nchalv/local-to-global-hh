#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate normalized window-count JSONL.")
    parser.add_argument("path", type=Path)
    parser.add_argument("--expected-windows", type=int)
    parser.add_argument("--label", default="dataset")
    args = parser.parse_args()

    windows: list[int] = []
    total_items = 0
    cardinalities: list[int] = []
    with args.path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{args.path}:{line_number}: invalid JSON: {exc}") from exc
            if not isinstance(record, dict) or "window" not in record or "counts" not in record:
                raise SystemExit(f"{args.path}:{line_number}: expected window and counts fields")
            window = int(record["window"])
            counts = record["counts"]
            if not isinstance(counts, dict) or not counts:
                raise SystemExit(f"{args.path}:{line_number}: counts must be a non-empty object")
            values = []
            for key, value in counts.items():
                if not isinstance(key, str) or not key:
                    raise SystemExit(f"{args.path}:{line_number}: keys must be non-empty strings")
                if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                    raise SystemExit(f"{args.path}:{line_number}: counts must be positive integers")
                values.append(value)
            windows.append(window)
            total_items += sum(values)
            cardinalities.append(len(values))

    if not windows:
        raise SystemExit(f"{args.path}: no windows found")
    if len(windows) != len(set(windows)):
        raise SystemExit(f"{args.path}: duplicate window identifiers")
    expected_ids = list(range(len(windows)))
    if sorted(windows) != expected_ids:
        raise SystemExit(f"{args.path}: windows must be contiguous and zero based")
    if args.expected_windows is not None and len(windows) != args.expected_windows:
        raise SystemExit(
            f"{args.path}: expected {args.expected_windows} windows, found {len(windows)}"
        )

    print(
        f"{args.label}: windows={len(windows)}, "
        f"mean_items={total_items / len(windows):.1f}, "
        f"mean_distinct={sum(cardinalities) / len(cardinalities):.1f}"
    )


if __name__ == "__main__":
    main()
