#!/usr/bin/env python3
"""Summarize POMPPC_GL_FRAMES swap intervals (not GPU completion timings)."""
import argparse
import csv
import json
import math
from collections import Counter


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def summarize(rows, start=0.0, duration=None, context=None):
    if not rows:
        raise ValueError("empty frame trace")
    contexts = Counter(row["context"] for row in rows)
    if context is None:
        if len(contexts) != 1:
            raise ValueError("multiple contexts: select --context from " + str(dict(contexts)))
        context = next(iter(contexts))
    selected = [r for r in rows if r["context"] == context]
    stop = math.inf if duration is None else start + duration
    intervals = []
    for before, after in zip(selected, selected[1:]):
        a, b = float(before["elapsed_ms"]), float(after["elapsed_ms"])
        if b <= a:
            raise ValueError("timestamps must increase within each context")
        # Keep only complete intervals in the requested window.
        if a >= start * 1000 and b <= stop * 1000:
            intervals.append(b - a)
    if not intervals:
        raise ValueError("no complete swap intervals in the selected window")
    elapsed = sum(intervals)
    return {
        "context": context,
        "intervals": len(intervals),
        "measured_seconds": elapsed / 1000,
        "fps": 1000 * len(intervals) / elapsed,
        "median_ms": percentile(intervals, 0.5),
        "p95_ms": percentile(intervals, 0.95),
        "p99_ms": percentile(intervals, 0.99),
        "worst_ms": max(intervals),
        "over_33_33_ms": sum(t > 1000 / 30 for t in intervals),
        "over_100_ms": sum(t > 100 for t in intervals),
        "over_250_ms": sum(t > 250 for t in intervals),
        "note": "Application swap intervals; exclude loading explicitly with --start. "
                "This does not prove GPU completion, scene identity or input latency.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--start", type=float, default=0, help="seconds since first swap")
    parser.add_argument("--duration", type=float)
    parser.add_argument("--context")
    args = parser.parse_args()
    if args.start < 0 or (args.duration is not None and args.duration <= 0):
        parser.error("start must be nonnegative and duration positive")
    try:
        with open(args.trace, newline="") as stream:
            result = summarize(list(csv.DictReader(stream)), args.start,
                               args.duration, args.context)
    except (ValueError, KeyError) as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
