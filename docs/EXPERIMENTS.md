# Experiment Catalog

This artifact separates the experiments retained in the paper from additional
diagnostics that support design decisions or expose negative results. Commands
are run from the repository root through `./artifact`.

## Reviewer workflow

| Stage | Command | External data | Purpose |
|---|---|---:|---|
| Environment | `./artifact check` | No | Check required tools and Python packages. |
| Build and tests | `./artifact test` | No | Build the evaluator and run its unit tests. |
| Smoke test | `./artifact smoke` | No | Generate a small deterministic stream and validate all principal methods. |
| Controller ablation | `./artifact temporal-controller` | No | Compare four downward-sizing policies under scheduled difficulty changes. |
| Stage CAIDA | `./artifact prepare-caida` | Yes | Validate and stage the two normalized CAIDA slices. |

The smoke test is the recommended first evaluation. It is not evidence used in
the paper; it verifies the complete generator--evaluator--summary path on a
small bundled configuration.

## Experiments retained in the paper

The paper reproduction matrix covers:

1. CAIDA-A promoted-head ablation under round-robin placement.
2. CAIDA-A margin sensitivity for Adaptive Space-Saving and Hybrid.
3. CAIDA-B input-partition and query-resolution scaling under round-robin and
   visibility-suppression placement.
4. Per-window quality distributions and the extended round-robin scaling case.
5. Coordinator reduction and exact-head dissemination measurements.

Each stage writes raw per-window CSV files, an aggregate summary, plots, logs,
and a configuration snapshot beneath `results/runs/`.

## Supplementary experiments

The supplementary matrix retains experiments that improve transparency but
were omitted from the paper for space:

- the complete margin sweep under visibility suppression;
- temporal-controller policy ablations under scheduled difficulty changes;
- partition-local cardinality and residual-mass diagnostics;
- separate per-method, per-placement error and memory trajectories;
- detailed serial/parallel reducer and exact-head delta measurements.

Ready aggregate outputs and their plot-only regeneration commands are described
in `docs/SUPPLEMENTARY_RESULTS.md`.

The artifact deliberately excludes discarded analyses whose methodology or
design no longer supports the submitted system: ambiguity adjustment,
single-process insertion-throughput comparisons, standalone obsolete MILP
baselines, the Twitter workload, and rejected synthetic end-to-end scenarios.
The controller ablation retains MILP-generated placement changes only as
controlled synthetic difficulty transitions, not as final comparison
baselines. Ambiguity adjustment was removed from the final method after it
failed to provide a consistent resource--quality improvement.
