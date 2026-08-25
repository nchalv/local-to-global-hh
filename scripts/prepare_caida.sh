#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COUNTS_A="${CAIDA_COUNTS_A:-}"
COUNTS_B="${CAIDA_COUNTS_B:-}"

if [[ -z "$COUNTS_A" || -z "$COUNTS_B" ]]; then
  cat >&2 <<'EOF'
Set CAIDA_COUNTS_A and CAIDA_COUNTS_B to normalized JSONL count files.
The artifact does not redistribute CAIDA data.
EOF
  exit 2
fi

for counts in "$COUNTS_A" "$COUNTS_B"; do
  [[ -f "$counts" ]] || { echo "missing counts file: $counts" >&2; exit 1; }
done

mkdir -p "$ROOT/generated/counts" "$ROOT/generated/runs"
cp -f "$COUNTS_A" "$ROOT/generated/counts/caida_a.jsonl"
cp -f "$COUNTS_B" "$ROOT/generated/counts/caida_b.jsonl"
echo 'Normalized CAIDA counts staged under generated/counts.'
echo 'Partition-specific generation commands are kept explicit in the run scripts.'
