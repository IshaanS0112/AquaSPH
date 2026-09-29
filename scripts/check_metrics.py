#!/usr/bin/env python3
"""Check that metrics files are valid JSON and carry their provenance.

The metrics file is what makes a scenario an experiment rather than an
animation, so it is checked like an interface: parseable, carrying the
fields that let a result be traced back to the code that produced it, and
reporting STABLE.

Usage: scripts/check_metrics.py <file-or-glob> [...]
Exit code 0 if every file passes.
"""
import glob
import json
import sys

REQUIRED = ["scenario", "tier", "tier_caveat", "git_revision", "quality",
            "threads", "status", "particles", "resolution", "time",
            "density", "dynamics", "volume"]


def main(patterns):
    files = sorted({p for pat in patterns for p in glob.glob(pat)})
    if not files:
        print("no metrics files matched", file=sys.stderr)
        return 1

    bad = 0
    for path in files:
        try:
            with open(path) as f:
                m = json.load(f)
        except Exception as e:
            # Catches the specific failure mode of writing NaN or Infinity, which are not valid
            # JSON and would make the file unreadable by every standard parser exactly when
            # something has gone wrong and you most need to read it.
            print(f"{path}: not valid JSON: {e}", file=sys.stderr)
            bad = 1
            continue

        missing = [k for k in REQUIRED if k not in m]
        if missing:
            print(f"{path}: missing {missing}", file=sys.stderr)
            bad = 1
        elif m["status"] != "STABLE":
            print(f"{path}: status is {m['status']}", file=sys.stderr)
            bad = 1
        elif not m["git_revision"] or m["git_revision"] == "unknown":
            print(f"{path}: no git revision recorded", file=sys.stderr)
            bad = 1

    print(f"checked {len(files)} metrics file(s)")
    return bad


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["results/**/*.json"]))
