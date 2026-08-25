#!/usr/bin/env bash
set -euo pipefail

# End-to-end CAIDA round-robin comparison.
# User-facing margin values are expressed as epsilon_M units. The evaluator
# converts epsilon_M internally to the absolute budget r_M = epsilon_M / n.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HH_BENCH="${PROJECT_ROOT}/evaluator/build/hh_bench"

STREAM="${STREAM:-${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_round_robin_n200/streams/stream_caida_200w_5s_n200_m100}"
OUT_DIR="${OUT_DIR:-experiments/end_to_end/caida_round_robin/full_comparison}"
if [[ "${OUT_DIR}" != /* ]]; then
  OUT_DIR="${PROJECT_ROOT}/${OUT_DIR}"
fi
CSV_DIR="${OUT_DIR}/csv"

M="${M:-100}"
N_PARAM="${N_PARAM:-200}"
MEM_KIB="${MEM_KIB:-128}"
TOPK="${TOPK:-200}"

ADAPTIVE_EPSILON_M_VALUES=(${ADAPTIVE_EPSILON_M_VALUES:-0.15})
HYBRID_EPSILON_M="${HYBRID_EPSILON_M:-0.15}"
HYBRID_EPSILON_M_VALUES=(${HYBRID_EPSILON_M_VALUES:-${HYBRID_EPSILON_M}})
ALPHA_REQ="${ALPHA_REQ:-0.95}"
SS_EPS="${SS_EPS:-per-item}"
AMB_ADJUST="${AMB_ADJUST:-off}"
# Keep the HeavyLocker width grid proportional to the selected HH denominator:
# 0.08n, 0.16n, 0.32n, 0.64n, and 1.28n.  An explicit HL_W_VALUES override
# remains available for dedicated width sweeps.
if [[ -n "${HL_W_VALUES:-}" ]]; then
  HL_W_VALUES=(${HL_W_VALUES})
else
  HL_W_VALUES=(
    $((N_PARAM * 2 / 25))
    $((N_PARAM * 4 / 25))
    $((N_PARAM * 8 / 25))
    $((N_PARAM * 16 / 25))
    $((N_PARAM * 32 / 25))
  )
fi
INCLUDE_HL="${INCLUDE_HL:-1}"
INCLUDE_CERTIFIED_HL="${INCLUDE_CERTIFIED_HL:-0}"
INCLUDE_HYBRID="${INCLUDE_HYBRID:-1}"
INCLUDE_STATIC="${INCLUDE_STATIC:-1}"
SS_REDUCER="${SS_REDUCER:-hash}"
HYBRID_REDUCER="${HYBRID_REDUCER:-hash}"

for reducer in "${SS_REDUCER}" "${HYBRID_REDUCER}"; do
  if [[ "${reducer}" != "hash" && "${reducer}" != "streaming" && "${reducer}" != "parallel-streaming" ]]; then
    echo "SS_REDUCER and HYBRID_REDUCER must be hash, streaming, or parallel-streaming" >&2
    exit 2
  fi
done

CLEAN_OUTPUT="${CLEAN_OUTPUT:-1}"
DRY_RUN="${DRY_RUN:-0}"
BUILD="${BUILD:-1}"

cd "${PROJECT_ROOT}"

if [[ "${DRY_RUN}" != "1" && "${BUILD}" == "1" ]]; then
  cmake -S evaluator -B evaluator/build -DCMAKE_BUILD_TYPE=Release
  cmake --build evaluator/build --target hh_bench
fi

if [[ "${DRY_RUN}" != "1" && ! -d "${STREAM}" ]]; then
  echo "missing stream directory: ${STREAM}" >&2
  exit 1
fi

hl_methods=""
if [[ "${INCLUDE_HL}" == "1" ]]; then
  for w in "${HL_W_VALUES[@]}"; do
    hl_methods+=",hl[hl-w=${w} hl-d=6 hl-L=0.7 hl-lossy=2]"
  done
fi
if [[ "${INCLUDE_CERTIFIED_HL}" == "1" ]]; then
  for w in "${HL_W_VALUES[@]}"; do
    hl_methods+=",hl[hl-w=${w} hl-d=6 hl-L=0.7 hl-lossy=2 hl-cert=on]"
  done
fi

adaptive_methods=""
for epsilon_m in "${ADAPTIVE_EPSILON_M_VALUES[@]}"; do
  epsilon_arg="$(awk -v epsilon_m="${epsilon_m}" 'BEGIN { printf "%.10g", epsilon_m }')"
  adaptive_methods+=",ss[policy=difficulty reducer=${SS_REDUCER} alpha-req=${ALPHA_REQ} epsilon-m=${epsilon_arg} downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=${AMB_ADJUST} diff-mode=predictive ss-eps=${SS_EPS}]"
done

hybrid_methods=""
if [[ "${INCLUDE_HYBRID}" == "1" ]]; then
  for epsilon_m in "${HYBRID_EPSILON_M_VALUES[@]}"; do
    hybrid_epsilon_arg="$(awk -v epsilon_m="${epsilon_m}" 'BEGIN { printf "%.10g", epsilon_m }')"
    hybrid_methods+=",hybrid[hyb-head=topn-frontier hyb-tail=difficulty reducer=${HYBRID_REDUCER} alpha-req=${ALPHA_REQ} epsilon-m=${hybrid_epsilon_arg} downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=${AMB_ADJUST} diff-mode=predictive ss-eps=${SS_EPS}]"
  done
fi
static_methods=""
if [[ "${INCLUDE_STATIC}" == "1" ]]; then
  static_methods=",ss[policy=static q=n reducer=${SS_REDUCER}],ss[policy=static q=2n reducer=${SS_REDUCER}],ss[policy=static q=4n reducer=${SS_REDUCER}]"
fi
methods="oracle${static_methods}${hl_methods}${adaptive_methods}${hybrid_methods}"

if [[ "${DRY_RUN}" != "1" ]]; then
  if [[ "${CLEAN_OUTPUT}" == "1" ]]; then
    rm -rf "${CSV_DIR}" "${OUT_DIR}/configuration.txt"
  fi
  mkdir -p "${CSV_DIR}"
  {
    printf 'experiment=caida_round_robin_end_to_end\n'
    printf 'stream=%s\n' "${STREAM}"
    printf 'm=%s\n' "${M}"
    printf 'n=%s\n' "${N_PARAM}"
    printf 'memory_kib=%s\n' "${MEM_KIB}"
    printf 'topk=%s\n' "${TOPK}"
    printf 'adaptive_epsilon_m_values=%s\n' "${ADAPTIVE_EPSILON_M_VALUES[*]}"
    printf 'hybrid_epsilon_m_values=%s\n' "${HYBRID_EPSILON_M_VALUES[*]}"
    printf 'alpha=%s\n' "${ALPHA_REQ}"
    printf 'ambiguity_adjustment=%s\n' "${AMB_ADJUST}"
    printf 'hl_w_values=%s\n' "${HL_W_VALUES[*]}"
    printf 'include_hl=%s\n' "${INCLUDE_HL}"
    printf 'include_certified_hl=%s\n' "${INCLUDE_CERTIFIED_HL}"
    printf 'include_static=%s\n' "${INCLUDE_STATIC}"
    printf 'hl_d=6\n'
    printf 'hl_L=0.7\n'
    printf 'hl_lossy=2\n'
    printf 'ss_error_mode=%s\n' "${SS_EPS}"
    printf 'ss_reducer=%s\n' "${SS_REDUCER}"
    printf 'hybrid_reducer=%s\n' "${HYBRID_REDUCER}"
  } > "${OUT_DIR}/configuration.txt"
fi

command=(
  "${HH_BENCH}"
  "${STREAM}"
  "${M}"
  "${N_PARAM}"
  "${MEM_KIB}"
  "${methods}"
  --csv-out "${CSV_DIR}/caida_round_robin_n${N_PARAM}_all_methods.csv"
  --topk "${TOPK}"
)

if [[ "${DRY_RUN}" == "1" ]]; then
  printf '%q ' "${command[@]}"
  printf '\n'
else
  "${command[@]}"
fi
