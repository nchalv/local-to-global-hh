#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib/common.sh"

"${ROOT}/scripts/check_environment.sh"
[[ -x "${ROOT}/evaluator/build/hh_bench" ]] || "${ROOT}/scripts/build.sh"

stamp="$(timestamp_utc)"
result_dir="${ROOT}/results/runs/smoke-${stamp}"
mkdir -p "${result_dir}"
record_environment "${result_dir}/environment.txt"

cd "${ROOT}"
python3 generator/main.py \
  --run-config generator/config/smoke/run.json \
  --run-name "smoke-${stamp}" \
  --no-plots | tee "${result_dir}/generator.log"

generator_dir="$(sed -n 's/^Generator run directory: //p' "${result_dir}/generator.log" | tail -n 1)"
[[ -n "${generator_dir}" ]] || {
  echo 'generator did not report its output directory' >&2
  exit 1
}
stream="${generator_dir}/streams/stream.json.gz"
require_file "${stream}"

methods='oracle,ss[policy=static q=2n reducer=streaming],hl[hl-w=16 hl-d=6 hl-L=0.7 hl-lossy=2],ss[policy=difficulty reducer=streaming alpha-req=0.95 epsilon-m=0.15 downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=off diff-mode=predictive ss-eps=per-item],hybrid[hyb-head=topn-frontier hyb-tail=difficulty reducer=streaming alpha-req=0.95 epsilon-m=0.40 downward-probing=on probe-residual-guard=on probe-strategy=comfort probe-pressure-gate=0.5 amb-adjust=off diff-mode=predictive ss-eps=per-item]'

printf '%s\n' "${methods}" > "${result_dir}/methods.txt"
"${ROOT}/evaluator/build/hh_bench" \
  "${stream}" 4 50 128 "${methods}" \
  --topk 50 \
  --csv-out "${result_dir}/windows.csv" | tee "${result_dir}/benchmark.log"

python3 "${ROOT}/scripts/validate_smoke.py" \
  "${result_dir}/windows.csv" \
  --expected-windows 4 \
  --expected-methods 4 | tee "${result_dir}/validation.txt"

ln -sfn "$(basename "${result_dir}")" "${ROOT}/results/runs/smoke-latest"
printf 'Smoke test passed. Results: %s\n' "${result_dir}"
