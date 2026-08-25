#!/usr/bin/env bash
set -euo pipefail

# Hybrid exact-approximate ablation after fixing the baseline controller
# configuration. The frontier policy promotes K_+ union K_?.
# The topn-frontier policy promotes Top_n union K_+ union K_?.
# The confirmed policy promotes only certified heavy hitters K_+.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HH_BENCH="${PROJECT_ROOT}/evaluator/build/hh_bench"

OUT_DIR="${OUT_DIR:-experiments/calibration/hybrid_ablation}"
if [[ "${OUT_DIR}" != /* ]]; then
  OUT_DIR="${PROJECT_ROOT}/${OUT_DIR}"
fi
CSV_DIR="${OUT_DIR}/csv"

M="${M:-100}"
MEM_KIB="${MEM_KIB:-128}"
TOPK="${TOPK:-}"

EPSILON_M="${EPSILON_M:-0.15}"
ALPHA_REQ="${ALPHA_REQ:-0.95}"
SS_EPS="${SS_EPS:-per-item}"
AMB_ADJUST="${AMB_ADJUST:-off}"

CLEAN_OUTPUT="${CLEAN_OUTPUT:-1}"
DRY_RUN="${DRY_RUN:-0}"

declare -A STREAMS=(
  [caida_round_robin_n100]="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_round_robin_n200/streams/stream_caida_200w_5s_n200_m100"
  [caida_round_robin_n200]="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_round_robin_n200/streams/stream_caida_200w_5s_n200_m100"
  [caida_round_robin_n400]="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_round_robin_n200/streams/stream_caida_200w_5s_n200_m100"
  [caida_round_robin_n600]="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_130000_200w_5s_dirA_round_robin_n200/streams/stream_caida_200w_5s_n200_m100"
  [milp_certificate_adversary_n100]="${PROJECT_ROOT}/generator/generated/runs/hybrid_ablation_head_halo_milp_certificate_adversary_n100/streams"
  [milp_certificate_adversary_n200]="${PROJECT_ROOT}/generator/generated/runs/hybrid_ablation_head_halo_milp_certificate_adversary_n200/streams"
  [milp_certificate_adversary_n400]="${PROJECT_ROOT}/generator/generated/runs/hybrid_ablation_head_halo_milp_certificate_adversary_n400/streams"
  [round_robin_n100]="${PROJECT_ROOT}/generator/generated/runs/hybrid_ablation_head_halo_round_robin/streams"
  [round_robin_n200]="${PROJECT_ROOT}/generator/generated/runs/hybrid_ablation_head_halo_round_robin/streams"
  [round_robin_n400]="${PROJECT_ROOT}/generator/generated/runs/hybrid_ablation_head_halo_round_robin/streams"
)

declare -A N_PARAMS=(
  [caida_round_robin_n100]=100
  [caida_round_robin_n200]=200
  [caida_round_robin_n400]=400
  [caida_round_robin_n600]=600
  [milp_certificate_adversary_n100]=100
  [milp_certificate_adversary_n200]=200
  [milp_certificate_adversary_n400]=400
  [round_robin_n100]=100
  [round_robin_n200]=200
  [round_robin_n400]=400
)

DATASETS=(${DATASETS:-milp_certificate_adversary_n100 milp_certificate_adversary_n200 milp_certificate_adversary_n400 round_robin_n100 round_robin_n200 round_robin_n400})

cd "${PROJECT_ROOT}"

if [[ "${DRY_RUN}" != "1" ]]; then
  cmake -S evaluator -B evaluator/build -DCMAKE_BUILD_TYPE=Release
  cmake --build evaluator/build --target hh_bench
fi

if [[ "${DRY_RUN}" != "1" ]]; then
  if [[ "${CLEAN_OUTPUT}" == "1" ]]; then
    rm -rf \
      "${CSV_DIR}" \
      "${OUT_DIR}/plots" \
      "${OUT_DIR}/summary.csv" \
      "${OUT_DIR}/configuration.txt"
  fi
  mkdir -p "${CSV_DIR}"
  {
    printf 'experiment=hybrid_ablation\n'
    printf 'datasets=%s\n' "${DATASETS[*]}"
    printf 'head_policies=confirmed frontier topn-frontier\n'
    printf 'm=%s\n' "${M}"
    printf 'epsilon_m=%s\n' "${EPSILON_M}"
    printf 'alpha=%s\n' "${ALPHA_REQ}"
    printf 'temporal_controller=guarded_margin_comfort\n'
    printf 'tail_policy=difficulty\n'
    printf 'memory_kib=%s\n' "${MEM_KIB}"
    printf 'topk=%s\n' "${TOPK:-n (dataset-specific)}"
    printf 'input_streams=dataset-specific; dataset names identify the source and threshold\n'
    printf 'ss_error_mode=%s\n' "${SS_EPS}"
    printf 'ambiguity_adjustment=%s\n' "${AMB_ADJUST}"
  } > "${OUT_DIR}/configuration.txt"
fi

for dataset in "${DATASETS[@]}"; do
  if [[ -z "${STREAMS[$dataset]:-}" ]]; then
    echo "unknown dataset: ${dataset}" >&2
    echo "known datasets: ${!STREAMS[*]}" >&2
    exit 1
  fi

  stream="${STREAMS[$dataset]}"
  n_param="${N_PARAMS[$dataset]}"
  topk="${TOPK:-${n_param}}"
  if [[ ! -e "${stream}" ]]; then
    echo "missing generated stream: ${stream}" >&2
    exit 1
  fi

  epsilon_arg="$(awk -v epsilon_m="${EPSILON_M}" 'BEGIN { printf "%.10g", epsilon_m }')"
  ss_method="ss[policy=difficulty alpha-req=${ALPHA_REQ} epsilon-m=${epsilon_arg} downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=${AMB_ADJUST} diff-mode=predictive ss-eps=${SS_EPS}]"

  hybrid_frontier_only="hybrid[hyb-head=frontier hyb-tail=difficulty alpha-req=${ALPHA_REQ} epsilon-m=${epsilon_arg} downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=${AMB_ADJUST} diff-mode=predictive ss-eps=${SS_EPS}]"
  hybrid_frontier="hybrid[hyb-head=topn-frontier hyb-tail=difficulty alpha-req=${ALPHA_REQ} epsilon-m=${epsilon_arg} downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=${AMB_ADJUST} diff-mode=predictive ss-eps=${SS_EPS}]"
  hybrid_confirmed="hybrid[hyb-head=confirmed hyb-tail=difficulty alpha-req=${ALPHA_REQ} epsilon-m=${epsilon_arg} downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=${AMB_ADJUST} diff-mode=predictive ss-eps=${SS_EPS}]"
  csv_out="${CSV_DIR}/${dataset}.csv"

  echo "running hybrid ablation: dataset=${dataset}, n=${n_param}, head_policies=confirmed,frontier,topn-frontier"
  command=(
    "${HH_BENCH}"
    "${stream}"
    "${M}"
    "${n_param}"
    "${MEM_KIB}"
    "oracle,${ss_method},${hybrid_confirmed},${hybrid_frontier_only},${hybrid_frontier}"
    --topk "${topk}"
    --csv-out "${csv_out}"
  )
  if [[ "${DRY_RUN}" == "1" ]]; then
    printf '  '
    printf '%q ' "${command[@]}"
    printf '\n'
  else
    "${command[@]}"
  fi
done

if [[ "${DRY_RUN}" == "1" ]]; then
  exit 0
fi

python3 experiments/calibration/summarize_hybrid_ablation.py \
  --root "${CSV_DIR}" \
  > "${OUT_DIR}/summary.csv"

python3 experiments/calibration/plot_hybrid_ablation.py \
  --summary "${OUT_DIR}/summary.csv" \
  --out "${OUT_DIR}/plots"

echo "summary: ${OUT_DIR}/summary.csv"
echo "plots: ${OUT_DIR}/plots"
