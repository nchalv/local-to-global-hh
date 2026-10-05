# Datasets

## CAIDA

The paper uses two disjoint 200-window slices of the CAIDA UCSD Anonymized
Internet Traces of March 15, 2018. Windows are five seconds long.

- CAIDA-A: source windows 0--199, used for configuration and head ablation.
- CAIDA-B: source windows 400--599, used for comparison and scaling.

The artifact expects normalized JSONL counts in the form:

```json
{"window": 0, "counts": {"key": 123, "other_key": 45}}
```

The CAIDA dataset must be cited as:

> CAIDA UCSD Anonymized Internet Traces - 2018-03-15.
> https://data.caida.org/datasets/passive-2018

Raw traces and derived streams are excluded from this repository. The exact
source paths are supplied through `CAIDA_COUNTS_A` and `CAIDA_COUNTS_B` when
running `./artifact prepare-caida`. The command validates the schema, requires
200 contiguous windows in each slice, and records SHA-256 checksums before any
partition-specific stream is generated.

## Synthetic data

The controlled trajectory workload is generated locally from the versioned
scenario and partitioning configurations. Generator manifests record the
configuration paths and input checksums for each run.
