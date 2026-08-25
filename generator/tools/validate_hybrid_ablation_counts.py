#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description="Validate the shared multi-resolution Hybrid-ablation trace."
    )
    parser.add_argument("counts", type=Path)
    parser.add_argument("--total", type=int, default=60000)
    parser.add_argument("--n", type=int, nargs="+", default=[100, 200, 400])
    parser.add_argument("--minimum-active", type=int, default=1600)
    parser.add_argument("--partitions", type=int, default=100)
    parser.add_argument("--minimum-mean-local-distinct", type=float, default=450.0)
    parser.add_argument("--halo-low", type=float, default=0.85)
    return parser.parse_args()


def main():
    args = parse_args()
    rows = []
    with args.counts.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            row = json.loads(line)
            counts = [int(value) for value in row["counts"].values() if int(value) > 0]
            if sum(counts) != args.total:
                raise ValueError(
                    f"line {line_number}: total={sum(counts)}, expected {args.total}"
                )
            if len(counts) < args.minimum_active:
                raise ValueError(
                    f"line {line_number}: active={len(counts)}, "
                    f"expected at least {args.minimum_active}"
                )
            rows.append(counts)

    if not rows:
        raise ValueError("global-count trace contains no windows")

    print(f"windows={len(rows)} total={args.total}")
    print(
        "active_keys="
        f"{min(map(len, rows))}..{max(map(len, rows))} "
        f"(mean={sum(map(len, rows)) / len(rows):.1f})"
    )
    mean_local_distinct = [
        sum(min(value, args.partitions) for value in row) / float(args.partitions)
        for row in rows
    ]
    if min(mean_local_distinct) < args.minimum_mean_local_distinct:
        raise ValueError(
            "round-robin local-support proxy is too small: "
            f"minimum={min(mean_local_distinct):.1f}, "
            f"expected at least {args.minimum_mean_local_distinct:.1f}"
        )
    print(
        "round_robin_mean_local_distinct_proxy="
        f"{min(mean_local_distinct):.1f}..{max(mean_local_distinct):.1f}"
    )
    for n_value in args.n:
        threshold = args.total / float(n_value)
        hh_counts = [sum(value > threshold for value in row) for row in rows]
        halo_counts = [
            sum(args.halo_low * threshold <= value <= threshold for value in row)
            for row in rows
        ]
        above_windows = sum(any(value > threshold for value in row) for row in rows)
        below_windows = sum(
            any(args.halo_low * threshold <= value <= threshold for value in row)
            for row in rows
        )
        if above_windows != len(rows) or below_windows != len(rows):
            raise ValueError(
                f"n={n_value}: HH/halo coverage is incomplete "
                f"({above_windows}/{below_windows} of {len(rows)} windows)"
            )
        print(
            f"n={n_value} threshold={threshold:.1f} "
            f"HH={min(hh_counts)}..{max(hh_counts)} "
            f"near_nonHH={min(halo_counts)}..{max(halo_counts)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
