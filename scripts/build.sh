#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$ROOT/evaluator" -B "$ROOT/evaluator/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/evaluator/build" --target hh_bench hh_run hh_tests -j2
