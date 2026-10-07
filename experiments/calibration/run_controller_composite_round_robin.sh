#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HH_BENCH="${PROJECT_ROOT}/evaluator/build/hh_bench"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${OUT_DIR:-${PROJECT_ROOT}/results/runs/temporal-controller-${STAMP}}"
RUN_NAME="controller_composite_round_robin_n200_${STAMP}"
M="${M:-20}"
N_PARAM="${N_PARAM:-200}"
MEM_KIB="${MEM_KIB:-128}"
EPSILON_M="${EPSILON_M:-0.15}"
ALPHA_REQ="${ALPHA_REQ:-0.95}"
BUILD="${BUILD:-1}"

mkdir -p "${OUT_DIR}"

if [[ "${BUILD}" == "1" ]]; then
  cmake -S "${PROJECT_ROOT}/evaluator" -B "${PROJECT_ROOT}/evaluator/build" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "${PROJECT_ROOT}/evaluator/build" --target hh_bench -j2
fi

GLOBAL_COUNTS="${OUT_DIR}/global_counts.jsonl"
GENERATOR_LOG="${OUT_DIR}/generator.log"

cd "${PROJECT_ROOT}"
python3 generator/main.py \
  --run-config generator/config/runs/controller_composite_round_robin_n200.json \
  --run-name "${RUN_NAME}" \
  --global-counts-out "${GLOBAL_COUNTS}" \
  --no-plots | tee "${GENERATOR_LOG}"

GENERATOR_DIR="$(sed -n 's/^Generator run directory: //p' "${GENERATOR_LOG}" | tail -n 1)"
if [[ -z "${GENERATOR_DIR}" ]]; then
  echo "generator did not report its run directory" >&2
  exit 1
fi
if [[ "${GENERATOR_DIR}" != /* ]]; then
  GENERATOR_DIR="${PROJECT_ROOT}/${GENERATOR_DIR}"
fi

STREAM="${GENERATOR_DIR}/streams/stream_controller_composite_rr_n200_m20.json.gz"
if [[ ! -f "${STREAM}" ]]; then
  echo "missing generated stream: ${STREAM}" >&2
  exit 1
fi

BASELINE_CSV="${OUT_DIR}/static_baselines.csv"
BASELINE_METHODS='oracle,ss[policy=static q=n reducer=streaming],ss[policy=static q=2n reducer=streaming],ss[policy=static q=4n reducer=streaming]'

"${HH_BENCH}" \
  "${STREAM}" "${M}" "${N_PARAM}" "${MEM_KIB}" "${BASELINE_METHODS}" \
  --topk "${N_PARAM}" \
  --csv-out "${BASELINE_CSV}" | tee "${OUT_DIR}/static_baselines.log"

CONTROLLER_DIR="${OUT_DIR}/controllers"
OUT_DIR="${CONTROLLER_DIR}" \
DATASETS="controller_composite_round_robin_n200" \
STREAM_OVERRIDE="${STREAM}" \
N_PARAM_OVERRIDE="${N_PARAM}" \
M="${M}" \
MEM_KIB="${MEM_KIB}" \
TOPK="${N_PARAM}" \
EPSILON_M="${EPSILON_M}" \
ALPHA_REQ="${ALPHA_REQ}" \
SS_EPS=per-item \
CLEAN_OUTPUT=1 \
BUILD=0 \
bash experiments/calibration/run_temporal_controller_ablation.sh \
  | tee "${OUT_DIR}/controllers.log"

python3 experiments/calibration/analyze_controller_composite.py \
  --scenario generator/config/scenarios/controller_composite_n200.json \
  --global-counts "${GLOBAL_COUNTS}" \
  --baselines "${BASELINE_CSV}" \
  --controller-csv "${CONTROLLER_DIR}/csv" \
  --n "${N_PARAM}" \
  --epsilon-m "${EPSILON_M}" \
  --out "${OUT_DIR}/analysis" \
  --strict

cat > "${OUT_DIR}/configuration.txt" <<EOF
experiment=controller_composite_round_robin
generated_run=${GENERATOR_DIR#${PROJECT_ROOT}/}
stream=${STREAM#${PROJECT_ROOT}/}
m=${M}
n=${N_PARAM}
memory_kib=${MEM_KIB}
epsilon_m=${EPSILON_M}
alpha=${ALPHA_REQ}
placement=round_robin
windows=160
EOF

printf 'Controller composite pilot complete: %s\n' "${OUT_DIR}"
printf 'Suitability report: %s\n' "${OUT_DIR}/analysis/acceptance.txt"
