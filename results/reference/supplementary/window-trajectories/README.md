# Per-Window Error and Memory Trajectories

## What is compared

These plots use the final CAIDA-B comparison at `m=100` input partitions and
heavy-hitter resolution `n=600`. Every method processes the same 200 five-second
windows under either round-robin or visibility-suppression placement. The
displayed HeavyLocker point uses the paper's selected configuration
`(d=6, w=0.32n, L=0.7, lossy=2)`. Hybrid uses `epsilon_M=0.40`, `alpha=0.95`,
per-item Space-Saving errors, and the streaming reducer.

Each figure places per-window average relative error (ARE) on the left axis and
mean resident per-partition memory on the right. Solid lines denote ARE and
dashed lines denote memory. Window 0 is the common controller-initialization
window and is omitted from the trajectory display. HeavyLocker and Hybrid use
identical axes within each placement.

Resident memory includes algorithm state and live key-identity state. Thus,
HeavyLocker's fixed cell array produces an almost flat, rather than perfectly
constant, memory trace.

## How the figures were produced

The committed CSVs are the per-window outputs of the final fair-accounting
evaluation. The plotting command is:

```bash
python3 experiments/end_to_end/plot_caida_stepwise.py \
  --root results/reference/supplementary/window-trajectories \
  --n 600 \
  --out results/reference/supplementary/window-trajectories/caida_stepwise.pdf
```

It produces one PDF and PNG for each method-placement pair.

## What the figures show

| Placement | Method | Mean plotted ARE | Mean memory | Memory range |
|---|---|---:|---:|---:|
| Round robin | HeavyLocker | 0.711% | 101.4 KiB | 101.0-102.2 KiB |
| Round robin | Hybrid | 1.225% | 102.2 KiB | 77.1-140.6 KiB |
| Visibility suppression | HeavyLocker | 1.694% | 98.3 KiB | 92.0-101.4 KiB |
| Visibility suppression | Hybrid | 0.768% | 72.8 KiB | 65.2-126.1 KiB |

**Round robin.** HeavyLocker has lower mean ARE at nearly the same mean memory.
This is the expected favorable case for its local admission mechanism. The
Hybrid trace nevertheless demonstrates the behavior hidden by aggregate plots:
its resident state contracts and expands across windows rather than reserving a
fixed allocation.

**Visibility suppression.** HeavyLocker's error rises when globally important
keys are deprived of strong local evidence. Hybrid attains both lower mean ARE
and lower mean memory, while increasing its allocation during difficult
windows. The result illustrates why adaptive state and deterministic
coordinator-side evidence become more valuable when local visibility is
unreliable.

The comparison is descriptive rather than a claim that one placement is
universally representative. Its purpose is to expose temporal behavior behind
the aggregate configuration curves. The reported means are arithmetic means of
the 199 plotted per-window values after the common initialization window is
removed. They are therefore not the pooled error aggregates used by the
paper's end-to-end comparison.
