#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNNER="${PROJECT_ROOT}/experiments/end_to_end/run_caida_round_robin_comparison.sh"

EPS_ROOT="${EPS_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_epsilon_m_final_fair}"
M_ROOT="${M_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_holdout_m_sweep_fair}"
N_ROOT="${N_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_holdout_n_sweep_fair}"
RUNTIME_ROOT="${RUNTIME_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_m_systems_optimizations_fair_runtime}"
DELTA_ROOT="${DELTA_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_m_systems_optimizations_fair_delta}"
RESET_EPSILON_VISIBILITY="${RESET_EPSILON_VISIBILITY:-0}"
RUN_EPSILON="${RUN_EPSILON:-1}"
RUN_M_SWEEP="${RUN_M_SWEEP:-1}"
RUN_N_SWEEP="${RUN_N_SWEEP:-1}"
RUN_RUNTIME="${RUN_RUNTIME:-1}"
RUN_DELTAS="${RUN_DELTAS:-1}"

EPSILON_VALUES="0.01 0.03 0.05 0.10 0.15 0.25 0.40 0.60"
COUNTS_B="${PROJECT_ROOT}/data_preparation/generated/caida_20180315_133230_200w_5s_dirA.jsonl"
MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib-hh-paper}"
export MPLCONFIGDIR

cd "${PROJECT_ROOT}"
mkdir -p "${MPLCONFIGDIR}"

cmake -S evaluator -B evaluator/build -DCMAKE_BUILD_TYPE=Release
cmake --build evaluator/build --target hh_bench -j2

valid_csv() {
  local path="$1"
  local expected_methods="$2"
  [[ -f "${path}" ]] || return 1
  python3 - "${path}" "${expected_methods}" <<'PY'
import csv
import sys
from collections import Counter

path = sys.argv[1]
expected = int(sys.argv[2])
counts = Counter()
try:
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if not row.get("window") or not row.get("method"):
                raise ValueError("truncated row")
            counts[int(row["window"])] += 1
except (OSError, ValueError, csv.Error):
    raise SystemExit(1)

if set(counts) != set(range(200)):
    raise SystemExit(1)
if any(value != expected for value in counts.values()):
    raise SystemExit(1)
PY
}

run_comparison() {
  local stream="$1"
  local out_dir="$2"
  local m="$3"
  local n="$4"
  local expected_methods="$5"
  local mode="$6"
  local csv="${out_dir}/csv/caida_round_robin_n${n}_all_methods.csv"
  local log="${out_dir}/run.log"

  if valid_csv "${csv}" "${expected_methods}"; then
    printf 'complete; skipping %s\n' "${out_dir}"
    return
  fi
  [[ -d "${stream}" ]] || {
    printf 'missing stream directory: %s\n' "${stream}" >&2
    exit 1
  }
  mkdir -p "${out_dir}"
  printf '\nRunning %s (m=%s, n=%s)\n' "${out_dir}" "${m}" "${n}"

  if [[ "${mode}" == "epsilon" ]]; then
    STREAM="${stream}" \
    OUT_DIR="${out_dir}" \
    M="${m}" \
    N_PARAM="${n}" \
    TOPK="${n}" \
    ADAPTIVE_EPSILON_M_VALUES="${EPSILON_VALUES}" \
    HYBRID_EPSILON_M_VALUES="${EPSILON_VALUES}" \
    ALPHA_REQ=0.95 \
    AMB_ADJUST=off \
    INCLUDE_STATIC=0 \
    INCLUDE_HL=0 \
    INCLUDE_HYBRID=1 \
    SS_REDUCER=streaming \
    HYBRID_REDUCER=streaming \
    SS_EPS=per-item \
    CLEAN_OUTPUT=1 \
    BUILD=0 \
    bash "${RUNNER}" 2>&1 | tee "${log}"
  else
    local w08=$((n * 2 / 25))
    local w16=$((n * 4 / 25))
    local w32=$((n * 8 / 25))
    local w64=$((n * 16 / 25))
    STREAM="${stream}" \
    OUT_DIR="${out_dir}" \
    M="${m}" \
    N_PARAM="${n}" \
    TOPK="${n}" \
    ADAPTIVE_EPSILON_M_VALUES=0.15 \
    HYBRID_EPSILON_M_VALUES=0.40 \
    ALPHA_REQ=0.95 \
    AMB_ADJUST=off \
    HL_W_VALUES="${w08} ${w16} ${w32} ${w64}" \
    INCLUDE_STATIC=0 \
    INCLUDE_HL=1 \
    INCLUDE_HYBRID=1 \
    SS_REDUCER=streaming \
    HYBRID_REDUCER=streaming \
    SS_EPS=per-item \
    CLEAN_OUTPUT=1 \
    BUILD=0 \
    bash "${RUNNER}" 2>&1 | tee "${log}"
  fi

  valid_csv "${csv}" "${expected_methods}" || {
    printf 'incomplete output: %s\n' "${csv}" >&2
    exit 1
  }
}

printf '\n=== 1. Finish CAIDA-A epsilon_M sweep ===\n'
if [[ "${RUN_EPSILON}" == "1" ]]; then
for n in 200 400 600 800; do
  csv="${EPS_ROOT}/round_robin_n${n}/csv/caida_round_robin_n${n}_all_methods.csv"
  valid_csv "${csv}" 16 || {
    printf 'completed round-robin margin result is missing or invalid: %s\n' "${csv}" >&2
    exit 1
  }
done

if [[ "${RESET_EPSILON_VISIBILITY}" == "1" ]]; then
  rm -rf \
    "${EPS_ROOT}"/filter_aware_threshold07_n* \
    "${EPS_ROOT}"/filter_aware_threshold07_n*.log \
    "${EPS_ROOT}/plots"
fi

for n in 200 400 600 800; do
  stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_filter_aware_visibility_threshold07_n${n}/streams/stream_caida_200w_5s_n${n}_m100"
  run_comparison \
    "${stream}" \
    "${EPS_ROOT}/filter_aware_threshold07_n${n}" \
    100 "${n}" 16 epsilon
done

python3 experiments/end_to_end/plot_caida_epsilon_m_sweep.py \
  --root "${EPS_ROOT}" \
  --epsilon-m-values ${EPSILON_VALUES} \
  --out "${EPS_ROOT}/plots"
fi

printf '\n=== 2. CAIDA-B input-partition scaling ===\n'
if [[ "${RUN_M_SWEEP}" == "1" ]]; then
for placement in round_robin visibility_suppression; do
  m_values=(20 50 100 200 250 500)
  [[ "${placement}" == "round_robin" ]] && m_values+=(1000)
  for m in "${m_values[@]}"; do
    if [[ "${placement}" == "round_robin" ]]; then
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_round_robin_m${m}/streams/stream_caida_200w_5s_n400_m${m}"
    else
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_visibility_suppression_threshold07_n400_m${m}/streams/stream_caida_200w_5s_n400_m${m}"
    fi
    run_comparison "${stream}" "${M_ROOT}/${placement}_m${m}" "${m}" 400 6 scaling
  done
done

python3 experiments/end_to_end/plot_caida_m_sweep.py \
  --root "${M_ROOT}" \
  --counts "${COUNTS_B}" \
  --out "${M_ROOT}/plots_single_column" \
  --single-column \
  --omit-hl-008 \
  --omit-static-ss
fi

printf '\n=== 3. CAIDA-B query-resolution scaling ===\n'
if [[ "${RUN_N_SWEEP}" == "1" ]]; then
mkdir -p "${N_ROOT}"
for placement in round_robin visibility_suppression; do
  source_dir="${M_ROOT}/${placement}_m100"
  target_dir="${N_ROOT}/${placement}_n400"
  target_csv="${target_dir}/csv/caida_round_robin_n400_all_methods.csv"
  if ! valid_csv "${target_csv}" 6; then
    rm -rf "${target_dir}"
    cp -a "${source_dir}" "${target_dir}"
  fi
done

for placement in round_robin visibility_suppression; do
  n_values=(200 600 800 1000)
  [[ "${placement}" == "round_robin" ]] && n_values+=(1200 1400 1600 1800 2000)
  for n in "${n_values[@]}"; do
    if [[ "${placement}" == "round_robin" ]]; then
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_round_robin_m100/streams/stream_caida_200w_5s_n400_m100"
    else
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_visibility_suppression_threshold07_n${n}_m100/streams/stream_caida_200w_5s_n${n}_m100"
    fi
    run_comparison "${stream}" "${N_ROOT}/${placement}_n${n}" 100 "${n}" 6 scaling
  done
done

python3 experiments/end_to_end/plot_caida_n_sweep.py \
  --root "${N_ROOT}" \
  --counts "${COUNTS_B}" \
  --out "${N_ROOT}/plots_single_column" \
  --single-column \
  --omit-hl-008 \
  --omit-static-ss

python3 experiments/end_to_end/plot_caida_window_cdfs.py \
  --root "${N_ROOT}" \
  --n 600 \
  --out "${N_ROOT}/plots_single_column/caida_window_cdfs.pdf"

python3 experiments/end_to_end/plot_caida_round_robin_control.py \
  --m-summary "${M_ROOT}/plots_single_column/m_sweep_summary.csv" \
  --n-summary "${N_ROOT}/plots_single_column/n_sweep_summary.csv" \
  --log-m-axis \
  --out "${N_ROOT}/plots_single_column/caida_round_robin_control.pdf"
fi

printf '\n=== 4. Coordinator reducer runtime ===\n'
if [[ "${RUN_RUNTIME}" == "1" ]]; then
M_VALUES="100 500 1000" \
OUT_ROOT="${RUNTIME_ROOT}" \
REPETITIONS=5 \
PARALLELISM=8 \
N_PARAM=400 \
BUILD=0 \
CLEAN_OUTPUT=1 \
RUN_CAIDA=1 \
RUN_SYNTHETIC_DELTA=0 \
RUNTIME_ONLY=1 \
bash experiments/end_to_end/run_caida_m_systems_optimizations.sh
fi

printf '\n=== 5. Exact-head delta traffic ===\n'
if [[ "${RUN_DELTAS}" == "1" ]]; then
M_VALUES="100 500 1000" \
OUT_ROOT="${DELTA_ROOT}" \
REPETITIONS=1 \
PARALLELISM=8 \
N_PARAM=400 \
BUILD=0 \
CLEAN_OUTPUT=1 \
RUN_CAIDA=1 \
RUN_SYNTHETIC_DELTA=0 \
DELTA_ONLY=1 \
bash experiments/end_to_end/run_caida_m_systems_optimizations.sh
fi

printf '\nAll remaining paper experiments completed.\n'
printf 'epsilon sweep: %s\n' "${EPS_ROOT}"
printf 'm sweep:       %s\n' "${M_ROOT}"
printf 'n sweep:       %s\n' "${N_ROOT}"
printf 'runtime:       %s\n' "${RUNTIME_ROOT}"
printf 'head deltas:   %s\n' "${DELTA_ROOT}"
