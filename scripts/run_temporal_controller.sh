#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib/common.sh"

"${ROOT}/scripts/check_environment.sh"

stamp="$(timestamp_utc)"
out_dir="${OUT_DIR:-${ROOT}/results/runs/temporal-controller-${stamp}}"
mkdir -p "${out_dir}"
record_environment "${out_dir}/environment.txt"

OUT_DIR="${out_dir}" \
BUILD="${BUILD:-1}" \
bash "${ROOT}/experiments/calibration/run_controller_composite_round_robin.sh"

printf 'Temporal-controller ablation complete: %s\n' "${out_dir}"
printf 'Validation report: %s\n' "${out_dir}/analysis/acceptance.txt"
