#!/usr/bin/env python3
"""Extract timestamped hashtag occurrences from the March 2013 Twitter N3 dump.

The source is line-oriented Turtle/N3 in which each tweet and its mentioned
entities form a blank-line-delimited record.  This extractor deliberately
keeps only the information required by the next windowing stage. One row is
written per hashtag occurrence, so a tweet with several hashtags emits several
rows with the same timestamp:

    {"timestamp": "2013-03-01T15:32:33", "hashtag": "kpopfanproblem"}

It streams the input and never builds an RDF graph, which keeps memory bounded
for the multi-gigabyte monthly dump.  Hashtag labels are preserved verbatim;
normalization is intentionally deferred to the dataset-definition stage.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


CREATED_RE = re.compile(
    r"^(_:[^\s]+)\s+.*?\bdc:created\s+\"((?:\\.|[^\"\\])*)\""
)
MENTION_RE = re.compile(r"^(_:[^\s]+)\s+schema:mentions\s+(_:[^\s]+)\s*\.")
TAG_RE = re.compile(
    r"^(_:[^\s]+)\s+.*?\bsioc_t:Tag\b.*?\brdfs:label\s+\"((?:\\.|[^\"\\])*)\""
)


@dataclass
class ExtractionStats:
    records_seen: int = 0
    posts_with_timestamp: int = 0
    posts_with_hashtags: int = 0
    hashtag_occurrences: int = 0
    records_without_timestamp: int = 0
    tagged_records_without_post: int = 0
    first_timestamp: str | None = None
    last_timestamp: str | None = None


def parse_n3_string(value: str) -> str:
    """Decode N3's quoted-string escapes with the JSON-compatible subset."""
    try:
        return json.loads(f'"{value}"')
    except json.JSONDecodeError:
        # Keep an unusual but valid-enough label rather than losing its event.
        return value


def records(lines: Iterable[str]) -> Iterable[list[str]]:
    """Yield blank-line-delimited source records without retaining the file."""
    record: list[str] = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            if record:
                yield record
                record = []
            continue
        if not stripped.startswith("@prefix"):
            record.append(stripped)
    if record:
        yield record


def extract_record(record: list[str]) -> tuple[str | None, list[str], bool]:
    """Return the post timestamp and mentioned hashtag labels for one record."""
    post_timestamps: dict[str, str] = {}
    mentions: dict[str, list[str]] = {}
    tag_labels: dict[str, str] = {}

    for line in record:
        if match := CREATED_RE.match(line):
            post_timestamps[match.group(1)] = parse_n3_string(match.group(2))
        if match := MENTION_RE.match(line):
            mentions.setdefault(match.group(1), []).append(match.group(2))
        if match := TAG_RE.match(line):
            tag_labels[match.group(1)] = parse_n3_string(match.group(2))

    if not post_timestamps:
        return None, [], bool(tag_labels)

    # A source record represents one tweet.  Selecting the first timestamp is
    # deliberate: malformed records with multiple post subjects are counted
    # once rather than producing an ambiguous duplicated event.
    post_id, timestamp = next(iter(post_timestamps.items()))
    hashtags = [
        tag_labels[target]
        for target in mentions.get(post_id, [])
        if target in tag_labels
    ]
    return timestamp, hashtags, False


def validate_timestamp(value: str) -> datetime:
    """Validate the expected ISO-8601 timestamp without changing its spelling."""
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract timestamped Twitter hashtag events from N3/Turtle input."
    )
    parser.add_argument("input", type=Path, help="Twitter month N3 file")
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="JSONL output, one hashtag occurrence per row",
    )
    parser.add_argument(
        "--max-records",
        type=int,
        default=None,
        help="Optional record limit for a quick format-validation run",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.max_records is not None and args.max_records <= 0:
        raise ValueError("--max-records must be positive")
    if not args.input.is_file():
        raise FileNotFoundError(f"input file does not exist: {args.input}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    stats = ExtractionStats()

    with args.input.open(encoding="utf-8", errors="replace") as source, args.output.open(
        "w", encoding="utf-8"
    ) as destination:
        for record in records(source):
            stats.records_seen += 1
            timestamp, hashtags, orphan_tag = extract_record(record)
            if orphan_tag:
                stats.tagged_records_without_post += 1
            if timestamp is None:
                stats.records_without_timestamp += 1
            else:
                stats.posts_with_timestamp += 1
                validate_timestamp(timestamp)
                if hashtags:
                    for hashtag in hashtags:
                        destination.write(
                            json.dumps(
                                {"timestamp": timestamp, "hashtag": hashtag},
                                ensure_ascii=False,
                                separators=(",", ":"),
                            )
                            + "\n"
                        )
                    stats.posts_with_hashtags += 1
                    stats.hashtag_occurrences += len(hashtags)
                    if stats.first_timestamp is None or timestamp < stats.first_timestamp:
                        stats.first_timestamp = timestamp
                    if stats.last_timestamp is None or timestamp > stats.last_timestamp:
                        stats.last_timestamp = timestamp

            if args.max_records and stats.records_seen >= args.max_records:
                break

    metadata_path = args.output.with_name(f"{args.output.stem}_metadata.json")
    metadata = {
        "source": str(args.input),
        "format": "timestamped Twitter hashtag events",
        "record_schema": {"timestamp": "ISO-8601 string", "hashtag": "string"},
        "normalization": "none; labels are preserved exactly as stored in N3",
        "stats": asdict(stats),
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    print(f"Extracted {stats.hashtag_occurrences:,} hashtag occurrences from "
          f"{stats.posts_with_hashtags:,} tweets.")
    print(f"JSONL: {args.output}")
    print(f"Metadata: {metadata_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
