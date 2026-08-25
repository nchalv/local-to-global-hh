#!/usr/bin/env python3
"""Concatenate normalized global-count traces with explicit provenance."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Combine label=path global-count JSONL traces."
    )
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        metavar="LABEL=PATH",
        help="Labeled input trace; may be supplied more than once.",
    )
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def parse_input(value: str) -> tuple[str, Path]:
    label, separator, path = value.partition("=")
    if not separator or not label or not path:
        raise ValueError(f"expected LABEL=PATH, got: {value}")
    return label, Path(path)


def main() -> int:
    args = parse_args()
    sources = [parse_input(value) for value in args.input]
    minimum_mass: int | None = None
    maximum_mass: int | None = None
    output_window = 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as output:
        for label, path in sources:
            if not path.is_file():
                raise FileNotFoundError(path)
            source_windows = 0
            with path.open(encoding="utf-8") as source:
                for line_number, line in enumerate(source, start=1):
                    line = line.strip()
                    if not line:
                        continue
                    row = json.loads(line)
                    counts = {
                        str(key): int(value)
                        for key, value in row["counts"].items()
                        if int(value) > 0
                    }
                    mass = sum(counts.values())
                    if mass <= 0:
                        raise ValueError(f"{path}:{line_number} has no positive mass")
                    minimum_mass = mass if minimum_mass is None else min(minimum_mass, mass)
                    maximum_mass = mass if maximum_mass is None else max(maximum_mass, mass)
                    combined = {
                        "window": output_window,
                        "source_profile": label,
                        "source_window": row.get("window", source_windows),
                        "counts": counts,
                    }
                    output.write(json.dumps(combined, separators=(",", ":")) + "\n")
                    output_window += 1
                    source_windows += 1
            if source_windows == 0:
                raise ValueError(f"empty global-count trace: {path}")
            print(f"{label}: {source_windows} windows from {path}")

    print(
        f"Combined {output_window} windows with mass range "
        f"[{minimum_mass}, {maximum_mass}]: {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
