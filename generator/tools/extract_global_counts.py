#!/usr/bin/env python3
"""Recover per-window global key counts from a partitioned JSON stream."""

from __future__ import annotations

import argparse
from collections import Counter
import gzip
import json
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Collapse a partitioned JSON or JSON.GZ stream into global-count JSONL."
    )
    parser.add_argument("stream", type=Path, help="Partitioned nested stream file.")
    parser.add_argument("--output", required=True, type=Path, help="Output JSONL path.")
    return parser.parse_args()


def open_text(path: Path):
    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8")
    return path.open("r", encoding="utf-8")


def window_order(value: Any) -> tuple[int, Any]:
    try:
        return (0, int(value))
    except (TypeError, ValueError):
        return (1, str(value))


def add_partition(counter: Counter[str], records: Any) -> None:
    if isinstance(records, dict):
        counter.update({str(key): int(count) for key, count in records.items()})
        return
    if not isinstance(records, list):
        raise TypeError(f"unsupported partition payload: {type(records).__name__}")
    if records and isinstance(records[0], (list, tuple)):
        counter.update({str(key): int(count) for key, count in records})
        return
    counter.update(str(key) for key in records)


def main() -> int:
    args = parse_args()
    with open_text(args.stream) as handle:
        windows = json.load(handle)
    if not isinstance(windows, dict):
        raise TypeError("expected a window-indexed JSON object")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    total_windows = 0
    with args.output.open("w", encoding="utf-8") as handle:
        for output_window, source_window in enumerate(
            sorted(windows, key=window_order)
        ):
            partitions = windows[source_window]
            if not isinstance(partitions, dict):
                raise TypeError(
                    f"window {source_window} is not partition-indexed"
                )
            counts: Counter[str] = Counter()
            for records in partitions.values():
                add_partition(counts, records)
            row = {
                "window": output_window,
                "counts": dict(sorted(counts.items())),
            }
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
            total_windows += 1

    print(f"Recovered {total_windows} global windows: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
