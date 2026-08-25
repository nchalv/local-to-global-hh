#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNNER="${PROJECT_ROOT}/experiments/end_to_end/run_caida_round_robin_comparison.sh"

EPSILON_M_VALUES="${EPSILON_M_VALUES:-0.05 0.10 0.15 0.20 0.25 0.30}"
OUT_ROOT="${OUT_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_epsilon_m_sweep}"
SS_EPS="${SS_EPS:-per-item}"
BUILD="${BUILD:-1}"

RR_STREAM="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_round_robin_n200/streams/stream_caida_200w_5s_n200_m100"

cd "${PROJECT_ROOT}"
mkdir -p "${OUT_ROOT}"

if [[ "${BUILD}" == "1" ]]; then
  cmake -S evaluator -B evaluator/build -DCMAKE_BUILD_TYPE=Release
  cmake --build evaluator/build --target hh_bench -j2
fi

for n in 200 400 600 800; do
  STREAM="${RR_STREAM}" \
  OUT_DIR="${OUT_ROOT}/round_robin_n${n}" \
  N_PARAM="${n}" \
  TOPK="${n}" \
  ADAPTIVE_EPSILON_M_VALUES="${EPSILON_M_VALUES}" \
  HYBRID_EPSILON_M_VALUES="${EPSILON_M_VALUES}" \
  INCLUDE_STATIC=0 \
  INCLUDE_HL=0 \
  INCLUDE_HYBRID=1 \
  SS_REDUCER=streaming \
  HYBRID_REDUCER=streaming \
  SS_EPS="${SS_EPS}" \
  BUILD=0 \
  CLEAN_OUTPUT=1 \
  bash "${RUNNER}" | tee "${OUT_ROOT}/round_robin_n${n}.log"
done

for n in 200 400 600 800; do
  stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_filter_aware_visibility_threshold07_n${n}/streams/stream_caida_200w_5s_n${n}_m100"
  STREAM="${stream}" \
  OUT_DIR="${OUT_ROOT}/filter_aware_threshold07_n${n}" \
  N_PARAM="${n}" \
  TOPK="${n}" \
  ADAPTIVE_EPSILON_M_VALUES="${EPSILON_M_VALUES}" \
  HYBRID_EPSILON_M_VALUES="${EPSILON_M_VALUES}" \
  INCLUDE_STATIC=0 \
  INCLUDE_HL=0 \
  INCLUDE_HYBRID=1 \
  SS_REDUCER=streaming \
  HYBRID_REDUCER=streaming \
  SS_EPS="${SS_EPS}" \
  BUILD=0 \
  CLEAN_OUTPUT=1 \
  bash "${RUNNER}" | tee "${OUT_ROOT}/filter_aware_threshold07_n${n}.log"
done

python3 experiments/end_to_end/plot_caida_epsilon_m_sweep.py \
  --root "${OUT_ROOT}" \
  --epsilon-m-values ${EPSILON_M_VALUES} \
  --out "${OUT_ROOT}/plots"
