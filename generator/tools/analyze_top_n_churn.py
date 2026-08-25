#!/usr/bin/env python3
"""Measure consecutive Top_n identity turnover in global-count JSONL."""

from __future__ import annotations

import argparse
import heapq
import json
import statistics
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("counts", type=Path)
    parser.add_argument("--n", type=int, required=True)
    return parser.parse_args()


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1, int(fraction * (len(ordered) - 1))))]


def main() -> int:
    args = parse_args()
    if args.n <= 0:
        raise ValueError("--n must be positive")

    heads: list[set[str]] = []
    with args.counts.open(encoding="utf-8") as handle:
        for line in handle:
            counts = json.loads(line)["counts"]
            top = heapq.nlargest(
                min(args.n, len(counts)),
                counts.items(),
                key=lambda item: (int(item[1]), str(item[0])),
            )
            heads.append({str(key) for key, _ in top})

    if len(heads) < 2:
        raise ValueError("at least two windows are required")

    replacement: list[float] = []
    jaccard: list[float] = []
    for previous, current in zip(heads, heads[1:]):
        replacement.append(len(current - previous) / float(len(current)))
        jaccard.append(1.0 - len(previous & current) / float(len(previous | current)))

    print(f"windows={len(heads)} n={args.n}")
    print(
        "replacement_pct "
        f"mean={100.0 * statistics.mean(replacement):.3f} "
        f"median={100.0 * statistics.median(replacement):.3f} "
        f"p95={100.0 * percentile(replacement, 0.95):.3f}"
    )
    print(
        "jaccard_churn_pct "
        f"mean={100.0 * statistics.mean(jaccard):.3f} "
        f"median={100.0 * statistics.median(jaccard):.3f} "
        f"p95={100.0 * percentile(jaccard, 0.95):.3f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
