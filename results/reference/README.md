# Reference Results

This directory is the entry point for committed experimental evidence. Run
`./artifact verify-results` before inspecting the bundles. The checksum
manifest covers every committed CSV, plot, configuration snapshot, and bundle
description below.

The repository does not redistribute CAIDA packet data, raw keys, expanded
streams, or full benchmark logs. The committed files therefore support result
inspection and plot regeneration. Full benchmark reproduction requires the
normalized CAIDA inputs described in `docs/DATASETS.md`.

## Recommended reading order

1. **Controller policy selection**
   (`supplementary/temporal-controller/`): compares four downward-sizing
   policies under controlled changes in global stream difficulty and fixed
   round-robin placement, with guarded margin comfort identified as the
   selected policy.
2. **Per-window adaptive behavior**
   (`supplementary/window-trajectories/`): contrasts Hybrid's changing
   allocation with HeavyLocker's fixed allocation on CAIDA-B.
3. **Placement structure**
   (`supplementary/partition-hardness/`): explains why balanced round-robin
   placement can be harder for partition-local Space-Saving summaries.
4. **Coordinator and control-plane costs**
   (`supplementary/systems/`): reports serial and parallel reduction time and
   exact-head delta effectiveness.

Each bundle contains a README that states the question, inputs, outputs,
interpretation, and regeneration command. These supplementary diagnostics
complement the primary comparison results with additional diagnostic detail.

## Integrity

```bash
./artifact verify-results
```

The manifest can be refreshed after an intentional update with
`scripts/refresh_reference_checksums.sh`.
