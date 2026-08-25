# Data Preparation

Utilities in this directory convert raw or external datasets into the
per-window count formats consumed by the generator.

The normalized interchange format is JSONL with one row per logical window:

```json
{"window": 0, "counts": {"key": 123, "other_key": 45}}
```

Dataset-specific converters should emit that shape so the generator and
evaluator remain dataset-agnostic.

## CAIDA

`caida_analyzer.py` converts CAIDA `.pcap.gz` traces into JSONL rows:

```json
{"window": 0, "counts": {"192.0.2.1": 42}}
```

Example:

```bash
python3 data_preparation/caida_analyzer.py /path/to/caida \
  --output /tmp/caida_counts \
  --window-seconds 10
```

The generated `/tmp/caida_counts.jsonl` can then be passed to the generator:

```bash
cd generator
python3 main.py \
  --run-config config/runs/round_robin.json \
  --real-counts /tmp/caida_counts.jsonl \
  --save-windows-separately
```

To model ingress or routing locality after source-IP count extraction, use the
generator's source-prefix partitioning policy:

```bash
cd generator
python3 main.py \
  --run-config config/runs/round_robin.json \
  --partitioning config/partitioning/source_prefix_locality.json \
  --real-counts /tmp/caida_counts.jsonl \
  --save-windows-separately
```

This assigns source-IP keys to stable workers by network prefix, using IPv4
`/16` and IPv6 `/48` by default.

For chunked CAIDA processing, pass the same `--origin-timestamp` and
`--window-seconds` to every chunk so window IDs remain comparable.

`--window-seconds` is the canonical window-size argument and supports
sub-minute windows. The older `--window` option is still accepted as minutes for
compatibility.

To extract a disjoint experiment interval from an already normalized trace,
use `slice_window_counts.py`. It validates that the selected source windows are
contiguous, rebases their IDs, retains each original ID as `source_window`, and
writes interval metadata:

```bash
python3 data_preparation/slice_window_counts.py \
  data_preparation/generated/caida_20180315_130000_5s_dirA.jsonl \
  --output data_preparation/generated/caida_20180315_133230_200w_5s_dirA.jsonl \
  --start-window 400 \
  --num-windows 200 \
  --source-metadata \
    data_preparation/generated/caida_20180315_130000_5s_dirA_metadata.json
```

## Twitter hashtags

`twitter_hashtag_extractor.py` streams the N3/Turtle monthly Twitter dump and
extracts only the association between a tweet's `dc:created` timestamp and the
hashtags it mentions through `sioc_t:Tag`. It writes one JSONL row for every
hashtag occurrence:

```json
{"timestamp":"2013-03-01T15:32:33","hashtag":"kpopfanproblem"}
```

For example:

```bash
python3 data_preparation/twitter_hashtag_extractor.py \
  /path/to/month_2013-03.n3 \
  --output data_preparation/generated/twitter_2013-03_hashtag_events.jsonl
```

The extractor preserves hashtag spelling and timestamp strings exactly. A
tweet with multiple hashtags emits multiple rows sharing its timestamp. The
next preparation step converts these events into the standard per-window count
format, allowing the window definition to be chosen independently of parsing.

`twitter_window_counts.py` performs that conversion. It accepts one or more
chronologically sorted event files, verifies their order across file
boundaries, and emits the same per-window JSONL shape used by the CAIDA tool:

```bash
python3 data_preparation/twitter_window_counts.py \
  data_preparation/generated/twitter_2013-04_hashtag_events_sorted.jsonl \
  data_preparation/generated/twitter_2013-05_hashtag_events_sorted.jsonl \
  --window-hours 24 \
  --output data_preparation/generated/twitter_2013-04_05_24h_hashtags.jsonl
```

By default, window 0 begins at midnight UTC on the first event's calendar day.
Pass `--origin 2013-04-01T00:00:00` to set this explicitly.

To retain one physical file per logical window, pass
`--save-windows-separately` and provide an output directory instead of a JSONL
filename. The directory then contains `window_000000.json`,
`window_000001.json`, and so on, plus `metadata.json`.
