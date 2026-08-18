#!/usr/bin/env python3
"""Convert a TraceX buffer dump into a native Perfetto trace.

    python trace_convert.py trace.trx

Produces trace.pftrace next to the input and prints a summary. The same dump
remains readable by the TraceX tooling; nothing about it is modified.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from instrumentation import metrics as metric_engine  # noqa: E402
from instrumentation.model import normalize  # noqa: E402
from instrumentation.schema import Schema  # noqa: E402
from pftrace.exporter import PerfettoExporter  # noqa: E402
from tracex import parser as tracex_parser  # noqa: E402


def _fmt_ms(value_us: float) -> str:
    return "n/a" if math.isnan(value_us) else f"{value_us / 1000.0:8.2f} ms"


def _fmt_bytes(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if abs(n) < 1024.0:
            return f"{n:7.2f} {unit}"
        n /= 1024.0
    return f"{n:7.2f} TiB"


def print_summary(trace, results, dump, out) -> None:
    p = lambda *a: print(*a, file=out)  # noqa: E731

    p("")
    p("=" * 62)
    p("Trace summary")
    p("=" * 62)
    p(f"Trace duration:      {results.duration_s:10.3f} s")
    p(f"Trace events:        {len(trace.events):10,}".replace(",", "'"))
    p(f"Buffer capacity:     {dump.capacity:10,} records".replace(",", "'"))
    p(f"Buffer wrapped:      {'yes' if dump.buffer_wrapped else 'no':>10}")
    p(f"Timestamp wraps:     {dump.wraps:10}   (32-bit DWT cycle counter)")
    p(f"CPU / trace clock:   {trace.cpu_hz / 1e6:10.1f} MHz")
    p(f"Registry objects:    {sum(1 for o in dump.objects if not o.available):10} used "
      f"of {len(dump.objects)}")

    p("")
    p(f"Schema:              version {trace.schema.version}, hash 0x{trace.schema.hash:08X}")
    if trace.firmware_schema_hash is None:
        p("                     firmware marker not in the ring "
          "(SCHEMA_INFO was overwritten); compatibility unverified")
    elif not trace.schema_ok:
        p(f"  ** MISMATCH **     firmware reported version "
          f"{trace.firmware_schema_version}, hash 0x{trace.firmware_schema_hash:08X}")
        p("                     Regenerate and reflash, or analyse with the matching schema.")
    else:
        p("                     firmware marker matches")

    counts = results.counts
    p("")
    p("Frames")
    p("-" * 62)
    for label, event in (
        ("captured", "FRAME_CAPTURED"),
        ("submitted to VENC", "VENC_SUBMITTED"),
        ("encoded", "VENC_DONE"),
        ("write started", "FRAME_WRITE_BEGIN"),
        ("written", "FRAME_WRITTEN"),
        ("dropped", "FRAME_DROPPED"),
        ("encoder errors", "VENC_ERROR"),
    ):
        p(f"  {label:<22}{counts.get(event, 0):10,}".replace(",", "'"))

    p("")
    p("Timing")
    p("-" * 62)
    p(f"  {'metric':<22}{'count':>8}{'min':>12}{'mean':>12}{'p95':>12}"
      f"{'p99':>12}{'max':>12}")
    for name, dist in results.distributions.items():
        if dist.unit not in metric_engine._TIME_SCALE or dist.count == 0:
            continue
        s = dist.summary()
        p(f"  {name:<22}{dist.count:>8}"
          f"{_fmt_ms(s['min']):>12}{_fmt_ms(s['mean']):>12}"
          f"{_fmt_ms(s['p95']):>12}{_fmt_ms(s['p99']):>12}{_fmt_ms(s['max']):>12}")
    for name, missing in results.unmatched.items():
        p(f"  note: {name} has {missing} unpaired start event(s)")

    size_dists = {
        n: d for n, d in results.distributions.items()
        if d.unit == "bytes" and d.count
    }
    if size_dists:
        p("")
        p("Sizes")
        p("-" * 62)
        for name, dist in size_dists.items():
            s = dist.summary()
            p(f"  {name:<22}min {_fmt_bytes(s['min'])}  "
              f"mean {_fmt_bytes(s['mean'])}  max {_fmt_bytes(s['max'])}")

    if results.rates:
        p("")
        p("Rates")
        p("-" * 62)
        for name, value in results.rates.items():
            unit = trace.schema.metrics[name].get("unit", "")
            if unit == "bit/s":
                p(f"  {name:<22}{value / 1e6:10.3f} Mbit/s")
            elif unit == "byte/s":
                p(f"  {name:<22}{value / 1e6:10.3f} MB/s")
            else:
                p(f"  {name:<22}{value:10.2f} {unit}")

    if results.gauges:
        p("")
        p("Buffers / queues")
        p("-" * 62)
        for name, series in results.gauges.items():
            if not series.samples:
                continue
            p(f"  {name:<22}current {series.current:>6}   "
              f"high-watermark {series.maximum:>6}   {series.unit}")

    p("")
    p("Top trace producers")
    p("-" * 62)
    p(f"  {'event':<38}{'count':>10}{'ev/s':>10}{'share':>8}")
    for name, count, per_s, share in results.event_histogram[:15]:
        rate = "n/a" if math.isnan(per_s) else f"{per_s:10.1f}"
        p(f"  {name:<38}{count:>10,}{rate:>10}{share:>7.1f}%".replace(",", "'"))
    p("")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path, help="Raw TraceX buffer dump (.trx / .bin)")
    ap.add_argument("-o", "--output", type=Path, help="Output .pftrace (default: <dump>.pftrace)")
    ap.add_argument("--schema", type=Path, help="Path to instrumentation.json")
    ap.add_argument(
        "--cpu-hz",
        type=int,
        help="Trace clock in Hz; overrides the value reported by SCHEMA_INFO.",
    )
    ap.add_argument(
        "--no-native-instants",
        action="store_true",
        help="Omit per-event instants for native ThreadX/FileX events "
             "(keeps thread activity and application events only).",
    )
    ap.add_argument(
        "--no-perfetto",
        action="store_true",
        help="Only print the statistics, do not write a .pftrace file.",
    )
    ap.add_argument("--strict-schema", action="store_true", help="Fail on a schema mismatch.")
    args = ap.parse_args(argv)

    if not args.dump.exists():
        ap.error(f"{args.dump} does not exist")

    schema = Schema.load(args.schema)
    dump = tracex_parser.parse(args.dump.read_bytes())
    trace = normalize(dump, schema, cpu_hz=args.cpu_hz)

    if not trace.events:
        print("The trace buffer contains no valid records.", file=sys.stderr)
        return 2

    results = metric_engine.compute(trace)
    print_summary(trace, results, dump, sys.stdout)

    if args.strict_schema and not trace.schema_ok:
        print("Aborting: schema mismatch (--strict-schema).", file=sys.stderr)
        return 3

    if not args.no_perfetto:
        out_path = args.output or args.dump.with_suffix(".pftrace")
        exporter = PerfettoExporter(trace, native_instants=not args.no_native_instants)
        exporter.build().write(out_path)
        print(f"Wrote {out_path} ({out_path.stat().st_size:,} bytes)".replace(",", "'"))
        print("Open it at https://ui.perfetto.dev")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
