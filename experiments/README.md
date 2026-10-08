# Experiment Tools

This directory contains the retained benchmark drivers and the scripts that
summarize or visualize their output. Start with the top-level `./artifact`
commands unless a specific internal stage needs to be run directly.

## Calibration

- `run_hybrid_ablation.sh`, `summarize_hybrid_ablation.py`, and
  `plot_hybrid_ablation.py`: promoted-head policy selection on CAIDA-A.
- `run_controller_composite_round_robin.sh`,
  `run_temporal_controller_ablation.sh`, `summarize_temporal_grid.py`, and
  `analyze_controller_composite.py`: self-contained fixed-placement
  ablation of the selected guarded margin-comfort controller and three
  alternatives, exposed as `./artifact temporal-controller`.

## End-to-end evaluation

- `run_caida_epsilon_m_sweep.sh`: Adaptive Space-Saving and Hybrid margin
  sweep.
- `rerun_remaining_paper_experiments.sh`: retained CAIDA-B scaling matrix.
- `run_caida_m_systems_optimizations.sh`: coordinator reduction and exact-head
  update measurements.
- `run_static_ss_plot_supplement.sh`: fixed Space-Saving reference points used
  in the comparison plots.

The remaining Python files consume benchmark CSVs to create the corresponding
summaries or plots. Their expected inputs are documented by their command-line
help and by `docs/SUPPLEMENTARY_RESULTS.md`. Generated outputs belong under
`results/runs/`; generated streams belong under `generated/runs/`.

Historical configuration files are retained where an active driver still
references them. Deprecated or exploratory experiment families are not part
of the documented workflow above.
