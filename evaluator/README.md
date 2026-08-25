# Heavyheads Experimental Harness

This repo contains the C++ harness used to reproduce the experiments from **“Adaptive Frequency Estimation for Load-Balanced Distributed Stream Processing.”** It simulates a load-balanced distributed pipeline, instantiates several heavy-hitter sketches, and applies the sizing policies described in the paper.

## Layout
- `apps/hh_run.cpp` – single-method driver for interactive runs and debugging.
- `apps/hh_bench.cpp` – batch benchmark runner that evaluates multiple methods against the oracle and reports window-aggregated metrics.
- `src/` and `include/` – sketch implementations (SpaceSaving, HeavyLocker, CHK), hybrid exact/approx head–tail sketch, and policy logic for adaptive sizing.
- `tests/` – light sanity checks for the sketch primitives.

## Input format
Streams can be provided in either of these forms:

- Single gzip-compressed JSON file (original format).
- Directory containing one gzip-compressed JSON file per window (new format), e.g. `stream_window/window_000000.json.gz`, `stream_window/window_000001.json.gz`, ...

Window IDs are zero-based. The evaluator preserves numeric window IDs from
single-file streams and infers zero-based IDs from per-window filenames such as
`window_000000.json.gz`.

### Single-file format
The original format is a gzip-compressed JSON with a nested structure per window and partition:

```json
{
  "0": {                          // window id (string, numeric preferred)
    "0": ["a", ["b", 3]],         // partition id -> list of keys or [key, weight]
    "1": [["a", 2], "c"]
  },
  "1": { "0": ["d"] }
}
```

`JsonGzNestedReader` preserves the natural numeric ordering of windows and partitions; any non-numeric keys fall back to lexical order and default to index 0.

### Directory-per-window format
If the input path is a directory, the reader loads all `*.json.gz` files in lexical filename order.

Each file may be either:
- The same nested format as above (`{window:{partition:[...]}}`), or
- A single-window partition object (`{partition:[...]}`), in which case the window id is inferred from the filename `window_<num>.json.gz` when possible (otherwise lexical file order is used).

## Building
```sh
cmake -S evaluator -B evaluator/build -DCMAKE_BUILD_TYPE=Release
cmake --build evaluator/build
```

For quick local rebuilds there is also an evaluator-local `makefile`:
- `make -C evaluator all` builds the binaries into `evaluator/build`.
- `make -C evaluator clean` drops `evaluator/build` for a fresh out-of-tree build.

## Requirements
- C++17 compiler (gcc/clang).
- CMake >= 3.16.
- zlib development headers (for gzip via `zstr`).
- Standard POSIX environment (pthread); no external runtime services required.

Executables land in `evaluator/build/` (e.g., `hh_run`, `hh_bench`, `test_basic`).

## Running `hh_run`
Single-method exploration with per-window telemetry:
```sh
./hh_run <stream.json.gz|stream_dir> <m> <n_param> <memKiB_each> <method:oracle|ss|hl|chk|hybrid> \
  [--policy difficulty|static] [--alpha-req A] [--epsilon-m EPS] \
  [--hyb-tail n|2n|difficulty] [--head-delta-eta ETA] \
  [--head-checkpoint WINDOWS] [--q kN]
```
- `m`: number of partitions; `n_param`: global HH denominator (strict threshold `floor(N/n_param)+1`, i.e., `p(k) > 1/n`).
- `memKiB_each`: memory budget per partition; `--q kN` sets static SS capacity to `k * n_param`.
- Policies (SpaceSaving only): `difficulty` applies the certificate-pressure controller; `static` holds `q` fixed. The adaptive controller raises capacity immediately after a service-margin violation and can probe downward only after repeated sufficient observations.
- Space-Saving exports per-item admission-error certificates by default. Pass `--ss-eps max-sketch` only for the coarser diagnostic mode that exports one shared maximum admission error per sketch.
- `hybrid` uses an exact head (`q_e`) plus SS tail (`q_a`); the tail can be fixed or controlled with the same difficulty policy via `--hyb-tail`.
- Hybrid workers retain the ordered head dictionary across windows. The default
  control protocol sends a generation-checked delta when its encoded size is
  at most `0.8` of a full dictionary and sends a full checkpoint every 32
  generations. `head-delta-eta` and `head-checkpoint` override these defaults.

Raw input bytes are available only during the synchronous sketch update. An
admission callback binds a newly resident 128-bit identifier to those bytes,
and a retirement callback releases the binding when the identifier is evicted.
The arena compacts reclaimed byte ranges amortized over later retirements, so
the persistent key dictionary is bounded by resident sketch state rather than
by the number of distinct keys observed in the window.

The binding index shared by Space-Saving, Hybrid tails, and HeavyLocker is a
flat open-addressed table over the full 128-bit identifiers. Slots are
contiguous, occupancy states use two packed bits per slot, and deleted slots
are rebuilt before they lengthen probe chains excessively. This changes only
the representation of the identity dictionary: lookup and collision semantics
remain exact. Reported key memory includes the table's allocated slot and state
buffers, the raw-key arena, and the `ArenaMap` object itself; it does not use an
allocator-dependent estimate of node overhead.

The Hybrid exact head is a replicated sorted identifier dictionary with a dense
local count array. Each worker owns its dictionary copy; the design assumes no
shared memory. Distributing the same ordered promoted set gives every worker a
consistent report-slot assignment while avoiding a node-based hash table for
the exact counters. During report preparation, each worker independently orders
its residual-tail records by identifier. The coordinator therefore performs
the streaming multiway merge directly and does not serially sort all worker
reports.

The program prints per-window global counts, certified bounds, and the next `q` suggestion when an adaptive policy is active.

## Running `hh_bench`
Batch evaluation against the oracle (exact counts):
```sh
./hh_bench <stream.json.gz|stream_dir> <m> <n_param> <memKiB_each> <methods_csv> \
  [--csv-out results.csv] [--topk K] [--reducer-workers P] [policy flags...]
```
`methods_csv` can include multiple entries (e.g., `oracle,ss,hybrid`). The first `oracle` run is treated as ground truth; subsequent methods reuse the same stream ordering. Metrics reported per window and as averages:
- Heavy-hitter precision/recall at the paper’s HH threshold 𝑝(𝑘) > 1/𝑛 (implemented as `f(k) ≥ floor(N/n_param)+1` with your supplied `n_param` = 𝑛).
- Heavy-hitter F1 at the same threshold.
- Average absolute/relative error on every oracle heavy hitter, including
  false negatives. A reported key below the HH threshold keeps its estimated
  count for the error calculation; an absent oracle HH is assigned estimate
  zero.
- Optional top-`K` overlap if `--topk` is supplied.

Space-Saving and Hybrid runs can opt into the specialized serial or parallel
streaming reducers with `reducer=streaming` or `reducer=parallel-streaming`.
Report construction is executed serially by the replay harness for
reproducibility, but each worker is timed independently:
`report_prepare_ms` is the maximum per-worker preparation latency.
`reduce_ms` measures only coordinator reduction, and `aggregation_ms` is the
sum of the worker critical-path preparation latency, coordinator reduction,
and controller processing.

```sh
"oracle,ss[policy=difficulty reducer=streaming],hybrid[hyb-head=topn-frontier hyb-tail=difficulty reducer=streaming]"
```

The default `reducer=hash` path constructs a full-union hash table. The generic
streaming path aggregates the shared exact head in dense slots and performs a
key-sorted multiway merge over worker tail reports, discarding certified
negatives immediately. Hybrid `reducer=streaming` uses a coordinated variant:
active exact-head reports are decoded directly into dense shared-dictionary
slots, while only the disjoint residual tail enters the sorted multiway merge.
Each tail aggregate contributes once to both the published certificate frontier
and residual-tail telemetry. Certificate envelopes are evaluated before raw
keys are materialized, and a surviving key is resolved only once even when it
belongs to both frontiers. Only the bounded top set and certificate frontiers
survive the merge. Exact inflation and hidden-mass components are attached to
retained residual keys, so the sizing controller does not reconstruct worker
incidence maps or copy residual snapshots. These paths preserve estimates and
certificates and do not reduce communication volume. Worker-side sorting work
is included in reduction time but not in the coordinator working-set model.

`reducer=parallel-streaming` additionally partitions the ordered 128-bit
fingerprint space into disjoint ranges and merges those ranges concurrently.
`--reducer-workers P` fixes the number of concurrent reducer tasks (default 8).
The reducer keeps a fixed worker pool alive across windows, chooses range
boundaries from the observed report-prefix histogram so shards receive similar
record counts, and reuses Hybrid shard result and cursor buffers at their
high-water capacities. Keys never cross ranges, so no locks are required for
aggregation. The Hybrid path also combines shard-local bounded top sets and
certificate frontiers into the same coordinated control result as the serial
reducer. Serial and parallel reducers therefore produce identical estimates,
certificates, and controller trajectories; the parallel mode exchanges a
larger resident working set for lower reduction latency. Coordinator telemetry
charges the retained reusable shard capacities rather than treating them as
zero-cost transient storage.

The benchmark reports communication and resident memory separately:

- `report_volume_kib` is upstream communication: the sum of all serialized
  worker telemetry and on-demand raw-key replies received in a window. It is
  charged equally regardless of whether the reducer retains those records.
  Space-Saving telemetry transmits a fingerprint, estimate, and epsilon; the
  lower bound is derived as `estimate - epsilon` rather than transmitted
  redundantly.
- `control_volume_kib` is downstream communication. Adaptive Space-Saving pays
  for distributing the next capacity. Hybrid additionally pays for distributing
  its ordered next-window head dictionary, which is required to interpret
  compact head-slot reports. Full frames carry the canonicalized dictionary
  actually installed, while delta frames carry removed slot indices and added
  identifiers. Generation-mismatch fallback traffic is also charged. The
  installed size is `q_head_next = |E^{t+1}|`; it is not rounded up to `n`, a
  nominal head capacity, or a placeholder slot. This field also includes
  batched raw-key resolution requests.
- `total_communication_kib` is the sum of upstream and downstream bytes. Message
  framing is charged in both directions; one-time static configuration is not
  charged per window.
- `key_request_volume_kib`, `key_reply_volume_kib`, and
  `key_resolution_count` expose the resolution component separately. After
  identifier-only reduction, each method resolves the identifiers returned by
  its HH query from one reporting worker. Hybrid also resolves newly promoted
  head identifiers and reuses bindings already held for its installed head.
  Replies contain the identifier, a 32-bit raw-key length, and the actual key
  bytes.
- `mem_coord_ingress_kib` is only the report framing concurrently resident while
  records are consumed. The model permits one active record per worker for a
  serial merge and one per worker per active key-range shard for the parallel
  merge.
- `mem_coord_work_kib` is the reducer's peak aggregation, merge, and materialized
  result state.
- `mem_coord_control_peak_kib` is the controller-phase peak. It includes the
  retained published and residual frontiers, bounded top identifiers, the
  canonical promoted-head frame, and policy state. Hybrid does not retain a
  copied residual snapshot or a full-union controller result. Static methods
  report zero controller memory.
- `mem_coord_resolution_peak_kib` is the key-resolution-phase peak. The
  identifier result remains resident while one batched worker response is
  decoded and emitted; worker responses are processed sequentially rather than
  retained together.
- `mem_coord_peak_kib` is the maximum of the complete reduction-phase peak
  (`resident ingress + reducer working state`), controller-phase peak, and
  key-resolution-phase peak. It does not add sequential phases or include
  report bytes already consumed and released.

The same definitions apply to HeavyLocker, Space-Saving, and Hybrid. Thus a
method that emits fewer records has lower report volume, while a streaming
implementation can independently lower resident coordinator memory without
claiming that the reports were never transmitted. Report records use fixed
128-bit fingerprints; Hybrid exact-head entries use compact shared-dictionary
slot identifiers, and zero-mass exact-head entries are omitted. The coordinator
serializes one immutable ordered head frame and streams it to every worker;
communication charges all `m` transmissions while coordinator memory contains
only the single frame. During reduction, modeled coordinator results contain
fingerprints and certificate fields only. Raw-key bytes enter the accounting
only through the common post-query resolution phase and Hybrid's persistent
promoted-head bindings.

Timing is also phase-separated. `report_prepare_ms` measures worker snapshot
preparation in this single-process harness, `reduce_ms` measures coordinator
reduction, and `control_ms` measures the next-window decision. Their sum is
reported as `aggregation_ms`. Because workers are simulated serially, these are
CPU-work measurements rather than claims about distributed wall-clock latency.

HeavyLocker follows the authors' `HeavyLockerSketch-main` implementation at
the algorithm boundary. A worker owns a dense `w`-by-`d` table; each positional
cell contains the common 128-bit fingerprint and a 32-bit counter. Reports
export that dense table without a repeated bucket index. The coordinator merges
one corresponding bucket at a time through a temporary ID-to-count summary,
retains its top `d`, and releases the temporary state before advancing. Its
working-set model contains the compact merged table and the threshold-query
output, not the larger `GlobalItemLB` objects used only by this evaluator.
RAP uses a reentrant reproduction of glibc `rand()` after `srand(1)`. One stream
is shared across all workers of an HL configuration, matching the authors'
harness, while separate configurations receive independent copies so adding
another HL width cannot perturb an existing result.

HeavyLocker can optionally be wrapped with deterministic residual accounting:

```sh
"oracle,hl[hl-w=64 hl-d=6 hl-L=0.7 hl-lossy=2],hl[hl-w=64 hl-d=6 hl-L=0.7 hl-lossy=2 hl-cert=on]"
```

`hl-cert=on` does not change admission, replacement, estimates, or merged
candidates. Each worker additionally retains a lower bound for the identity
currently occupying each slot and the total mass routed through each bucket.
Counter mass inherited through RAP replacement is deliberately excluded from
the new identity's lower bound. The difference between bucket mass and current
lower bounds is reported as residual mass. During the ordinary serial
bucket-wise merge, the coordinator uses it to bound retained candidates,
reported candidates dropped by the top-`d` merge, and keys absent from every
report.

The CSV fields `completeness_certified`, `unseen_mass_ub`, and
`unseen_ub_over_threshold` expose the result. Completeness is certified exactly
when every key omitted from the merged candidate set has a deterministic upper
bound below the strict global HH threshold. A failed certificate means
“unresolved,” not that an HH was missed. Worker memory and upstream
communication include the lower-bound and residual ledgers; ordinary `hl`
retains the reference layout and accounting.

`hl-adaptive=on` enables the conservative certificate-driven width controller
and implies `hl-cert=on`. The initial width defaults to `0.08n` unless `hl-w`
is supplied. After a failed completeness certificate, the next window advances
one rung through `0.08n, 0.16n, 0.32n, 0.64n, 1.28n`; a successful certificate
holds the current width. This initial controller deliberately performs no
downward probes.

## Running experiment sweeps
The top-level `experiments/run_experiments.py` script automates the
paper-oriented experiment grid. It runs `hh_bench`, stores one CSV per run under
a timestamped output directory, writes a manifest, and creates PDF/PNG plots for
parameter sweeps, memory-quality frontiers, controller traces, certification
mass/interval summaries, and timing overheads.

```sh
python3 experiments/run_experiments.py \
  --dataset hash:/path/to/hash_partitioned_stream \
  --dataset stress:/path/to/stress_partitioned_stream \
  --m 16 --n-param 200 --mem-kib 32,64,128,256 --topk 200 --build
```

Datasets must already be partitioned in the input format above. The script uses
one-at-a-time sweeps around the recommended default controller configuration by
default; pass `--profile full` to add the pairwise ambiguity-gain grid. Use
`--dry-run` to inspect the generated `hh_bench` commands without executing them.

## Relation to the paper
- The default adaptive sizing policy is the paper's one-sided, residual-guarded controller. Service violations produce an upward capacity demand from the observed certificate margin. Sufficient observations certify only the deployed capacity; memory is released through explicit geometric probes. A failed probe recovers through a fresh upward response, and its residual evidence limits the depth of later probes until a lower capacity is confirmed sufficient.
- The hybrid sketch mirrors the paper’s head/tail decomposition: an exact head seeded from the previous window’s certified set, plus an adaptive SS tail sized for the remaining mass.
