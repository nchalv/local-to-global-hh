#!/usr/bin/env bash
set -euo pipefail

# Build the Hybrid-ablation inputs in two explicit phases:
#   global: create and validate one immutable multi-resolution head-and-halo
#           sequence with pressure around the n={100,200,400} thresholds;
#   partition: replay those exact global counts for each n and placement.
#
# Usage:
#   prepare_hybrid_ablation_datasets.sh global
#   prepare_hybrid_ablation_datasets.sh round-robin
#   prepare_hybrid_ablation_datasets.sh milp
#   prepare_hybrid_ablation_datasets.sh partition
#   prepare_hybrid_ablation_datasets.sh all

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GENERATOR_ROOT="${PROJECT_ROOT}/generator"
PYTHON="${PYTHON:-${PROJECT_ROOT}/.hh-venv/bin/python}"
GLOBAL_COUNTS="${GLOBAL_COUNTS:-${GENERATOR_ROOT}/generated/global_counts/hybrid_ablation_head_halo_global_counts.jsonl}"
PHASE="${1:-all}"

if [[ ! -x "${PYTHON}" ]]; then
  PYTHON=python3
fi
if [[ \
  "${PHASE}" != "global" && \
  "${PHASE}" != "round-robin" && \
  "${PHASE}" != "milp" && \
  "${PHASE}" != "partition" && \
  "${PHASE}" != "all" \
]]; then
  echo "usage: $0 [global|round-robin|milp|partition|all]" >&2
  exit 1
fi

cd "${GENERATOR_ROOT}"

if [[ "${PHASE}" == "global" || "${PHASE}" == "all" ]]; then
  if [[ -e "${GLOBAL_COUNTS}" ]]; then
    echo "refusing to overwrite canonical global counts: ${GLOBAL_COUNTS}" >&2
    echo "remove that file explicitly before regenerating it" >&2
    exit 1
  fi

  "${PYTHON}" main.py \
    --run-config config/runs/hybrid_ablation_head_halo_globals.json \
    --global-counts-out "${GLOBAL_COUNTS}" \
    --global-counts-only \
    --no-plots \
    --run-name hybrid_ablation_head_halo_globals

  "${PYTHON}" tools/validate_hybrid_ablation_counts.py "${GLOBAL_COUNTS}"
fi

if [[ \
  "${PHASE}" == "round-robin" || \
  "${PHASE}" == "milp" || \
  "${PHASE}" == "partition" || \
  "${PHASE}" == "all" \
]]; then
  if [[ ! -f "${GLOBAL_COUNTS}" ]]; then
    echo "missing canonical global-count trace: ${GLOBAL_COUNTS}" >&2
    echo "run '$0 global' first" >&2
    exit 1
  fi

  # Round robin is independent of n, so generate it once and reuse that stream
  # at all three evaluator thresholds. MILP placement remains n-specific.
  if [[ "${PHASE}" == "round-robin" || "${PHASE}" == "partition" || "${PHASE}" == "all" ]]; then
    round_robin_dir="${GENERATOR_ROOT}/generated/runs/hybrid_ablation_head_halo_round_robin"
    if [[ -e "${round_robin_dir}" ]]; then
      echo "refusing to create suffixed run; remove existing directory first: ${round_robin_dir}" >&2
      exit 1
    fi
  fi

  if [[ "${PHASE}" == "milp" || "${PHASE}" == "partition" || "${PHASE}" == "all" ]]; then
    for n in 100 200 400; do
      run_dir="${GENERATOR_ROOT}/generated/runs/hybrid_ablation_head_halo_milp_certificate_adversary_n${n}"
      if [[ -e "${run_dir}" ]]; then
        echo "refusing to create suffixed run; remove existing directory first: ${run_dir}" >&2
        exit 1
      fi
    done
  fi

  if [[ "${PHASE}" == "round-robin" || "${PHASE}" == "partition" || "${PHASE}" == "all" ]]; then
    "${PYTHON}" main.py \
      --run-config config/runs/calibration_synthetic_round_robin_n400.json \
      --real-counts "${GLOBAL_COUNTS}" \
      --no-plots \
      --run-name hybrid_ablation_head_halo_round_robin
  fi

  if [[ "${PHASE}" == "milp" || "${PHASE}" == "partition" || "${PHASE}" == "all" ]]; then
    for n in 100 200 400; do
      run_name="hybrid_ablation_head_halo_milp_certificate_adversary_n${n}"

      "${PYTHON}" main.py \
        --run-config "config/runs/calibration_synthetic_milp_certificate_adversary_n${n}.json" \
        --real-counts "${GLOBAL_COUNTS}" \
        --no-plots \
        --run-name "${run_name}"
    done
  fi
fi

if [[ "${PHASE}" == "global" ]]; then
  echo "Prepared and validated Hybrid-ablation global counts: ${GLOBAL_COUNTS}"
elif [[ "${PHASE}" == "round-robin" ]]; then
  echo "Partitioned canonical Hybrid-ablation counts with round robin."
elif [[ "${PHASE}" == "milp" ]]; then
  echo "Partitioned canonical Hybrid-ablation counts with all three MILP policies."
elif [[ "${PHASE}" == "partition" ]]; then
  echo "Partitioned canonical Hybrid-ablation counts for all n and placements."
else
  echo "Prepared canonical counts and all six Hybrid-ablation datasets."
fi
