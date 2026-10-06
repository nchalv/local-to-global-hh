# Coordinator Reduction and Exact-head Updates

## Questions

1. How does Hybrid's parallel coordinator reducer compare with its serial
   reducer and HeavyLocker's specialized reducer as the partition count grows?
2. When does sending exact-head deltas reduce coordinator-to-partition traffic
   relative to complete snapshots?

## Evidence

- `reducer_runtime_summary.csv`: five repetitions at m=100, 500, and 1000.
- `head_delta_summary.csv`: delta activation, update traffic, and equality of
  quality and controller trajectories.
- `m_reducer_runtime.*` and `head_delta_effect.*`: ready plots.

Parallel reduction roughly halves Hybrid's coordinator reduction time relative
to serial reduction, while HeavyLocker's specialized reduction remains faster.
Head deltas activate when consecutive promoted heads overlap sufficiently. In
the retained CAIDA-B runs, they reduce downstream head-update traffic at
m=500 and m=1000. At m=100 the policy deliberately sends snapshots, so no
delta reduction is reported.

## Regeneration

```bash
python3 experiments/end_to_end/plot_caida_m_systems_optimizations.py \
  --reducer-summary results/reference/supplementary/systems/reducer_runtime_summary.csv \
  --delta-summary results/reference/supplementary/systems/head_delta_summary.csv \
  --out results/reference/supplementary/systems
```
