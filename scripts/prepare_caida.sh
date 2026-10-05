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
python3 "$ROOT/scripts/validate_counts.py" "$COUNTS_A" \
  --expected-windows 200 --label CAIDA-A
python3 "$ROOT/scripts/validate_counts.py" "$COUNTS_B" \
  --expected-windows 200 --label CAIDA-B

cp -f "$COUNTS_A" "$ROOT/generated/counts/caida-a.jsonl"
cp -f "$COUNTS_B" "$ROOT/generated/counts/caida-b.jsonl"
sha256sum \
  "$ROOT/generated/counts/caida-a.jsonl" \
  "$ROOT/generated/counts/caida-b.jsonl" \
  > "$ROOT/generated/counts/SHA256SUMS"

cat > "$ROOT/generated/counts/README.txt" <<'EOF'
caida-a.jsonl: 200 five-second windows used for configuration and ablation.
caida-b.jsonl: disjoint 200 five-second windows used for final evaluation.
These files are user-supplied and are not distributed with the artifact.
EOF

echo 'Validated CAIDA counts staged under generated/counts.'
echo 'Checksums: generated/counts/SHA256SUMS'
