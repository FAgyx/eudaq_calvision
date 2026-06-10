#!/usr/bin/env python3
"""
Write top-level event IDs plus DRS/FERS subevent IDs from a Calvision EUDAQ
.raw file into a CSV file.

Edit the fixed paths in main() and run:
    python3 print_event_ids.py
"""

from __future__ import annotations

import re
import subprocess
import sys
import csv
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
EUCLI_READER = REPO_ROOT / "bin" / "euCliReader"
EVENT_HIGH_ALL = str(2**32 - 1)


TAG_EVENT_N = re.compile(r"<EventN>(\d+)</EventN>")
TAG_TRIGGER_N = re.compile(r"<TriggerN>(\d+)</TriggerN>")
TAG_DESCRIPTION = re.compile(r"<Description>([^<]+)</Description>")


def validate_raw_path(raw_path: Path) -> Path:
    raw_path = raw_path.expanduser().resolve()
    if not raw_path.exists():
        raise SystemExit(f"Raw file does not exist: {raw_path}")
    if not raw_path.is_file():
        raise SystemExit(f"Not a file: {raw_path}")
    return raw_path

def event_high_from_limit(max_events: int | None) -> str:
    if max_events is None:
        event_high = EVENT_HIGH_ALL
        return event_high
    if max_events < 0:
        raise SystemExit("MAX_EVENTS must be >= 0")
    return str(max_events)


def run_eucli_reader(raw_path: Path, event_high: str) -> str:
    if not EUCLI_READER.exists():
        raise SystemExit(f"Missing reader executable: {EUCLI_READER}")

    cmd = [
        str(EUCLI_READER),
        "-i",
        str(raw_path),
        "-e",
        "0",
        "-E",
        event_high,
    ]
    result = subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"euCliReader failed:\n{result.stdout}")
    return result.stdout


def csv_value(value: int | None) -> str:
    return "" if value is None else str(value)

def collect_rows(reader_output: str) -> list[list[str]]:
    rows: list[list[str]] = []

    depth = 0
    top_event: dict[str, int | None] | None = None
    current_sub: dict[str, int | None | str] | None = None
    drs = {"event_n": None, "trigger_n": None}
    fers = {"event_n": None, "trigger_n": None}
    subevent_count = 0

    for raw_line in reader_output.splitlines():
        line = raw_line.strip()
        if line == "<Event>":
            depth += 1
            if depth == 1:
                top_event = {"event_n": None, "trigger_n": None}
                drs = {"event_n": None, "trigger_n": None}
                fers = {"event_n": None, "trigger_n": None}
                subevent_count = 0
            elif depth == 2:
                current_sub = {"description": None, "event_n": None, "trigger_n": None}
            continue

        if line == "</Event>":
            if depth == 2 and current_sub is not None:
                description = current_sub["description"]
                if description == "DRSProducer":
                    drs["event_n"] = current_sub["event_n"]
                    drs["trigger_n"] = current_sub["trigger_n"]
                elif description == "FERSProducer":
                    fers["event_n"] = current_sub["event_n"]
                    fers["trigger_n"] = current_sub["trigger_n"]
                if description in ("DRSProducer", "FERSProducer"):
                    subevent_count += 1
                current_sub = None
            elif depth == 1 and top_event is not None:
                rows.append(
                    [
                        csv_value(top_event["event_n"]),
                        csv_value(top_event["trigger_n"]),
                        csv_value(drs["event_n"]),
                        csv_value(drs["trigger_n"]),
                        csv_value(fers["event_n"]),
                        csv_value(fers["trigger_n"]),
                        str(subevent_count),
                    ]
                )
                top_event = None
            depth -= 1
            continue

        if depth == 1 and top_event is not None:
            if top_event["event_n"] is None:
                match = TAG_EVENT_N.search(line)
                if match:
                    top_event["event_n"] = int(match.group(1))
                    continue
            if top_event["trigger_n"] is None:
                match = TAG_TRIGGER_N.search(line)
                if match:
                    top_event["trigger_n"] = int(match.group(1))
                    continue

        if depth == 2 and current_sub is not None:
            if current_sub["description"] is None:
                match = TAG_DESCRIPTION.search(line)
                if match:
                    current_sub["description"] = match.group(1)
                    continue
            if current_sub["event_n"] is None:
                match = TAG_EVENT_N.search(line)
                if match:
                    current_sub["event_n"] = int(match.group(1))
                    continue
            if current_sub["trigger_n"] is None:
                match = TAG_TRIGGER_N.search(line)
                if match:
                    current_sub["trigger_n"] = int(match.group(1))
                    continue

    return rows


def write_csv(output_csv: Path, rows: list[list[str]]) -> None:
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(
            [
                "top_event_n",
                "top_trigger_n",
                "drs_event_n",
                "drs_trigger_n",
                "fers_event_n",
                "fers_trigger_n",
                "subevent_count",
            ]
        )
        writer.writerows(rows)


def main(argv: list[str]) -> int:
    # Fixed inputs: edit these values directly.
    raw_path = validate_raw_path(Path("/hdd/euDAQ_staging/run070/run070.raw"))
    output_csv = raw_path.parent / f"{raw_path.stem}_event_ids.csv"
    max_events: int | None = None

    event_high = event_high_from_limit(max_events)
    reader_output = run_eucli_reader(raw_path, event_high)
    rows = collect_rows(reader_output)
    write_csv(output_csv, rows)
    print(f"Wrote {len(rows)} rows to {output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
