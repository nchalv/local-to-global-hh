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
./scripts/check_environment.sh
./scripts/build.sh
```

See `docs/REPRODUCIBILITY.md` for the staged commands for data preparation,
calibration, final evaluation, and figure/table generation.
