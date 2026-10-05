# Supplementary Results

The files under `results/reference/supplementary/` are ready-to-inspect
aggregate outputs. They are not additional claims from the paper. They expose
supporting diagnostics and systems measurements that were omitted for space.
No CAIDA packet data, raw keys, or partitioned streams are distributed.

The reference CSVs were produced by research-workspace commit
`f40543fa5cd3d2e8b0a7945f124d95c8bd10ac27`. Each plot can be regenerated from
its committed aggregate CSV without rerunning the benchmark.

## Margin sensitivity under visibility suppression

Directory: `results/reference/supplementary/margin-visibility/`

This extends the paper's round-robin margin study to the placement that
suppresses the local evidence used by HeavyLocker. Across (n=200,400,600,800),
the selected Hybrid setting retains a favorable quality--memory position. The
complete curve also shows why the selected margins should be interpreted as
operating points rather than universally optimal constants.

Regenerate the ready plot:

```bash
python3 experiments/end_to_end/plot_supplementary_margin.py \
  --summary results/reference/supplementary/margin-visibility/summary.csv \
  --out results/reference/supplementary/margin-visibility
```

## Partition-local structure

Directory: `results/reference/supplementary/partition-hardness/`

These matched-window diagnostics explain why round robin can be harder for a
local Space-Saving summary even though it balances partition load. Round robin
replicates a broader local tail, leaving substantially more mass beyond each
partition's local top-(q). Visibility suppression concentrates the residual
support and becomes easier once capacity covers its local competition band.

Regenerate the plot:

```bash
python3 experiments/end_to_end/plot_partition_hardness.py \
  --summary results/reference/supplementary/partition-hardness/partition_hardness_summary.csv \
  --out results/reference/supplementary/partition-hardness
```

`partition_hardness_windows.csv` contains the corresponding per-window
measurements, allowing the aggregate statistics to be recomputed.

## Coordinator reduction and exact-head updates

Directory: `results/reference/supplementary/systems/`

The reducer summary reports five repetitions at (m=100,500,1000). Parallel
Hybrid reduction is approximately twice as fast as serial Hybrid reduction at
all three points, while HeavyLocker's specialized bucket-wise reduction remains
faster. The delta summary shows that low head churn activates deltas in 92.5%
and 96.5% of windows at (m=500) and (m=1000), reducing head-update traffic by
22.5% and 28.9%. At (m=100), the policy remains inactive and sends snapshots.
All compared delta and snapshot runs retain identical quality and controller
trajectories.

Regenerate both plots:

```bash
python3 experiments/end_to_end/plot_caida_m_systems_optimizations.py \
  --reducer-summary results/reference/supplementary/systems/reducer_runtime_summary.csv \
  --delta-summary results/reference/supplementary/systems/head_delta_summary.csv \
  --out results/reference/supplementary/systems
```

## Pending controller ablation

The former controller-policy ablation is not included in the reference bundle.
Its retained output predates the final evaluator and accounting configuration.
It will be added only after rerunning the round-robin experiment through the
artifact workflow. Ambiguity adjustment is not pending: it was removed from the
submitted method and is intentionally excluded.
