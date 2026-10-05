#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib/common.sh"

datasets=(
  temporal_guard_step_schedule_n200
  temporal_guard_ramp_schedule_n200
  temporal_guard_burst_schedule_n200
  temporal_guard_oscillation_schedule_n200
)

"${ROOT}/scripts/check_environment.sh"

for dataset in "${datasets[@]}"; do
  stream_dir="${ROOT}/generator/generated/runs/${dataset}/streams"
  if [[ "${REGENERATE:-0}" == "1" || ! -d "${stream_dir}" ]]; then
    python3 "${ROOT}/generator/main.py" \
      --run-config "${ROOT}/generator/config/runs/${dataset}.json" \
      --run-name "${dataset}" \
      --no-plots
  else
    printf 'reusing generated dataset: %s\n' "${stream_dir}"
  fi
done

stamp="$(timestamp_utc)"
out_dir="${OUT_DIR:-${ROOT}/results/runs/temporal-controller-${stamp}}"
mkdir -p "${out_dir}"
record_environment "${out_dir}/environment.txt"

OUT_DIR="${out_dir}" \
DATASETS="${datasets[*]}" \
EPSILON_M=0.15 \
ALPHA_REQ=0.95 \
SS_EPS=per-item \
CLEAN_OUTPUT=1 \
bash "${ROOT}/experiments/calibration/run_temporal_controller_ablation.sh"

printf 'Temporal-controller ablation complete: %s\n' "${out_dir}"
