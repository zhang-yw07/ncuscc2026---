#!/usr/bin/env python3
"""Generate deterministic log-filter.v1 inputs on Linux."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys
import tempfile

GENERATOR_VERSION = "log-generator.v1"
MAX_RECORDS_DEFAULT = 10_000_000
HEADER = struct.Struct("<8sIIQ")
RECORD = struct.Struct("<QIIII")
MASK32 = 0xFFFFFFFF


def unsigned_decimal(value: str) -> int:
    if not value or not value.isascii() or not value.isdecimal():
        raise argparse.ArgumentTypeError("expected unsigned decimal integer")
    return int(value, 10)


def random32(state: int) -> int:
    return (state * 1664525 + 1013904223) & MASK32


def generate_record(index: int, hit_percent: int, pattern: str, state: int):
    state = random32(state)
    decision = state
    hit = ((decision >> 8) % 100 < hit_percent) if pattern == "random" else (index % 10000 < hit_percent * 100)
    state = random32(state); timestamp = 1000 + state % 1000
    state = random32(state); latency = 100 + state % 1000
    state = random32(state); payload = state
    state = random32(state); flags = 3 | (state & 0xFFFFFFFC)
    if not hit:
        reason = (decision >> 16) % 3
        if reason == 0:
            state = random32(state)
            timestamp = 2000 + state % 1000
        elif reason == 1:
            state = random32(state)
            latency = state % 100
        else:
            flags &= ~1
    return (timestamp, index & MASK32, latency, payload, flags), state


def checked_target(path: pathlib.Path, force: bool) -> None:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"refusing non-regular output path: {path}")
    if not force:
        raise RuntimeError(f"output already exists: {path}")
    if info.st_nlink != 1:
        raise RuntimeError(f"refusing multiply-linked output path: {path}")


def write_atomic(path: pathlib.Path, data_writer, force: bool) -> None:
    checked_target(path, force)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary_path = pathlib.Path(temporary)
    try:
        with os.fdopen(fd, "wb") as stream:
            data_writer(stream)
            stream.flush()
            os.fsync(stream.fileno())
        if not force and path.exists():
            raise RuntimeError(f"output appeared during generation: {path}")
        os.replace(temporary_path, path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def main() -> int:
    if not sys.platform.startswith("linux"):
        raise RuntimeError("generate_logs.py only runs on Linux (WSL 2 is supported)")
    parser = argparse.ArgumentParser(description="Generate deterministic log-filter.v1 input data")
    parser.add_argument("--count", required=True, type=unsigned_decimal)
    parser.add_argument("--hit-percent", required=True, type=unsigned_decimal)
    parser.add_argument("--pattern", required=True, choices=("random", "blocks"))
    parser.add_argument("--seed", required=True, type=unsigned_decimal)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--metadata", type=pathlib.Path, help="default: OUTPUT.json")
    parser.add_argument("--max-records", type=unsigned_decimal, default=MAX_RECORDS_DEFAULT)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if args.hit_percent > 100:
        parser.error("--hit-percent must be between 0 and 100")
    if args.seed > MASK32:
        parser.error("--seed must fit uint32")
    if args.count > args.max_records:
        parser.error("--count exceeds --max-records")
    # Make paths absolute without resolving symlinks; security checks must see the link itself.
    output = pathlib.Path(os.path.abspath(args.output))
    metadata = pathlib.Path(os.path.abspath(args.metadata or pathlib.Path(f"{args.output}.json")))
    if output == metadata:
        parser.error("--metadata must differ from --output")
    checked_target(output, args.force)
    checked_target(metadata, args.force)

    digest = hashlib.sha256()
    hits = 0

    def write_binary(stream):
        nonlocal hits
        header = HEADER.pack(b"NCULOG01", 1, 24, args.count)
        stream.write(header); digest.update(header)
        state = args.seed
        for index in range(args.count):
            row, state = generate_record(index, args.hit_percent, args.pattern, state)
            encoded = RECORD.pack(*row)
            stream.write(encoded); digest.update(encoded)
            if 1000 <= row[0] < 2000 and row[2] >= 100 and row[4] & 3 == 3:
                hits += 1

    write_atomic(output, write_binary, args.force)
    manifest = {
        "schemaVersion": "log-generator-manifest.v1",
        "generatorVersion": GENERATOR_VERSION,
        "format": "log-filter.v1",
        "count": args.count,
        "hitPercentRequested": args.hit_percent,
        "hitCountActual": hits,
        "hitPercentActual": 0.0 if args.count == 0 else 100.0 * hits / args.count,
        "pattern": args.pattern,
        "seed": args.seed,
        "query": {"begin": 1000, "end": 2000, "minLatencyUs": 100, "requiredFlags": 3},
        "bytes": 24 + 24 * args.count,
        "sha256": digest.hexdigest(),
        "file": output.name,
    }
    encoded_manifest = (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    try:
        write_atomic(metadata, lambda stream: stream.write(encoded_manifest), args.force)
    except BaseException:
        output.unlink(missing_ok=True)
        raise
    print(json.dumps(manifest, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
