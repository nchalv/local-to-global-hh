# Local-to-Global Heavy-Hitter Artifact

This repository contains the implementation and reproducibility material for
deterministic global heavy-hitter inference from bounded partition-local
summaries. A quick-start workflow is separated from long-running dataset
preparation and full experiment orchestration.

## Start here

1. Validate the implementation with `./artifact test`.
2. Exercise the complete local pipeline with `./artifact smoke`.
3. Verify and inspect the committed evidence with
   `./artifact verify-results` and `results/reference/README.md`.
4. Use `docs/REPRODUCIBILITY.md` only when reproducing CAIDA-dependent runs.

The first three steps require no external dataset. The smoke workload is a
functional check rather than part of the reported experimental evidence.

## Contents

- `evaluator/`: C++17 sketches, coordinator, reducers, metrics, and tests.
- `generator/`: normalized-window generation and final partitioning policies.
- `data_preparation/`: conversion of external traces into normalized counts.
- `experiments/`: retained experiment drivers, summarizers, and plotters. See
  `experiments/README.md` before invoking an internal script directly.
- `scripts/`: artifact-level entry points and environment checks.
- `docs/`: dataset, accounting, and reproduction notes.
- `results/reference/`: ready-to-inspect evidence, interpretation, and
  checksums. This is the entry point for the committed results.

The manuscript source and exploratory experiment directories are intentionally
excluded.

## Reproduction boundary

The artifact reproduces the retained calibration and end-to-end results. It
does not redistribute CAIDA data. Users must obtain the permitted CAIDA slice
independently and place the normalized per-window counts at the path described
in `docs/DATASETS.md`.

Generated streams belong under `generated/`, and local experiment outputs
belong under `results/runs/`; both are ignored by Git. Curated evidence under
`results/reference/` is tracked and protected by checksums.

## Quick validation

```bash
python3 -m venv .hh-venv
. .hh-venv/bin/activate
pip install -r requirements.txt
./artifact test
./artifact smoke
./artifact verify-results
```

The smoke workflow generates a small deterministic stream, evaluates static and
adaptive Space-Saving, HeavyLocker, and Hybrid, and validates the resulting
per-window CSV. It requires no external data and normally completes within a
few minutes.

See `docs/EXPERIMENTS.md` for the evaluation scope and
`docs/REPRODUCIBILITY.md` for CAIDA preparation and long-running workflows.
The committed supplementary evidence is indexed by
`results/reference/README.md`.
