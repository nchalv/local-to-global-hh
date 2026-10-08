# Fixed-Placement Temporal-Controller Ablation

## Question

This experiment compares four policies for releasing Space-Saving capacity
after a deployment has accumulated sufficient evidence. All policies use the
same upward-sizing rule. They differ only in when they probe a lower capacity,
how they select it, and how failed-probe evidence constrains later reductions.
Guarded margin comfort is the policy selected for the evaluated system. The
other three policies provide controlled alternatives for this ablation.

## Controlled workload

The workload has 160 windows at `(m,n)=(20,200)`, with 120,000 items and
20,000 distinct keys in every window. Round-robin placement is fixed for the
entire run. Only the global frequency profile changes: a moderate warm-up is
followed by an abrupt hard phase, a five-step recovery, an easy plateau, a
short hard burst, an easy recovery, three hard/easy oscillations, and a final
moderate phase. The corresponding phases contain 8, 20, or 40 heavy hitters
and controlled populations of keys immediately below the threshold.

Keeping placement fixed is important. Capacity changes can be attributed to
temporal variation in summary difficulty rather than to a different
partitioning algorithm. The experiment requires neither MILP nor external
data and is generated deterministically from the bundled configuration.

## Contents

- `static_baselines.csv`: per-window Space-Saving results at `q=n`, `2n`, and
  `4n`.
- `controllers/*.csv`: per-window results for bracket probing, guarded bracket
  probing, pressure-gated comfort probing, and guarded margin comfort (the
  selected policy).
- `controllers/summary.csv`: aggregate controller metrics.
- `analysis/global_phase_summary.csv`: workload mass, cardinality, and
  threshold populations by phase.
- `analysis/phase_summary.csv`: quality, capacity, and service results by
  method and phase.
- `analysis/acceptance.txt`: machine-readable suitability checks in concise
  form; `acceptance.json` contains the same evidence structurally.
- `analysis/controller_composite_trajectories.{pdf,png}`: per-window
  threshold-normalized error and deployed capacity.

## Result

All four policies retain heavy-hitter and candidate recall 1.000 while moving
capacity over a `1.000n--4.535n` range. At fixed `q=n`, hard-phase
threshold-normalized error is 0.0268, compared with 0.0100 in the easy phase;
at `q=4n`, hard-phase error falls to zero. The workload therefore separates
intrinsic sketch difficulty before any controller comparison.

The selected guarded margin-comfort policy uses `2.145n` counters on average
and obtains the lowest aggregate threshold-normalized error (0.00328) and
highest F1 (0.987) among the adaptive policies, with a 4.4%
service-violation rate. Bracket probing uses the least capacity (`1.386n`) but
accepts more error and service violations. The result supports guarded margin
comfort as a balanced policy, not as an optimizer that dominates every
resource metric.

## Reproduce

From the artifact root:

```bash
./artifact temporal-controller
```

The command generates a fresh timestamped run under `results/runs/`, records
its environment and configuration, executes the static and adaptive methods,
and fails if any acceptance check does not hold.
