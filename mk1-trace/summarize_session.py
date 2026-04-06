#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path


BTN_TYPE = "0x03734e00"
HEADER_WORDS = 7

ENTRY_RE = re.compile(
    r"^\[(?P<stamp>[^\]]+)\]\s+"
    r"(?P<direction>\S+)\s+port=(?P<port>\S+)\s+msgid=(?P<msgid>-?\d+)\s+"
    r"len=(?P<length>\d+)\s+type=(?P<type>0x[0-9a-fA-F]+)\s+\((?P<name>[^)]+)\)$"
)
WORD_RE = re.compile(r"0x[0-9a-fA-F]{8}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize BTN_DATA events from a mk1-trace session.log file."
    )
    parser.add_argument("session_log", type=Path, help="Path to mk1-trace session.log")
    parser.add_argument(
        "--press-only",
        action="store_true",
        help="Only print records where is_pressed == 1",
    )
    return parser.parse_args()


def iter_entries(lines):
    block = []
    for raw_line in lines:
        line = raw_line.rstrip("\n")
        if line:
            block.append(line)
            continue
        if block:
            yield block
            block = []
    if block:
        yield block


def parse_words(block):
    for line in block:
        stripped = line.strip()
        if stripped.startswith("words:"):
            return [int(token, 16) for token in WORD_RE.findall(stripped)]
    return []


def summarize(path: Path, press_only: bool) -> int:
    if not path.exists():
        print(f"error: file not found: {path}", file=sys.stderr)
        return 1

    event_count = 0
    record_count = 0

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for block in iter_entries(handle):
            match = ENTRY_RE.match(block[0])
            if not match:
                continue
            if match.group("type").lower() != BTN_TYPE:
                continue

            words = parse_words(block)
            if len(words) < HEADER_WORDS:
                continue

            payload_size = words[6]
            records = words[HEADER_WORDS:]
            expected_record_words = max((payload_size - 0x18) // 4, 0)
            if expected_record_words and len(records) > expected_record_words:
                records = records[:expected_record_words]

            if len(records) % 2 != 0:
                records = records[:-1]

            event_count += 1
            print(
                f"event={event_count} stamp={match.group('stamp')} port={match.group('port')} "
                f"payload_size=0x{payload_size:08x} records={len(records) // 2}"
            )

            for index in range(0, len(records), 2):
                control_index = records[index]
                is_pressed = records[index + 1]
                if press_only and is_pressed == 0:
                    continue
                record_count += 1
                print(
                    f"  control_index=0x{control_index:08x} ({control_index:3d}) "
                    f"is_pressed={is_pressed}"
                )

    print(f"summary: btn_events={event_count} btn_records={record_count}")
    return 0


def main() -> int:
    args = parse_args()
    return summarize(args.session_log, args.press_only)


if __name__ == "__main__":
    raise SystemExit(main())
