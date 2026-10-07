# Supplementary Results

The files under `results/reference/supplementary/` are ready-to-inspect
supporting diagnostics and systems measurements. Start with
`results/reference/README.md`, which explains the role and evidence level of
each bundle. No CAIDA packet data, raw keys, or partitioned streams are
distributed.

The CAIDA-dependent reference CSVs were produced by research-workspace commit
`f40543fa5cd3d2e8b0a7945f124d95c8bd10ac27`. The temporal-controller bundle is
self-contained and reproduced by the artifact command below. Every plot can
be regenerated from its committed aggregate or per-window CSVs without
rerunning the corresponding benchmark.

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

## Per-window error and memory trajectories

Directory: `results/reference/supplementary/window-trajectories/`

The aggregate configuration curves hide how resource use changes from one
decision window to the next. At the representative CAIDA-B point
(m=100,n=600), four plots pair per-window ARE with resident per-partition
memory: one for each method and placement. HeavyLocker (w=0.32n) has a fixed
memory trace, while Hybrid adjusts its residual state as observed stream
difficulty changes. Separating both methods and placements makes each temporal
trajectory readable without overlapping series.

The round-robin traces favor HeavyLocker at this operating point: its mean
plotted ARE is 0.711% at 101.4 KiB, compared with Hybrid's 1.225% at 102.2 KiB.
Under visibility suppression, Hybrid instead records 0.768% mean plotted ARE
at 72.8 KiB, versus HeavyLocker's 1.694% at 98.3 KiB. These are arithmetic
means of the displayed per-window values rather than the paper's pooled error
aggregates. The primary takeaway is temporal:
HeavyLocker's allocation is nearly fixed, whereas Hybrid expands and contracts
with observed difficulty. A complete per-case explanation is provided in the
directory's `README.md`.

Regenerate all four plots from the committed per-window CSVs:

```bash
python3 experiments/end_to_end/plot_caida_stepwise.py \
  --root results/reference/supplementary/window-trajectories \
  --n 600 \
  --out results/reference/supplementary/window-trajectories/caida_stepwise.pdf
```

## Temporal-controller policy ablation

Directory: `results/reference/supplementary/temporal-controller/`

This experiment compares bracket probing, guarded bracket probing,
pressure-gated comfort probing, and the selected guarded margin-comfort policy
at `(m,n)=(20,200)`, `epsilon_M=0.15`, and `alpha=0.95`. A single 160-window
global workload contains an abrupt hardening, gradual recovery, easy plateau,
short hard burst, and block oscillation. Every window contains 120,000 items
over 20,000 keys. Round-robin placement remains fixed throughout, so changes
in controller behavior arise from the global distribution rather than a
changing partitioner. Ambiguity adjustment is disabled.

The workflow evaluates static Space-Saving at `q=n`, `2n`, and `4n` before
running all four controllers. It writes raw per-window CSVs, phase-level
summaries, threshold-normalized error and capacity trajectories, and an
acceptance report. The report verifies the workload dimensions, confirms that
the hard phase is more difficult than the easy phase at fixed capacity, checks
that additional capacity reduces hard-phase error, and requires every
controller to change capacity while retaining heavy-hitter recall.

Run the complete self-contained experiment with:

```bash
./artifact temporal-controller
```

Outputs are written beneath
`results/runs/temporal-controller-<timestamp>/`. The global and controller
phase summaries are under `analysis/`, together with the validation report and
ready PDF/PNG trajectory plots.
