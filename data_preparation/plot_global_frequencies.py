#!/usr/bin/env python3
"""Plot global per-window key frequencies from prepared JSONL counts."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.ticker import FuncFormatter


def parse_window_selector(value: str | None) -> set[int] | None:
    if value is None or value.strip().lower() in {"", "all"}:
        return None
    selected: set[int] = set()
    for chunk in value.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "-" in chunk:
            lo_s, hi_s = chunk.split("-", 1)
            lo, hi = int(lo_s), int(hi_s)
            if hi < lo:
                raise ValueError(f"invalid window range: {chunk}")
            selected.update(range(lo, hi + 1))
        else:
            selected.add(int(chunk))
    return selected


def iter_count_windows(path: Path) -> Iterable[tuple[int, dict[str, int]]]:
    with path.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            if "counts" not in row:
                raise ValueError(f"missing 'counts' in {path}:{line_no}")
            window = int(row.get("window", line_no - 1))
            counts = {str(k): int(v) for k, v in row["counts"].items() if int(v) > 0}
            yield window, counts


def plot_window(
    *,
    window: int,
    counts: dict[str, int],
    n: int,
    out_dir: Path,
    fmt: str,
    top_keys: int,
    title_prefix: str,
) -> None:
    total = sum(counts.values())
    if total <= 0:
        return

    threshold = total / float(n)
    items = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    total_keys = len(items)
    visible = items[:top_keys] if top_keys > 0 else items
    visible_mass = sum(v for _, v in visible)
    keys = [k for k, _ in visible]
    values = [v for _, v in visible]
    colors = ["#d62728" if v > threshold else "#7f8c8d" for v in values]

    fig, ax = plt.subplots(figsize=(14, 7))
    ax.bar(range(len(values)), values, color=colors, width=0.65, edgecolor=colors, linewidth=0.4)
    ax.axhline(threshold, color="#d62728", linestyle="--", linewidth=1.2, alpha=0.8)
    ax.text(
        0.99,
        threshold,
        f"  1/{n} threshold = {threshold:,.0f}",
        color="#d62728",
        ha="right",
        va="bottom",
        transform=ax.get_yaxis_transform(),
    )

    ax.set_title(f"{title_prefix} window {window}: global key frequencies", fontsize=13, fontweight="bold")
    ax.set_ylabel("Frequency (count)")
    ax.set_xlabel(f"Keys sorted by frequency (top {len(visible)} of {total_keys})")
    ax.set_xticks(range(len(keys)))
    ax.set_xticklabels(keys, rotation=90, fontsize=7)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{int(y):,}"))
    ax.yaxis.grid(True, linestyle="--", linewidth=0.5, alpha=0.55)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    hh_count = sum(1 for _, v in items if v > threshold)
    ax.text(
        0.01,
        0.98,
        f"N={total:,}\nkeys={total_keys:,}\nHHs={hh_count:,}\nvisible mass={visible_mass / total:.1%}",
        transform=ax.transAxes,
        ha="left",
        va="top",
        fontsize=9,
        bbox={"facecolor": "white", "edgecolor": "#bdc3c7", "alpha": 0.92},
    )
    ax.legend(
        handles=[
            Patch(color="#d62728", label=f"Heavy hitters (> 1/{n})"),
            Patch(color="#7f8c8d", label="Other keys"),
        ],
        loc="upper right",
        framealpha=0.95,
    )

    fig.tight_layout()
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"window_{window:06d}_global_freq.{fmt}"
    fig.savefig(out_path)
    plt.close(fig)
    print(f"saved {out_path}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot global per-window frequencies from prepared JSONL counts."
    )
    parser.add_argument("counts_jsonl", type=Path, help="Prepared counts JSONL file.")
    parser.add_argument("--n", type=int, default=200, help="HH denominator. Default: 200.")
    parser.add_argument(
        "--windows",
        default="0",
        help="Comma/range selector, e.g. '0,10,20-25', or 'all'. Default: 0.",
    )
    parser.add_argument("--out-dir", type=Path, default=Path("data_preparation/plots/global_frequencies"))
    parser.add_argument("--top-keys", type=int, default=100, help="Number of sorted keys to render per window.")
    parser.add_argument("--format", choices=("png", "pdf"), default="png")
    parser.add_argument("--title-prefix", default="CAIDA")
    args = parser.parse_args()

    if args.n <= 0:
        parser.error("--n must be positive")
    if args.top_keys < 0:
        parser.error("--top-keys must be non-negative")

    selected = parse_window_selector(args.windows)
    plotted = 0
    for window, counts in iter_count_windows(args.counts_jsonl):
        if selected is not None and window not in selected:
            continue
        plot_window(
            window=window,
            counts=counts,
            n=args.n,
            out_dir=args.out_dir,
            fmt=args.format,
            top_keys=args.top_keys,
            title_prefix=args.title_prefix,
        )
        plotted += 1
    if plotted == 0:
        raise SystemExit("no matching windows found")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
