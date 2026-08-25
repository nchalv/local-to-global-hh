#!/usr/bin/env python3
"""Summarize repeated round-robin m-scaling systems experiments."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


SEMANTIC_COLUMNS = (
    "N_global",
    "threshold",
    "hh_precision",
    "hh_recall",
    "hh_f1",
    "aae",
    "are",
    "q_current",
    "q_next",
    "q_head_current",
    "q_tail_current",
    "q_head_next",
    "q_tail_next",
    "margin_alpha",
    "service_violation",
    "candidate_count",
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    rank = (len(ordered) - 1) * fraction
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return ordered[lower]
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def mean(rows: list[dict[str, str]], field: str) -> float:
    values = [float(row[field]) for row in rows if row.get(field, "") != ""]
    return statistics.fmean(values) if values else math.nan


def median(values: list[float]) -> float:
    return statistics.median(values) if values else math.nan


def classify(row: dict[str, str]) -> str | None:
    method = row["method"]
    if row["method_type"] == "hl":
        return "hl_bucketwise"
    if row["method_type"] != "hybrid":
        return None
    if "head-update=full" in method:
        return "hybrid_serial_full"
    if "reducer=parallel-streaming" in method:
        return "hybrid_parallel_delta"
    if "reducer=streaming" in method:
        return "hybrid_serial_delta"
    return None


def close(left: str, right: str) -> bool:
    if left == right:
        return True
    if left == "" or right == "":
        return False
    try:
        return math.isclose(
            float(left), float(right), rel_tol=1e-10, abs_tol=1e-10
        )
    except ValueError:
        return False


def verify_hybrid_semantics(
    grouped: dict[str, list[dict[str, str]]],
    source: Path,
    names: tuple[str, ...] = (
        "hybrid_serial_delta",
        "hybrid_parallel_delta",
        "hybrid_serial_full",
    ),
) -> None:
    indexed = {
        name: {int(row["window"]): row for row in grouped[name]}
        for name in names
    }
    windows = set(indexed[names[0]])
    if any(set(indexed[name]) != windows for name in names[1:]):
        raise RuntimeError(f"Hybrid window mismatch in {source}")
    for window in sorted(windows):
        reference = indexed[names[0]][window]
        for name in names[1:]:
            candidate = indexed[name][window]
            for field in SEMANTIC_COLUMNS:
                if not close(reference.get(field, ""), candidate.get(field, "")):
                    raise RuntimeError(
                        f"Hybrid mismatch in {source}, window={window}, "
                        f"field={field}: {reference.get(field)} != "
                        f"{candidate.get(field)}"
                    )


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--reducer-out", type=Path, required=True)
    parser.add_argument("--delta-out", type=Path, required=True)
    parser.add_argument("--delta-only", action="store_true")
    parser.add_argument("--runtime-only", action="store_true")
    args = parser.parse_args()
    if args.delta_only and args.runtime_only:
        parser.error("--delta-only and --runtime-only are mutually exclusive")

    repeat_metrics: dict[tuple[int, str], list[dict[str, float]]] = defaultdict(list)
    delta_metrics: dict[tuple[str, int], list[dict[str, float]]] = defaultdict(list)
    paths = sorted(args.root.glob("m*/csv/repeat_*.csv"))
    if not paths:
        raise RuntimeError(f"no repeated systems CSVs found under {args.root}")

    if args.runtime_only:
        trial_paths: dict[tuple[int, str], list[Path]] = defaultdict(list)
        for path in paths:
            parts = path.stem.split("_")
            if len(parts) != 3 or parts[0] != "repeat":
                raise RuntimeError(
                    f"runtime CSV does not identify one fresh-process method: {path}"
                )
            m = int(path.parent.parent.name.removeprefix("m"))
            trial_paths[(m, "_".join(parts[:2]))].append(path)
        trials = [
            (m, label, sorted(members))
            for (m, label), members in sorted(trial_paths.items())
        ]
    else:
        trials = [
            (int(path.parent.parent.name.removeprefix("m")), path.stem, [path])
            for path in paths
        ]

    for m, trial_label, members in trials:
        grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
        for path in members:
            with path.open(newline="", encoding="utf-8") as handle:
                for row in csv.DictReader(handle):
                    if int(row["window"]) == 0:
                        continue
                    name = classify(row)
                    if name is not None:
                        grouped[name].append(row)
        source = members[0].parent / trial_label
        if args.delta_only:
            required = {"hybrid_serial_delta", "hybrid_serial_full"}
        elif args.runtime_only:
            required = {
                "hl_bucketwise",
                "hybrid_serial_delta",
                "hybrid_parallel_delta",
            }
        else:
            required = {
                "hl_bucketwise",
                "hybrid_serial_delta",
                "hybrid_parallel_delta",
                "hybrid_serial_full",
            }
        if set(grouped) != required:
            raise RuntimeError(
                f"incomplete methods in {source}: {sorted(grouped)}"
            )
        if args.delta_only:
            hybrid_names = (
                "hybrid_serial_delta",
                "hybrid_serial_full",
            )
        elif args.runtime_only:
            hybrid_names = (
                "hybrid_serial_delta",
                "hybrid_parallel_delta",
            )
        else:
            hybrid_names = (
                "hybrid_serial_delta",
                "hybrid_parallel_delta",
                "hybrid_serial_full",
            )
        verify_hybrid_semantics(grouped, source, hybrid_names)

        for name in (() if args.delta_only else sorted(required)):
            rows = grouped[name]
            reduce_values = [float(row["reduce_ms"]) for row in rows]
            # Input files aggregate repeated keys into weighted updates. The
            # sketch update path applies the complete weight, so throughput is
            # measured in logical stream insertions rather than JSON records.
            update_events = sum(float(row["N_global"]) for row in rows)
            update_ms = sum(float(row["update_ms"]) for row in rows)
            repeat_metrics[(m, name)].append(
                {
                    "windows": float(len(rows)),
                    "reduce_mean_ms": statistics.fmean(reduce_values),
                    "reduce_p50_ms": percentile(reduce_values, 0.50),
                    "reduce_p95_ms": percentile(reduce_values, 0.95),
                    "update_mops": update_events / (1000.0 * update_ms),
                    "report_prepare_mean_ms": mean(rows, "report_prepare_ms"),
                    "aggregation_mean_ms": mean(rows, "aggregation_ms"),
                    "control_mean_ms": mean(rows, "control_ms"),
                    "coord_peak_mean_kib": mean(rows, "mem_coord_peak_kib"),
                    "upstream_mean_kib": mean(rows, "report_volume_kib"),
                    "control_mean_kib": mean(rows, "control_volume_kib"),
                    "head_apply_mean_ms": mean(rows, "head_update_apply_ms"),
                }
            )

        if not args.runtime_only:
            delta = grouped["hybrid_serial_delta"]
            full = grouped["hybrid_serial_full"]
            delta_head_update = mean(delta, "head_update_volume_kib")
            full_head_update = mean(full, "head_update_volume_kib")
            delta_apply = mean(delta, "head_update_apply_ms")
            full_apply = mean(full, "head_update_apply_ms")
            delta_metrics[("CAIDA", m)].append(
                {
                    "windows": float(len(delta)),
                    "delta_selection_rate": mean(delta, "head_delta_selected"),
                    "delta_head_update_kib": delta_head_update,
                    "full_head_update_kib": full_head_update,
                    "head_update_saving_fraction": (
                        1.0 - delta_head_update / full_head_update
                    ),
                    "delta_apply_ms": delta_apply,
                    "full_apply_ms": full_apply,
                    "apply_speedup": full_apply / delta_apply,
                }
            )

    reducer_rows: list[dict[str, object]] = []
    for (m, method), repeats in sorted(repeat_metrics.items()):
        reducer_rows.append(
            {
                "m": m,
                "method": method,
                "repetitions": len(repeats),
                "windows_per_repetition": int(repeats[0]["windows"]),
                "reduce_mean_ms": median(
                    [row["reduce_mean_ms"] for row in repeats]
                ),
                "reduce_mean_iqr_ms": percentile(
                    [row["reduce_mean_ms"] for row in repeats], 0.75
                )
                - percentile([row["reduce_mean_ms"] for row in repeats], 0.25),
                "reduce_p50_ms": median(
                    [row["reduce_p50_ms"] for row in repeats]
                ),
                "reduce_p95_ms": median(
                    [row["reduce_p95_ms"] for row in repeats]
                ),
                "update_mops": median(
                    [row["update_mops"] for row in repeats]
                ),
                "report_prepare_mean_ms": median(
                    [row["report_prepare_mean_ms"] for row in repeats]
                ),
                "aggregation_mean_ms": median(
                    [row["aggregation_mean_ms"] for row in repeats]
                ),
                "control_mean_ms": median(
                    [row["control_mean_ms"] for row in repeats]
                ),
                "coord_peak_mean_kib": median(
                    [row["coord_peak_mean_kib"] for row in repeats]
                ),
                "upstream_mean_kib": median(
                    [row["upstream_mean_kib"] for row in repeats]
                ),
                "control_mean_kib": median(
                    [row["control_mean_kib"] for row in repeats]
                ),
            }
        )

    synthetic_paths = [] if args.runtime_only else sorted(
        args.root.glob("synthetic_delta/csv/repeat_*.csv")
    )
    for path in synthetic_paths:
        grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
        with path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                if int(row["window"]) == 0:
                    continue
                name = classify(row)
                if name in ("hybrid_serial_delta", "hybrid_serial_full"):
                    grouped[name].append(row)
        required = ("hybrid_serial_delta", "hybrid_serial_full")
        if set(grouped) != set(required):
            raise RuntimeError(
                f"incomplete synthetic delta methods in {path}: "
                f"{sorted(grouped)}"
            )
        verify_hybrid_semantics(grouped, path, required)
        delta = grouped["hybrid_serial_delta"]
        full = grouped["hybrid_serial_full"]
        delta_head_update = mean(delta, "head_update_volume_kib")
        full_head_update = mean(full, "head_update_volume_kib")
        delta_apply = mean(delta, "head_update_apply_ms")
        full_apply = mean(full, "head_update_apply_ms")
        delta_metrics[("Synthetic", 100)].append(
            {
                "windows": float(len(delta)),
                "delta_selection_rate": mean(delta, "head_delta_selected"),
                "delta_head_update_kib": delta_head_update,
                "full_head_update_kib": full_head_update,
                "head_update_saving_fraction": (
                    1.0 - delta_head_update / full_head_update
                ),
                "delta_apply_ms": delta_apply,
                "full_apply_ms": full_apply,
                "apply_speedup": full_apply / delta_apply,
            }
        )

    delta_rows: list[dict[str, object]] = []
    for (workload, m), repeats in sorted(delta_metrics.items()):
        delta_rows.append(
            {
                "workload": workload,
                "m": m,
                "repetitions": len(repeats),
                "windows_per_repetition": int(repeats[0]["windows"]),
                **{
                    field: median([row[field] for row in repeats])
                    for field in (
                        "delta_selection_rate",
                        "delta_head_update_kib",
                        "full_head_update_kib",
                        "head_update_saving_fraction",
                        "delta_apply_ms",
                        "full_apply_ms",
                        "apply_speedup",
                    )
                },
                "quality_and_trajectory_identical": "yes",
            }
        )

    if reducer_rows:
        write_csv(args.reducer_out, reducer_rows)
    if delta_rows:
        write_csv(args.delta_out, delta_rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
