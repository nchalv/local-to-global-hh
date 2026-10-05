#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REFERENCE_ROOT="${ROOT}/results/reference"

[[ -d "${REFERENCE_ROOT}" ]] || {
  echo "missing reference-output directory: ${REFERENCE_ROOT}" >&2
  exit 1
}

cd "${REFERENCE_ROOT}"
find . -type f ! -name SHA256SUMS -print0 \
  | sort -z \
  | xargs -0 sha256sum > SHA256SUMS
echo "updated ${REFERENCE_ROOT}/SHA256SUMS"
