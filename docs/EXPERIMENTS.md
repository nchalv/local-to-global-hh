# Evaluation Scope

The evaluation has three distinct levels. Quick validation checks that the
software builds and that all principal methods execute. Committed reference
results support inspection without redistributing CAIDA. Full reproduction
recreates the long-running CAIDA-dependent matrices.

## Recommended workflow

| Stage | Command | External data | Purpose |
|---|---|---:|---|
| Environment | `./artifact check` | No | Check required tools and Python packages. |
| Build and tests | `./artifact test` | No | Build the evaluator and run its unit tests. |
| Smoke test | `./artifact smoke` | No | Generate a small deterministic stream and validate all principal methods. |
| Reference evidence | `./artifact verify-results` | No | Verify the integrity of committed CSV and plot outputs. |
| Controller ablation | `./artifact temporal-controller` | No | Compare the selected guarded margin-comfort controller with three alternatives. |
| Stage CAIDA | `./artifact prepare-caida` | Yes | Validate and stage the two normalized CAIDA slices. |

The smoke test is the recommended first evaluation. It verifies the complete
generator--evaluator--summary path on a small bundled configuration, but is
not part of the reported experimental evidence.

## Primary evaluation

The full reproduction matrix covers:

1. CAIDA-A promoted-head ablation under round-robin placement.
2. CAIDA-A margin sensitivity for Adaptive Space-Saving and Hybrid.
3. CAIDA-B input-partition and query-resolution scaling under round-robin and
   visibility-suppression placement.
4. Per-window quality distributions and the extended round-robin scaling case.
5. Coordinator reduction and exact-head dissemination measurements.

Each stage writes raw per-window CSV files, an aggregate summary, plots, logs,
and a configuration snapshot beneath `results/runs/`.

## Retained supplementary evidence

The supplementary matrix retains additional experiments that improve
transparency without being required for the primary result set:

- temporal-controller policy ablations under scheduled difficulty changes;
- partition-local cardinality and residual-mass diagnostics;
- separate per-method, per-placement error and memory trajectories;
- detailed serial/parallel reducer and exact-head delta measurements.

Ready outputs are ordered and interpreted in `results/reference/README.md`.
Plot-only regeneration commands are collected in
`docs/SUPPLEMENTARY_RESULTS.md`.

The repository deliberately excludes discarded analyses whose methodology or
design no longer supports the current system: ambiguity adjustment,
single-process insertion-throughput comparisons, standalone obsolete MILP
baselines, the Twitter workload, and rejected synthetic end-to-end scenarios.
The controller ablation keeps round-robin placement fixed and changes only the
global frequency distribution. This isolates temporal control from placement
effects and requires neither MILP nor external data. Guarded margin comfort is
the selected controller. Ambiguity adjustment was removed from the
final method after it failed to provide a consistent resource--quality
improvement.
