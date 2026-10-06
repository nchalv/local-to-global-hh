# Partition-local Structure

## Question

Why does round-robin placement require more Adaptive Space-Saving capacity
than visibility suppression even though round robin balances record volume?

## Evidence

- `partition_hardness_windows.csv`: per-window measurements for matched CAIDA-B
  global windows.
- `partition_hardness_summary.csv`: aggregates by placement and threshold.
- `partition_hardness.pdf` and `.png`: compact comparison of local cardinality,
  support replication, and residual mass.

The global windows are identical across placements. Round robin spreads a
broader tail across input partitions, so more mass remains beyond each local
top-q set. Visibility suppression is deliberately hostile to HeavyLocker's
local admission assumption, but its residual support is more concentrated once
the Space-Saving capacity covers the local competition band. The diagnostic
therefore explains partition-local sketch difficulty rather than record-load
imbalance.

## Regeneration

```bash
python3 experiments/end_to_end/plot_partition_hardness.py \
  --summary results/reference/supplementary/partition-hardness/partition_hardness_summary.csv \
  --out results/reference/supplementary/partition-hardness
```
