#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HH_BENCH="${PROJECT_ROOT}/evaluator/build/hh_bench"
COUNTS_B="${PROJECT_ROOT}/data_preparation/generated/caida_20180315_133230_200w_5s_dirA.jsonl"

FRESH_M_ROOT="${FRESH_M_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_holdout_m_sweep_fair}"
FRESH_N_ROOT="${FRESH_N_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_holdout_n_sweep_fair}"
SUPPLEMENT_ROOT="${SUPPLEMENT_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_static_ss_supplement_fair}"
MERGED_M_ROOT="${MERGED_M_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_holdout_m_sweep_fair_with_static}"
MERGED_N_ROOT="${MERGED_N_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_holdout_n_sweep_fair_with_static}"
MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib-hh-static-supplement}"
BUILD="${BUILD:-1}"
export MPLCONFIGDIR

METHODS="oracle,ss[policy=static q=2n reducer=streaming],ss[policy=static q=4n reducer=streaming]"

cd "${PROJECT_ROOT}"
mkdir -p "${MPLCONFIGDIR}" "${SUPPLEMENT_ROOT}"

if [[ "${BUILD}" == "1" ]]; then
  cmake -S evaluator -B evaluator/build -DCMAKE_BUILD_TYPE=Release
  cmake --build evaluator/build --target hh_bench -j2
fi

valid_static_csv() {
  local path="$1"
  [[ -f "${path}" ]] || return 1
  python3 - "${path}" <<'PY'
import csv
import sys
from collections import Counter

expected = Counter({
    "ss[policy=static q=2n]": 1,
    "ss[policy=static q=4n]": 1,
})
by_window = {}
try:
    with open(sys.argv[1], newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            window = int(row["window"])
            by_window.setdefault(window, Counter())[row["method"]] += 1
except (OSError, KeyError, TypeError, ValueError, csv.Error):
    raise SystemExit(1)

if set(by_window) != set(range(200)):
    raise SystemExit(1)
if any(methods != expected for methods in by_window.values()):
    raise SystemExit(1)
PY
}

run_static() {
  local stream="$1"
  local out_dir="$2"
  local m="$3"
  local n="$4"
  local csv="${out_dir}/csv/caida_round_robin_n${n}_static_ss.csv"
  local log="${out_dir}/run.log"
  local configuration="${out_dir}/configuration.txt"

  [[ -d "${stream}" ]] || {
    printf 'missing stream directory: %s\n' "${stream}" >&2
    exit 1
  }

  mkdir -p "${out_dir}/csv"
  {
    printf 'experiment=static_ss_plot_supplement\n'
    printf 'stream=%s\n' "${stream}"
    printf 'm=%s\n' "${m}"
    printf 'n=%s\n' "${n}"
    printf 'memory_kib=128\n'
    printf 'methods=static_ss_2n static_ss_4n\n'
    printf 'reducer=streaming\n'
    printf 'per_window_csv=%s\n' "${csv}"
    printf 'run_log=%s\n' "${log}"
  } > "${configuration}"

  if valid_static_csv "${csv}" && [[ -s "${log}" ]]; then
    printf 'complete; skipping %s\n' "${out_dir}"
    return
  fi

  printf '\nStatic SS supplement: m=%s n=%s\n' "${m}" "${n}"
  printf 'Log: %s\n' "${log}"
  "${HH_BENCH}" \
    "${stream}" \
    "${m}" \
    "${n}" \
    128 \
    "${METHODS}" \
    --csv-out "${csv}" \
    --topk "${n}" \
    2>&1 | tee "${log}"

  valid_static_csv "${csv}" || {
    printf 'incomplete static supplement: %s\n' "${csv}" >&2
    exit 1
  }
}

printf '\n=== Static SS supplement for Figure 5 (m scaling) ===\n'
for placement in round_robin visibility_suppression; do
  for m in 20 100 500; do
    if [[ "${placement}" == "round_robin" ]]; then
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_round_robin_m${m}/streams/stream_caida_200w_5s_n400_m${m}"
    else
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_visibility_suppression_threshold07_n400_m${m}/streams/stream_caida_200w_5s_n400_m${m}"
    fi
    run_static "${stream}" "${SUPPLEMENT_ROOT}/m_scaling/${placement}_m${m}" "${m}" 400
  done
done

printf '\n=== Static SS supplement for Figure 6 (n scaling) ===\n'
for placement in round_robin visibility_suppression; do
  for n in 200 600 1000; do
    if [[ "${placement}" == "round_robin" ]]; then
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_round_robin_m100/streams/stream_caida_200w_5s_n400_m100"
    else
      stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_visibility_suppression_threshold07_n${n}_m100/streams/stream_caida_200w_5s_n${n}_m100"
    fi
    run_static "${stream}" "${SUPPLEMENT_ROOT}/n_scaling/${placement}_n${n}" 100 "${n}"
  done
done

merge_case() {
  local base="$1"
  local static_csv="$2"
  local out="$3"
  if [[ -n "${static_csv}" && -f "${static_csv}" ]]; then
    python3 experiments/end_to_end/merge_static_ss_supplement.py \
      --base "${base}" \
      --static "${static_csv}" \
      --out "${out}"
  else
    mkdir -p "$(dirname "${out}")"
    cp "${base}" "${out}"
  fi
}

printf '\n=== Build isolated merged m-sweep root ===\n'
for placement in round_robin visibility_suppression; do
  m_values=(20 50 100 200 250 500)
  [[ "${placement}" == "round_robin" ]] && m_values+=(1000)
  for m in "${m_values[@]}"; do
    base="${FRESH_M_ROOT}/${placement}_m${m}/csv/caida_round_robin_n400_all_methods.csv"
    static_csv="${SUPPLEMENT_ROOT}/m_scaling/${placement}_m${m}/csv/caida_round_robin_n400_static_ss.csv"
    out="${MERGED_M_ROOT}/${placement}_m${m}/csv/caida_round_robin_n400_all_methods.csv"
    [[ -f "${base}" ]] || { printf 'missing fresh result: %s\n' "${base}" >&2; exit 1; }
    merge_case "${base}" "${static_csv}" "${out}"
  done
done

printf '\n=== Build isolated merged n-sweep root ===\n'
for placement in round_robin visibility_suppression; do
  n_values=(200 400 600 800 1000)
  [[ "${placement}" == "round_robin" ]] && n_values+=(1200 1400 1600 1800 2000)
  for n in "${n_values[@]}"; do
    base="${FRESH_N_ROOT}/${placement}_n${n}/csv/caida_round_robin_n${n}_all_methods.csv"
    static_csv="${SUPPLEMENT_ROOT}/n_scaling/${placement}_n${n}/csv/caida_round_robin_n${n}_static_ss.csv"
    if [[ "${n}" == "400" ]]; then
      static_csv="${SUPPLEMENT_ROOT}/m_scaling/${placement}_m100/csv/caida_round_robin_n400_static_ss.csv"
    fi
    out="${MERGED_N_ROOT}/${placement}_n${n}/csv/caida_round_robin_n${n}_all_methods.csv"
    [[ -f "${base}" ]] || { printf 'missing fresh result: %s\n' "${base}" >&2; exit 1; }
    merge_case "${base}" "${static_csv}" "${out}"
  done
done

printf '\n=== Regenerate static-inclusive previews ===\n'
python3 experiments/end_to_end/plot_caida_m_sweep.py \
  --root "${MERGED_M_ROOT}" \
  --counts "${COUNTS_B}" \
  --out "${MERGED_M_ROOT}/plots_single_column" \
  --single-column \
  --omit-hl-008

python3 experiments/end_to_end/plot_caida_n_sweep.py \
  --root "${MERGED_N_ROOT}" \
  --counts "${COUNTS_B}" \
  --out "${MERGED_N_ROOT}/plots_single_column" \
  --single-column \
  --omit-hl-008

printf '\nStatic-inclusive previews written to:\n%s\n%s\n' \
  "${MERGED_M_ROOT}/plots_single_column" \
  "${MERGED_N_ROOT}/plots_single_column"
printf '\nPer-window reports, configurations, and run logs are retained under:\n%s\n' \
  "${SUPPLEMENT_ROOT}"
