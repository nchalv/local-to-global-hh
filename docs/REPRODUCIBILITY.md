# Reproducibility Workflow

Run commands from the artifact root.

1. Install the Python dependencies and verify the C++ toolchain:

   ```bash
   python3 -m venv .hh-venv
   . .hh-venv/bin/activate
   pip install -r requirements.txt
   ./scripts/check_environment.sh
   ```

2. Build the evaluator:

   ```bash
   ./scripts/build.sh
   ```

3. Prepare normalized CAIDA counts according to `docs/DATASETS.md`.

4. Generate the evaluator-ready streams with the versioned generator
   configurations. The final paper-facing commands will be provided by
   `scripts/prepare_caida.sh` once the external data path is set.

5. Run calibration and final comparisons through the staged scripts under
   `scripts/`. Each script writes a configuration snapshot, logs, CSV summaries,
   and plots beneath `results/`.

Every stage is independently rerunnable. Existing outputs are never used as
implicit input; paths and configuration values are recorded in each run
directory.
