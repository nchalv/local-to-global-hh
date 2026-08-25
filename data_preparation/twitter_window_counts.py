#!/usr/bin/env python3
"""Aggregate chronologically sorted Twitter hashtag events into time windows.

Input rows are produced by ``twitter_hashtag_extractor.py``:

    {"timestamp":"2013-04-01T00:00:00","hashtag":"example"}

The output follows the repository's common real-counts interchange format:

    {"window":0,"counts":{"example":42}}

Inputs must be chronologically ordered, including at file boundaries.  The
converter emits a window as soon as its end is crossed, so only one window's
hashtag map is resident at a time.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from contextlib import nullcontext
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path


@dataclass
class WindowingStats:
    input_events: int = 0
    emitted_windows: int = 0
    nonempty_windows: int = 0
    hashtag_occurrences: int = 0
    first_timestamp: str | None = None
    last_timestamp: str | None = None
    origin_timestamp: str | None = None


def parse_timestamp(value: str) -> datetime:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        return parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def format_timestamp(value: datetime) -> str:
    return value.astimezone(timezone.utc).replace(tzinfo=None).isoformat(timespec="seconds")


def write_window(destination, output_dir: Path | None, window_id: int,
                 counts: Counter[str], stats: WindowingStats) -> None:
    record = {"window": window_id, "counts": dict(counts)}
    if output_dir is not None:
        path = output_dir / f"window_{window_id:06d}.json"
        path.write_text(json.dumps(record, ensure_ascii=False) + "\n", encoding="utf-8")
    else:
        assert destination is not None
        destination.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
    stats.emitted_windows += 1
    if counts:
        stats.nonempty_windows += 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Aggregate sorted timestamped Twitter hashtag events into tumbling windows."
    )
    parser.add_argument(
        "inputs", nargs="+", type=Path,
        help="Chronologically sorted hashtag-event JSONL files",
    )
    parser.add_argument("--output", required=True, type=Path,
                        help="Per-window counts JSONL output, or output directory with --save-windows-separately")
    parser.add_argument("--save-windows-separately", action="store_true",
                        help="Write one window_XXXXXX.json file per window instead of one JSONL file")
    parser.add_argument(
        "--window-hours", type=float, default=24.0,
        help="Tumbling window duration in hours (default: 24)",
    )
    parser.add_argument(
        "--origin", default=None,
        help="UTC ISO-8601 window-0 start; default is midnight of the first event's day",
    )
    parser.add_argument(
        "--max-events", type=int, default=None,
        help="Optional event limit for a quick validation run",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.window_hours <= 0:
        raise ValueError("--window-hours must be positive")
    if args.max_events is not None and args.max_events <= 0:
        raise ValueError("--max-events must be positive")
    for path in args.inputs:
        if not path.is_file():
            raise FileNotFoundError(f"input file does not exist: {path}")

    window_duration = timedelta(hours=args.window_hours)
    origin = parse_timestamp(args.origin) if args.origin else None
    previous_timestamp: datetime | None = None
    current_window: int | None = None
    counts: Counter[str] = Counter()
    stats = WindowingStats()
    if origin is not None:
        stats.origin_timestamp = format_timestamp(origin)
    output_dir = args.output if args.save_windows_separately else None
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)

    destination_context = (
        nullcontext(None)
        if output_dir is not None
        else args.output.open("w", encoding="utf-8")
    )
    with destination_context as destination:
        for path in args.inputs:
            with path.open(encoding="utf-8") as source:
                for line_number, line in enumerate(source, start=1):
                    event = json.loads(line)
                    timestamp = parse_timestamp(event["timestamp"])
                    hashtag = event["hashtag"]
                    if not isinstance(hashtag, str) or not hashtag:
                        raise ValueError(f"invalid hashtag in {path}:{line_number}")
                    if previous_timestamp is not None and timestamp < previous_timestamp:
                        raise ValueError(
                            f"input is not timestamp-sorted at {path}:{line_number}: "
                            f"{format_timestamp(timestamp)} < {format_timestamp(previous_timestamp)}"
                        )
                    if origin is None:
                        origin = timestamp.replace(hour=0, minute=0, second=0, microsecond=0)
                        stats.origin_timestamp = format_timestamp(origin)
                    if timestamp < origin:
                        raise ValueError(f"event precedes the configured origin at {path}:{line_number}")

                    window_id = int((timestamp - origin) // window_duration)
                    if current_window is None:
                        current_window = window_id
                    while current_window < window_id:
                        write_window(destination, output_dir, current_window, counts, stats)
                        counts.clear()
                        current_window += 1
                    counts[hashtag] += 1
                    stats.input_events += 1
                    stats.hashtag_occurrences += 1
                    if stats.first_timestamp is None:
                        stats.first_timestamp = format_timestamp(timestamp)
                    stats.last_timestamp = format_timestamp(timestamp)
                    previous_timestamp = timestamp

                    if args.max_events and stats.input_events >= args.max_events:
                        break
                if args.max_events and stats.input_events >= args.max_events:
                    break

        if current_window is not None:
            write_window(destination, output_dir, current_window, counts, stats)

    metadata_path = (
        output_dir / "metadata.json"
        if output_dir is not None
        else args.output.with_name(f"{args.output.stem}_metadata.json")
    )
    metadata = {
        "sources": [str(path) for path in args.inputs],
        "format": "per-window Twitter hashtag counts",
        "record_schema": {"window": "zero-based integer", "counts": "hashtag -> occurrence count"},
        "window_hours": args.window_hours,
        "origin_timestamp": stats.origin_timestamp,
        "stats": asdict(stats),
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    print(f"Emitted {stats.emitted_windows:,} windows "
          f"({stats.nonempty_windows:,} non-empty) from {stats.input_events:,} hashtag events.")
    print(f"Window files: {output_dir}" if output_dir is not None else f"JSONL: {args.output}")
    print(f"Metadata: {metadata_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
