# Generator Configurations

Generator presets are grouped by role:

- `smoke/`: small self-contained validation used by `./artifact smoke`.
- `runs/`: composed run presets.
- `generation/`: topology, output, and identity-evolution settings.
- `scenarios/`: generated global frequency distributions.
- `partitioning/`: placement policies applied to global windows.

The canonical reviewer workflows select these files through top-level artifact
commands or the drivers listed in `experiments/README.md`. Some additional
presets remain because retained drivers and implementation tests reference
them; their presence does not make them part of the submitted evaluation.
