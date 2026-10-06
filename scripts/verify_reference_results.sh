#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REFERENCE_ROOT="${ROOT}/results/reference"

[[ -f "${REFERENCE_ROOT}/SHA256SUMS" ]] || {
  echo "missing reference checksum manifest: ${REFERENCE_ROOT}/SHA256SUMS" >&2
  exit 1
}

cd "${REFERENCE_ROOT}"
sha256sum --check SHA256SUMS
echo "Reference results verified. Start with results/reference/README.md."
