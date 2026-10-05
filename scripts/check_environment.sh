#!/usr/bin/env bash
set -euo pipefail

command -v python3 >/dev/null || { echo 'missing: python3' >&2; exit 1; }
command -v cmake >/dev/null || { echo 'missing: cmake' >&2; exit 1; }
command -v c++ >/dev/null || { echo 'missing: C++ compiler' >&2; exit 1; }
command -v git >/dev/null || { echo 'missing: git' >&2; exit 1; }
command -v gzip >/dev/null || { echo 'missing: gzip' >&2; exit 1; }

python3 - <<'PY'
import importlib.util
required = ["numpy", "scipy", "matplotlib"]
missing = [name for name in required if importlib.util.find_spec(name) is None]
if missing:
    raise SystemExit("missing Python packages: " + ", ".join(missing))
PY

echo 'environment checks passed'
