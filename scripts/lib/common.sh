#!/usr/bin/env bash

artifact_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
}

require_file() {
  local path="$1"
  [[ -f "${path}" ]] || {
    printf 'missing required file: %s\n' "${path}" >&2
    return 1
  }
}

require_dir() {
  local path="$1"
  [[ -d "${path}" ]] || {
    printf 'missing required directory: %s\n' "${path}" >&2
    return 1
  }
}

timestamp_utc() {
  date -u +%Y%m%dT%H%M%SZ
}

record_environment() {
  local destination="$1"
  {
    printf 'timestamp_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'git_commit=%s\n' "$(git rev-parse HEAD 2>/dev/null || printf unknown)"
    printf 'python=%s\n' "$(python3 --version 2>&1)"
    printf 'cmake=%s\n' "$(cmake --version | head -n 1)"
    printf 'compiler=%s\n' "$(c++ --version | head -n 1)"
    printf 'kernel=%s\n' "$(uname -srmo)"
  } > "${destination}"
}
