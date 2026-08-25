#!/usr/bin/env python3
"""Extract a contiguous window range from normalized JSONL count data."""

import argparse
import json
import os
from datetime import datetime, timezone
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Slice and optionally rebase normalized per-window JSONL counts."
    )
    parser.add_argument("input", type=Path, help="Source JSONL count file")
    parser.add_argument("--output", required=True, type=Path, help="Output JSONL file")
    parser.add_argument("--start-window", required=True, type=int)
    parser.add_argument("--num-windows", required=True, type=int)
    parser.add_argument(
        "--source-metadata",
        type=Path,
        help="Source analyzer metadata; used to derive the UTC interval",
    )
    parser.add_argument(
        "--metadata-output",
        type=Path,
        help="Output metadata path (default: <output stem>_metadata.json)",
    )
    parser.add_argument(
        "--keep-window-ids",
        action="store_true",
        help="Keep original window IDs instead of rebasing them to zero",
    )
    return parser.parse_args()


def source_origin(metadata: dict) -> float | None:
    timestamps = [
        float(row["first_timestamp"])
        for row in metadata.get("files", [])
        if row.get("first_timestamp") is not None
    ]
    if timestamps:
        return min(timestamps)
    timestamp = metadata.get("dataset_start_time", {}).get("timestamp")
    return float(timestamp) if timestamp is not None else None


def utc_iso(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).isoformat()


def main() -> int:
    args = parse_args()
    if args.start_window < 0:
        raise ValueError("--start-window must be non-negative")
    if args.num_windows <= 0:
        raise ValueError("--num-windows must be positive")

    end_window = args.start_window + args.num_windows
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary_output = args.output.with_name(f".{args.output.name}.tmp")
    total_events = 0
    selected_count = 0
    try:
        with (
            args.input.open(encoding="utf-8") as source,
            temporary_output.open("w", encoding="utf-8") as target,
        ):
            for line_no, line in enumerate(source, start=1):
                if not line.strip():
                    continue
                row = json.loads(line)
                source_window = int(row.get("window", line_no - 1))
                if source_window < args.start_window:
                    continue
                if source_window >= end_window:
                    break

                expected_window = args.start_window + selected_count
                if source_window != expected_window:
                    raise ValueError(
                        f"Expected source window {expected_window}, "
                        f"found {source_window}"
                    )

                counts = {str(key): int(value) for key, value in row["counts"].items()}
                total_events += sum(counts.values())
                output_row = dict(row)
                output_row["window"] = (
                    source_window if args.keep_window_ids else selected_count
                )
                if not args.keep_window_ids:
                    output_row["source_window"] = source_window
                output_row["counts"] = counts
                target.write(json.dumps(output_row, separators=(",", ":")) + "\n")
                selected_count += 1

        if selected_count != args.num_windows:
            raise ValueError(
                f"Expected {args.num_windows} windows beginning at "
                f"{args.start_window}; found {selected_count}"
            )
        os.replace(temporary_output, args.output)
    except BaseException:
        temporary_output.unlink(missing_ok=True)
        raise

    metadata_path = args.metadata_output or args.output.with_name(
        f"{args.output.stem}_metadata.json"
    )
    source_metadata = {}
    if args.source_metadata:
        with args.source_metadata.open(encoding="utf-8") as handle:
            source_metadata = json.load(handle)

    metadata = {
        "source_counts": str(args.input),
        "source_metadata": str(args.source_metadata) if args.source_metadata else None,
        "source_window_range": [args.start_window, end_window - 1],
        "output_window_range": (
            [args.start_window, end_window - 1]
            if args.keep_window_ids
            else [0, args.num_windows - 1]
        ),
        "total_windows": args.num_windows,
        "total_events": total_events,
        "window_ids_rebased": not args.keep_window_ids,
    }
    window_seconds = source_metadata.get("window_seconds")
    origin = source_origin(source_metadata)
    if window_seconds is not None:
        metadata["window_seconds"] = float(window_seconds)
    if origin is not None and window_seconds is not None:
        start_timestamp = origin + args.start_window * float(window_seconds)
        end_timestamp = origin + end_window * float(window_seconds)
        metadata["utc_interval"] = {
            "start_inclusive": utc_iso(start_timestamp),
            "end_exclusive": utc_iso(end_timestamp),
            "start_timestamp": start_timestamp,
            "end_timestamp": end_timestamp,
        }

    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    with metadata_path.open("w", encoding="utf-8") as handle:
        json.dump(metadata, handle, indent=2)
        handle.write("\n")

    print(
        f"Extracted {args.num_windows} windows "
        f"({args.start_window}..{end_window - 1}) to {args.output}"
    )
    print(f"Metadata: {metadata_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
