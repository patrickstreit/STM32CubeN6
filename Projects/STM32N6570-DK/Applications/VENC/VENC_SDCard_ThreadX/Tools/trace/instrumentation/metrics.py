"""Metric engine.

Everything here is derived from the raw event stream; the target computes no
statistics of its own. Metric definitions come from instrumentation.yaml via
the generated schema, so adding a metric never requires touching this file.
"""

from __future__ import annotations

import math
from collections import Counter, defaultdict
from collections.abc import Iterator
from dataclasses import dataclass, field

from .model import NormalizedEvent, NormalizedTrace


@dataclass
class Distribution:
    unit: str
    values: list[float] = field(default_factory=list)

    @property
    def count(self) -> int:
        return len(self.values)

    def _sorted(self) -> list[float]:
        return sorted(self.values)

    def percentile(self, p: float) -> float:
        if not self.values:
            return math.nan
        ordered = self._sorted()
        idx = min(len(ordered) - 1, max(0, int(math.ceil(p / 100.0 * len(ordered))) - 1))
        return ordered[idx]

    @property
    def min(self) -> float:
        return min(self.values) if self.values else math.nan

    @property
    def max(self) -> float:
        return max(self.values) if self.values else math.nan

    @property
    def mean(self) -> float:
        return sum(self.values) / len(self.values) if self.values else math.nan

    @property
    def median(self) -> float:
        return self.percentile(50)

    def histogram(self, bins: int = 10) -> list[tuple[float, float, int]]:
        if not self.values:
            return []
        lo, hi = self.min, self.max
        if hi <= lo:
            return [(lo, hi, len(self.values))]
        width = (hi - lo) / bins
        counts = [0] * bins
        for v in self.values:
            idx = min(bins - 1, int((v - lo) / width))
            counts[idx] += 1
        return [(lo + i * width, lo + (i + 1) * width, counts[i]) for i in range(bins)]

    def summary(self) -> dict[str, float]:
        return {
            "count": self.count,
            "min": self.min,
            "max": self.max,
            "mean": self.mean,
            "median": self.median,
            "p95": self.percentile(95),
            "p99": self.percentile(99),
        }


@dataclass
class GaugeSeries:
    unit: str
    samples: list[tuple[int, int]] = field(default_factory=list)  # (ts_ns, value)

    @property
    def current(self) -> int | None:
        return self.samples[-1][1] if self.samples else None

    @property
    def maximum(self) -> int | None:
        return max(v for _, v in self.samples) if self.samples else None


@dataclass
class MetricResults:
    counts: dict[str, int]
    rates: dict[str, float]
    distributions: dict[str, Distribution]
    gauges: dict[str, GaugeSeries]
    event_histogram: list[tuple[str, int, float, float]]
    unmatched: dict[str, int]
    duration_s: float


_TIME_SCALE = {"us": 1e-3, "ms": 1e-6, "ns": 1.0, "s": 1e-9}


def compute(trace: NormalizedTrace) -> MetricResults:
    schema = trace.schema
    duration_ns = trace.duration_ns
    duration_s = duration_ns / 1e9 if duration_ns else 0.0

    by_name: dict[str, list] = defaultdict(list)
    for event in trace.events:
        by_name[event.name].append(event)

    counts = {name: len(evs) for name, evs in by_name.items()}
    rates: dict[str, float] = {}
    distributions: dict[str, Distribution] = {}
    gauges: dict[str, GaugeSeries] = {}
    unmatched: dict[str, int] = {}

    for metric_name, spec in schema.metrics.items():
        kind = spec.get("type")
        unit = spec.get("unit", "")

        if kind == "event_rate":
            n = counts.get(spec["event"], 0)
            rates[metric_name] = n / duration_s if duration_s else math.nan

        elif kind == "period":
            evs = by_name.get(spec["event"], [])
            dist = Distribution(unit)
            scale = _TIME_SCALE.get(unit, 1.0)
            for prev, cur in zip(evs, evs[1:]):
                dist.values.append((cur.ts_ns - prev.ts_ns) * scale)
            distributions[metric_name] = dist

        elif kind == "delta":
            dist, missing = _delta(by_name, spec, unit)
            distributions[metric_name] = dist
            if missing:
                unmatched[metric_name] = missing

        elif kind == "event_sum_rate":
            total = sum(e.args.get(spec["value"], 0) for e in by_name.get(spec["event"], []))
            total *= spec.get("scale", 1)
            rates[metric_name] = total / duration_s if duration_s else math.nan

        elif kind == "distribution":
            dist = Distribution(unit)
            dist.values = [
                float(e.args.get(spec["value"], 0)) for e in by_name.get(spec["event"], [])
            ]
            distributions[metric_name] = dist

        elif kind == "gauge":
            series = GaugeSeries(unit)
            for e in by_name.get(spec["event"], []):
                if spec["value"] in e.args:
                    series.samples.append((e.ts_ns, e.args[spec["value"]]))
            gauges[metric_name] = series

    total_events = len(trace.events)
    histogram = [
        (
            name,
            n,
            n / duration_s if duration_s else math.nan,
            100.0 * n / total_events if total_events else 0.0,
        )
        for name, n in Counter({k: v for k, v in counts.items()}).most_common()
    ]

    return MetricResults(
        counts=counts,
        rates=rates,
        distributions=distributions,
        gauges=gauges,
        event_histogram=histogram,
        unmatched=unmatched,
        duration_s=duration_s,
    )


def _delta(by_name, spec, unit) -> tuple[Distribution, int]:
    """Pair start/end events on a correlation key and measure the gap."""
    scale = _TIME_SCALE.get(unit, 1.0)
    dist = Distribution(unit)
    matched, unpaired = 0, 0
    for begin, end in pair_events(by_name, spec["start"], spec["end"], spec["key"]):
        dist.values.append((end.ts_ns - begin.ts_ns) * scale)
        matched += 1
    unpaired = len(by_name.get(spec["start"], [])) - matched
    return dist, max(0, unpaired)


def pair_events(
    by_name: dict[str, list[NormalizedEvent]],
    begin_name: str,
    end_name: str,
    key: str,
    where: dict[str, object] | None = None,
) -> Iterator[tuple[NormalizedEvent, NormalizedEvent]]:
    """Yield (begin, end) pairs correlated on `key`, in chronological order.

    The two streams are merged and walked in time order rather than collecting
    all begins first: a correlation key is only unique within one pass through
    the pipeline, and it repeats whenever the encoder restarts and the frame
    counter is reset. Matching against a later begin would otherwise produce
    negative durations. A begin whose end was lost to the ring wrapping (or
    vice versa) is simply dropped.
    """
    begins = by_name.get(begin_name, [])
    ends = by_name.get(end_name, [])
    if not begins or not ends:
        return

    def matches(event: NormalizedEvent) -> bool:
        return all(event.args.get(name) == value for name, value in (where or {}).items())

    merged = [(e.ts_ns, e.order, 0, e) for e in begins if key in e.args and matches(e)]
    merged += [(e.ts_ns, e.order, 1, e) for e in ends if key in e.args and matches(e)]
    merged.sort(key=lambda item: (item[0], item[1], item[2]))

    pending: dict[int, NormalizedEvent] = {}
    for _ts, _order, is_end, event in merged:
        k = event.args[key]
        if not is_end:
            pending[k] = event
        else:
            begin = pending.pop(k, None)
            if begin is not None:
                yield begin, event
