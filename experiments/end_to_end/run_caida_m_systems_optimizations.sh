#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HH_BENCH="${PROJECT_ROOT}/evaluator/build/hh_bench"
OUT_ROOT="${OUT_ROOT:-${PROJECT_ROOT}/experiments/end_to_end/caida_m_systems_optimizations}"
M_VALUES=(${M_VALUES:-20 50 100 250 500 1000})
REPETITIONS="${REPETITIONS:-5}"
PARALLELISM="${PARALLELISM:-8}"
N_PARAM="${N_PARAM:-400}"
MEM_KIB="${MEM_KIB:-128}"
BUILD="${BUILD:-1}"
CLEAN_OUTPUT="${CLEAN_OUTPUT:-1}"
TASKSET_CPUS="${TASKSET_CPUS:-}"
RUN_CAIDA="${RUN_CAIDA:-1}"
RUN_SYNTHETIC_DELTA="${RUN_SYNTHETIC_DELTA:-1}"
DELTA_ONLY="${DELTA_ONLY:-0}"
RUNTIME_ONLY="${RUNTIME_ONLY:-0}"

cd "${PROJECT_ROOT}"

if [[ "${DELTA_ONLY}" == "1" && "${RUNTIME_ONLY}" == "1" ]]; then
  echo "DELTA_ONLY and RUNTIME_ONLY are mutually exclusive" >&2
  exit 1
fi

if [[ "${BUILD}" == "1" ]]; then
  cmake -S evaluator -B evaluator/build -DCMAKE_BUILD_TYPE=Release
  cmake --build evaluator/build --target hh_bench -j2
fi

if [[ "${CLEAN_OUTPUT}" == "1" ]]; then
  rm -rf "${OUT_ROOT}"
fi
mkdir -p "${OUT_ROOT}"

hl_width=$((32 * N_PARAM / 100))
controller="alpha-req=0.95 epsilon-m=0.40 downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=off diff-mode=predictive ss-eps=per-item"
serial_delta="hybrid[hyb-head=topn-frontier hyb-tail=difficulty reducer=streaming ${controller}]"
parallel_delta="hybrid[hyb-head=topn-frontier hyb-tail=difficulty reducer=parallel-streaming ${controller}]"
serial_full="hybrid[hyb-head=topn-frontier hyb-tail=difficulty reducer=streaming head-delta-eta=0 ${controller}]"
hl="hl[hl-w=${hl_width} hl-d=6 hl-L=0.7 hl-lossy=2]"

run_bench() {
  if [[ -n "${TASKSET_CPUS}" ]]; then
    taskset -c "${TASKSET_CPUS}" "$@"
  else
    "$@"
  fi
}

if [[ "${RUN_CAIDA}" == "1" ]]; then
for m in "${M_VALUES[@]}"; do
  stream="${PROJECT_ROOT}/generator/generated/runs/caida_20180315_133230_200w_5s_dirA_round_robin_m${m}/streams/stream_caida_200w_5s_n400_m${m}"
  if [[ ! -d "${stream}" ]]; then
    echo "missing stream directory: ${stream}" >&2
    exit 1
  fi

  run_dir="${OUT_ROOT}/m${m}"
  mkdir -p "${run_dir}/csv"
  {
    printf 'stream=%s\n' "${stream}"
    printf 'm=%s\n' "${m}"
    printf 'n=%s\n' "${N_PARAM}"
    printf 'repetitions=%s\n' "${REPETITIONS}"
    printf 'parallelism=%s\n' "${PARALLELISM}"
    printf 'runtime_only=%s\n' "${RUNTIME_ONLY}"
    printf 'taskset_cpus=%s\n' "${TASKSET_CPUS}"
    printf 'hl_width=%s\n' "${hl_width}"
    printf 'hybrid_epsilon_m=0.40\n'
    printf 'head_delta_eta=0.80\n'
    printf 'head_checkpoint=32\n'
  } > "${run_dir}/configuration.txt"

  for ((rep = 1; rep <= REPETITIONS; ++rep)); do
    if [[ "${RUNTIME_ONLY}" == "1" ]]; then
      # Time each reducer in a fresh process. Rotate launch order across trials
      # to distribute thermal and cache-state effects uniformly.
      case $((rep % 3)) in
        1) runtime_order=(hl serial parallel) ;;
        2) runtime_order=(serial parallel hl) ;;
        0) runtime_order=(parallel hl serial) ;;
      esac
      for runtime_method in "${runtime_order[@]}"; do
        case "${runtime_method}" in
          hl) method="${hl}" ;;
          serial) method="${serial_delta}" ;;
          parallel) method="${parallel_delta}" ;;
        esac
        csv="${run_dir}/csv/repeat_$(printf '%02d' "${rep}")_${runtime_method}.csv"
        log="${run_dir}/repeat_$(printf '%02d' "${rep}")_${runtime_method}.log"
        printf 'm=%s repetition=%s/%s method=%s\n' \
          "${m}" "${rep}" "${REPETITIONS}" "${runtime_method}"
        run_bench "${HH_BENCH}" \
          "${stream}" \
          "${m}" \
          "${N_PARAM}" \
          "${MEM_KIB}" \
          "oracle,${method}" \
          --reducer-workers "${PARALLELISM}" \
          --csv-out "${csv}" \
          --topk "${N_PARAM}" \
          > "${log}"
      done
      continue
    elif [[ "${DELTA_ONLY}" == "1" ]] && ((rep % 2 == 1)); then
      methods="oracle,${serial_delta},${serial_full}"
    elif [[ "${DELTA_ONLY}" == "1" ]]; then
      methods="oracle,${serial_full},${serial_delta}"
    elif ((rep % 2 == 1)); then
      methods="oracle,${hl},${serial_delta},${parallel_delta},${serial_full}"
    else
      methods="oracle,${hl},${parallel_delta},${serial_full},${serial_delta}"
    fi
    csv="${run_dir}/csv/repeat_$(printf '%02d' "${rep}").csv"
    log="${run_dir}/repeat_$(printf '%02d' "${rep}").log"
    printf 'm=%s repetition=%s/%s\n' "${m}" "${rep}" "${REPETITIONS}"
    run_bench "${HH_BENCH}" \
      "${stream}" \
      "${m}" \
      "${N_PARAM}" \
      "${MEM_KIB}" \
      "${methods}" \
      --reducer-workers "${PARALLELISM}" \
      --csv-out "${csv}" \
      --topk "${N_PARAM}" \
      > "${log}"
  done
done
fi

if [[ "${RUN_SYNTHETIC_DELTA}" == "1" && "${RUNTIME_ONLY}" != "1" ]]; then
  synthetic_stream="${PROJECT_ROOT}/generator/generated/runs/dynamic_mixed_heavy_tail_200w_round_robin_m100/streams/stream_dynamic_mixed_heavy_tail_200w_m100"
  if [[ ! -d "${synthetic_stream}" ]]; then
    echo "missing synthetic stream directory: ${synthetic_stream}" >&2
    exit 1
  fi

  run_dir="${OUT_ROOT}/synthetic_delta"
  mkdir -p "${run_dir}/csv"
  {
    printf 'stream=%s\n' "${synthetic_stream}"
    printf 'm=100\n'
    printf 'n=%s\n' "${N_PARAM}"
    printf 'repetitions=%s\n' "${REPETITIONS}"
    printf 'head_delta_eta=0.80\n'
    printf 'head_checkpoint=32\n'
  } > "${run_dir}/configuration.txt"

  for ((rep = 1; rep <= REPETITIONS; ++rep)); do
    if ((rep % 2 == 1)); then
      methods="oracle,${serial_delta},${serial_full}"
    else
      methods="oracle,${serial_full},${serial_delta}"
    fi
    csv="${run_dir}/csv/repeat_$(printf '%02d' "${rep}").csv"
    log="${run_dir}/repeat_$(printf '%02d' "${rep}").log"
    printf 'synthetic-delta repetition=%s/%s\n' "${rep}" "${REPETITIONS}"
    run_bench "${HH_BENCH}" \
      "${synthetic_stream}" \
      100 \
      "${N_PARAM}" \
      "${MEM_KIB}" \
      "${methods}" \
      --reducer-workers "${PARALLELISM}" \
      --csv-out "${csv}" \
      --topk "${N_PARAM}" \
      > "${log}"
  done
fi

summary_args=(
  --root "${OUT_ROOT}"
  --reducer-out "${OUT_ROOT}/reducer_runtime_summary.csv"
  --delta-out "${OUT_ROOT}/head_delta_summary.csv"
)
if [[ "${DELTA_ONLY}" == "1" ]]; then
  summary_args+=(--delta-only)
elif [[ "${RUNTIME_ONLY}" == "1" ]]; then
  summary_args+=(--runtime-only)
fi
python3 experiments/end_to_end/summarize_caida_m_systems_optimizations.py \
  "${summary_args[@]}"

if [[ "${DELTA_ONLY}" != "1" && "${RUNTIME_ONLY}" != "1" ]]; then
  python3 experiments/end_to_end/plot_caida_m_systems_optimizations.py \
    --reducer-summary "${OUT_ROOT}/reducer_runtime_summary.csv" \
    --delta-summary "${OUT_ROOT}/head_delta_summary.csv" \
    --out "${OUT_ROOT}/plots"
  echo "Plots: ${OUT_ROOT}/plots"
fi
if [[ "${DELTA_ONLY}" != "1" ]]; then
  echo "Reducer summary: ${OUT_ROOT}/reducer_runtime_summary.csv"
fi
if [[ "${RUNTIME_ONLY}" != "1" ]]; then
  echo "Head-delta summary: ${OUT_ROOT}/head_delta_summary.csv"
fi
