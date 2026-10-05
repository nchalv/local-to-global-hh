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
- ambiguity adjustment enabled and disabled on the same CAIDA-A grid;
- round-robin temporal-controller policy ablations;
- partition-local cardinality and residual-mass diagnostics;
- detailed serial/parallel reducer and exact-head delta measurements.

The artifact deliberately excludes discarded analyses whose methodology does
not support a paper claim: single-process insertion-throughput comparisons,
obsolete MILP placements, the Twitter workload, and rejected synthetic
end-to-end scenarios.
