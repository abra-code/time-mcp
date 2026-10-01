#!/usr/bin/env python3
"""Checks time-mcp's time zone reader against two independent readers of the
same zone database: Python's zoneinfo module and the C library.

Usage: python3 test_zones.py [path/to/zonedump]

For every zone in the database:
1. offsets and daylight saving flags at UTC instants, hourly through 2026
   and every 11 days or so from 1900 to 2100, against zoneinfo;
2. the same hourly and from 1970 to 2100 every 3 hours against localtime_r
   (to 2037 where the C library is known to be wrong after that);
3. wall clock readings every 5 minutes around each 2026 change, against
   zoneinfo's first-occurrence rule (fold=0).
"""

import os
import subprocess
import sys
from datetime import datetime, timedelta
from zoneinfo import TZPATH, ZoneInfo, available_timezones

HERE = os.path.dirname(os.path.abspath(__file__))
ZONEDUMP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "zonedump")

YEAR_START = 1767225600  # 2026-01-01T00:00:00Z
YEAR_END = 1798761600  # 2027-01-01T00:00:00Z
LONG_START = -2208988800  # 1900-01-01
LONG_END = 4102444800  # 2100-01-01
LONG_STEP = 951437  # about 11 days, and no multiple of an hour
LIBC_RULE_LIMIT = 2114380800  # 2037-01-01
EPOCH = datetime(1970, 1, 1)
ZERO = timedelta(0)

failures = 0


def fail(text):
    global failures
    failures += 1
    if failures <= 30:
        print("FAIL: " + text)


class Dump:
    """A running zonedump process."""

    def __init__(self):
        env = dict(os.environ)
        env.pop("TZDIR", None)
        self.process = subprocess.Popen([ZONEDUMP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env,
                                        universal_newlines=True)

    def ask(self, command, zone, start, end, step):
        self.process.stdin.write("%s %s %d %d %d\n" % (command, zone, start, end, step))
        self.process.stdin.flush()
        lines = []
        while True:
            line = self.process.stdout.readline().rstrip("\n")
            if not line:
                raise RuntimeError("zonedump stopped")
            lines.append(line)
            if line.startswith("end") or line.startswith("error"):
                return lines

    def changes(self, command, zone, start, end, step):
        lines = self.ask(command, zone, start, end, step)
        if lines[-1] != "end":
            return None
        return [tuple(int(field) for field in line.split()) for line in lines[:-1]]

    def close(self):
        self.process.stdin.close()
        self.process.wait()


def rule_of(name):
    """The POSIX TZ rule at the end of the zone's file, or an empty string."""
    for directory in TZPATH:
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            with open(path, "rb") as file:
                lines = file.read().split(b"\n")
            return lines[-2].decode("ascii", "replace") if len(lines) >= 3 else ""
    return ""


def python_utc_changes(zone, start, end, step):
    """What zonedump's "utc" command prints, computed with zoneinfo."""
    changes = []
    previous = None
    for sample in range(start, end + 1, step):
        moment = datetime.fromtimestamp(sample, zone)
        current = (moment.utcoffset(), moment.dst())
        if current != previous:
            changes.append((sample, int(current[0].total_seconds()), 1 if current[1] else 0))
            previous = current
    return changes


def python_wall_changes(zone, start, end, step):
    """What zonedump's "wall" command prints, computed with zoneinfo."""
    changes = []
    previous = None
    for sample in range(start, end + 1, step):
        moment = (EPOCH + timedelta(seconds=sample)).replace(tzinfo=zone)
        current = (moment.utcoffset(), moment.dst())
        if current != previous:
            changes.append((sample, int(current[0].total_seconds()), 1 if current[1] else 0))
            previous = current
    return changes


def compare(label, ours, theirs):
    if ours != theirs:
        differing = [pair for pair in zip(ours or [], theirs) if pair[0] != pair[1]][:2]
        fail("%s: ours %d changes, zoneinfo %d; first differences %s" % (label, len(ours or []), len(theirs),
                                                                         differing))


def test_reader(names):
    dump = Dump()
    for name in names:
        zone = ZoneInfo(name)
        hourly = dump.changes("utc", name, YEAR_START, YEAR_END, 3600)
        if hourly is None:
            fail("%s: does not load" % name)
            continue
        compare(name + " hourly 2026", hourly, python_utc_changes(zone, YEAR_START, YEAR_END, 3600))
        compare(name + " 1900-2100", dump.changes("utc", name, LONG_START, LONG_END, LONG_STEP),
                python_utc_changes(zone, LONG_START, LONG_END, LONG_STEP))

        # The C library on macOS ignores a negative time of day in the rule
        # that takes over after the last recorded transition ("M3.5.0/-1",
        # three Greenland zones), so those are compared up to 2037 only.
        libc_end = LIBC_RULE_LIMIT if "/-" in rule_of(name) else LONG_END
        for start, end, step in [(YEAR_START, YEAR_END, 3600), (0, libc_end, 10800)]:
            lines = dump.ask("libc", name, start, end, step)
            if lines[-1] != "end 0":
                fail("%s: differs from localtime_r: %s" % (name, lines[:2] + lines[-1:]))

        # Wall clock readings around each change of the year. The first
        # entry of `hourly` is the starting state, not a change.
        for instant, offset, _ in hourly[1:]:
            start = instant + offset - 4 * 3600
            end = instant + offset + 4 * 3600
            compare("%s wall readings near %d" % (name, instant), dump.changes("wall", name, start, end, 300),
                    python_wall_changes(zone, start, end, 300))
        # And over the whole year, coarsely.
        compare(name + " wall readings 2026", dump.changes("wall", name, YEAR_START, YEAR_END, 9000),
                python_wall_changes(zone, YEAR_START, YEAR_END, 9000))
    dump.close()


def main():
    for path in (ZONEDUMP,):
        if not os.path.isfile(path):
            print("no binary at %s" % path)
            return 2
    names = sorted(available_timezones())
    print("%d zones" % len(names))
    test_reader(names)
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
