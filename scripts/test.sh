#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"${ROOT}/scripts/check_environment.sh"
"${ROOT}/scripts/build.sh"
ctest --test-dir "${ROOT}/evaluator/build" --output-on-failure
PYTHONPATH="${ROOT}/generator" python3 -m unittest discover \
  -s "${ROOT}/generator/tests" -p 'test_*.py' -v
