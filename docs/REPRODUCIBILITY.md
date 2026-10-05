# Reproducibility Workflow

Run commands from the artifact root.

## Quick validation

Install the Python dependencies and run the self-contained validation:

   ```bash
   python3 -m venv .hh-venv
   . .hh-venv/bin/activate
   pip install -r requirements.txt
   ./artifact test
   ./artifact smoke
   ```

The smoke workload is deterministic and uses bundled configurations. Its run
directory contains the exact method string, environment metadata, generator
log, benchmark log, raw per-window CSV, and validation report. The symbolic
link `results/runs/smoke-latest` identifies the newest run.

The supplementary temporal-controller study is also self-contained:

```bash
./artifact temporal-controller
```

It generates four deterministic difficulty schedules and evaluates the four
downward-sizing policies documented in `docs/SUPPLEMENTARY_RESULTS.md`.

## CAIDA-dependent reproduction

1. Prepare normalized CAIDA counts according to `docs/DATASETS.md`, then stage
   them through the validated data boundary:

   ```bash
   CAIDA_COUNTS_A=/path/to/caida-a.jsonl \
   CAIDA_COUNTS_B=/path/to/caida-b.jsonl \
   ./artifact prepare-caida
   ```

The long-running paper and supplementary matrices are listed in
`docs/EXPERIMENTS.md`. Their orchestration is kept separate from the quick
validation path because visibility-suppression generation at large partition
counts can require several hours.

5. Run calibration and final comparisons through the staged scripts under
   `scripts/`. Each script writes a configuration snapshot, logs, CSV summaries,
   and plots beneath `results/`.

Every result directory records its configuration, logs, and environment. Raw
per-window CSVs remain separate from summaries and plots so aggregate claims
can be independently recomputed.
