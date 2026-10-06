# Results

- `reference/` contains committed, checksum-protected evidence and a guided
  reading order. Start with `reference/README.md`.
- `runs/` is created locally by artifact workflows and is ignored by Git. Each
  run contains its configuration, logs, per-window CSV, and derived outputs.

Use `./artifact verify-results` to validate the committed reference bundle.
