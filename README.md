# Local-to-Global Heavy-Hitter Artifact

This repository contains the reproducibility artifact for the distributed
heavy-hitter evaluation. It separates the executable implementation and
paper-facing reproduction workflow from the research workspace.

## Contents

- `evaluator/`: C++17 sketches, coordinator, reducers, metrics, and tests.
- `generator/`: normalized-window generation and final partitioning policies.
- `data_preparation/`: conversion of external traces into normalized counts.
- `experiments/`: selected calibration, comparison, plotting, and summary tools.
- `scripts/`: artifact-level entry points and environment checks.
- `docs/`: dataset, accounting, and reproduction notes.
- `results/reference/`: compact expected summaries and checksums; large outputs
  are generated locally and are not committed by default.

The manuscript source and exploratory experiment directories are intentionally
excluded.

## Reproduction boundary

The artifact reproduces the retained calibration and end-to-end results. It
does not redistribute CAIDA data. Users must obtain the permitted CAIDA slice
independently and place the normalized per-window counts at the path described
in `docs/DATASETS.md`.

All generated data and experiment outputs belong under `generated/` or
`results/`, both of which are ignored by Git. The source tree remains clean
after a run.

## Quick start

```bash
python3 -m venv .hh-venv
. .hh-venv/bin/activate
pip install -r requirements.txt
./artifact test
./artifact smoke
```

The smoke workflow generates a small deterministic stream, evaluates static and
adaptive Space-Saving, HeavyLocker, and Hybrid, and validates the resulting
per-window CSV. It requires no external data and normally completes within a
few minutes.

See `docs/REPRODUCIBILITY.md` for CAIDA preparation and the staged paper and
supplementary workflows. `docs/EXPERIMENTS.md` maps commands to experiment
families and explains which exploratory analyses are intentionally excluded.
