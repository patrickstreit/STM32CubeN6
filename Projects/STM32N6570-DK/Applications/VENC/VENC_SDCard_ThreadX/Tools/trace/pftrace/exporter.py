"""Maps the normalized event model onto Perfetto tracks, slices and flows."""

from __future__ import annotations

from collections import defaultdict

from instrumentation.metrics import pair_events
from instrumentation.model import CTX_INIT, CTX_ISR, NormalizedEvent, NormalizedTrace
from tracex import events as txev

from .writer import UNIT_COUNT, PerfettoTraceWriter

# Native events used to reconstruct the scheduler timeline instead of being
# drawn as ordinary instants.
_ISR_ENTER = 3
_ISR_EXIT = 4
_THREAD_RESUME = 1
_THREAD_SUSPEND = 2
_SCHEDULER_EVENT_IDS = {_ISR_ENTER, _ISR_EXIT}

_UNIT_BY_NAME = {"frames": UNIT_COUNT, "count": UNIT_COUNT}


class PerfettoExporter:
    def __init__(self, trace: NormalizedTrace, native_instants: bool = True) -> None:
        self.trace = trace
        self.native_instants = native_instants
        self.writer = PerfettoTraceWriter()
        self._thread_tracks: dict[int, int] = {}
        self._app_tracks: dict[str, int] = {}
        self._next_tid = 1

    # -- public ------------------------------------------------------------

    def build(self) -> PerfettoTraceWriter:
        self._declare_tracks()
        self._emit_scheduler()
        self._emit_native_instants()
        self._emit_app_events()
        self._emit_gauges()
        return self.writer

    # -- tracks ------------------------------------------------------------

    def _declare_tracks(self) -> None:
        for pointer, name in sorted(self.trace.threads.items(), key=lambda kv: kv[1]):
            self._thread_tracks[pointer] = self.writer.add_thread_track(name, self._next_tid)
            self._next_tid += 1

        self._isr_track = self.writer.add_thread_track(CTX_ISR, self._next_tid)
        self._next_tid += 1

        tracks = {e.definition.track for e in self.trace.events if e.definition is not None}
        tracks.update(spec["track"] for spec in self.trace.schema.slices.values())
        for name in sorted(tracks):
            self._app_tracks[name] = self.writer.add_track(
                name, description=self._track_description(name)
            )

    def _track_description(self, track: str) -> str:
        """Documentation from instrumentation.yaml, shown by the track's help button."""
        lines = []
        for ev in sorted(self.trace.schema.events_by_name.values(), key=lambda e: e.id):
            if ev.track != track:
                continue
            args = ", ".join(a.name for a in ev.args if not a.is_reserved)
            lines.append(f"{ev.name}({args})")
            if ev.description:
                lines.append(f"    {ev.description}")
            for arg in ev.args:
                if arg.description and not arg.is_reserved:
                    lines.append(f"    {arg.name}: {arg.description}")
        return "\n".join(lines)

    def _thread_track(self, pointer: int) -> int | None:
        if pointer in self._thread_tracks:
            return self._thread_tracks[pointer]
        if pointer in (0, txev.THREAD_PTR_INIT, txev.THREAD_PTR_ISR):
            return None
        # A thread that never made it into the object registry (32 slots).
        uuid = self.writer.add_thread_track(f"thread@0x{pointer:08X}", self._next_tid)
        self._next_tid += 1
        self._thread_tracks[pointer] = uuid
        return uuid

    # -- scheduler ---------------------------------------------------------

    def _emit_scheduler(self) -> None:
        """Reconstruct thread activity and ISR nesting from native events only.

        TraceX has no dedicated context-switch record, so two sources are
        combined:
          * every trace record names the thread that produced it, which bounds
            the running interval;
          * TX_THREAD_RESUME / TX_THREAD_SUSPEND carry the next thread to run
            in information field 4, which gives the exact switch instant.
        Consequence: a switch is only visible once one of the two occurs. A
        thread that runs without emitting any traced call is attributed to the
        previously observed thread until the next record appears.
        """
        current: int | None = None
        run_start = 0
        isr_stack: list[int] = []

        for event in self.trace.events:
            ts = event.ts_ns

            if event.context == CTX_ISR:
                if event.event_id == _ISR_ENTER:
                    isr_stack.append(ts)
                    self.writer.slice_begin(
                        ts,
                        self._isr_track,
                        f"ISR {event.args.get('isr_number', 0)}",
                        {"system_state": event.args.get("system_state", 0)},
                    )
                elif event.event_id == _ISR_EXIT and isr_stack:
                    isr_stack.pop()
                    self.writer.slice_end(ts, self._isr_track)
                continue

            if event.context == CTX_INIT:
                continue

            pointer = event.thread_pointer
            if pointer != current:
                current = self._switch(current, pointer, run_start, ts)
                run_start = ts

            if event.event_id in (_THREAD_RESUME, _THREAD_SUSPEND):
                nxt = event.args.get("next_thread", 0)
                if nxt and nxt != txev.INVALID_EVENT and nxt != current:
                    current = self._switch(current, nxt, run_start, ts)
                    run_start = ts

        if current is not None and self.trace.events:
            track = self._thread_track(current)
            if track is not None:
                self.writer.slice_end(self.trace.events[-1].ts_ns, track)

        while isr_stack:
            isr_stack.pop()
            self.writer.slice_end(self.trace.events[-1].ts_ns, self._isr_track)

    def _switch(self, previous: int | None, nxt: int, run_start: int, ts: int) -> int:
        if previous is not None:
            track = self._thread_track(previous)
            if track is not None:
                self.writer.slice_end(max(ts, run_start), track)
        track = self._thread_track(nxt)
        if track is not None:
            self.writer.slice_begin(ts, track, "running")
        return nxt

    # -- native instants ---------------------------------------------------

    def _emit_native_instants(self) -> None:
        if not self.native_instants:
            return
        for event in self.trace.events:
            if event.source not in ("threadx", "filex"):
                continue
            if event.event_id in _SCHEDULER_EVENT_IDS:
                continue
            track = (
                self._isr_track
                if event.context == CTX_ISR
                else self._thread_track(event.thread_pointer)
            )
            if track is None:
                continue
            self.writer.instant(event.ts_ns, track, event.name, self._annotations(event))

    # -- application events ------------------------------------------------

    def _emit_app_events(self) -> None:
        schema = self.trace.schema
        by_name: dict[str, list[NormalizedEvent]] = defaultdict(list)
        for event in self.trace.events:
            by_name[event.name].append(event)

        # Events consumed by a slice definition are not also drawn as instants.
        slice_events: set[str] = set()
        for spec in schema.slices.values():
            slice_events.add(spec["begin"])
            slice_events.add(spec["end"])

        flow_key = schema.correlation_key
        flow_stages = {
            stage
            for spec in schema.flows.values()
            for stage in spec.get("stages", [])
        }

        for spec in schema.slices.values():
            track = self._app_tracks.get(spec["track"])
            if track is None:
                continue
            begin_name, end_name = spec["begin"], spec["end"]
            for begin, end in pair_events(
                by_name, begin_name, end_name, spec["key"], spec.get("where")
            ):
                fid = self._flow_id(begin.args.get(flow_key))
                self.writer.slice_begin(
                    begin.ts_ns,
                    track,
                    spec.get("name", begin_name),
                    self._annotations(begin),
                    flow_ids=[fid] if fid and begin_name in flow_stages else None,
                )
                terminating = fid if fid and end_name == _last_stage(schema) else None
                self.writer.slice_end(
                    max(end.ts_ns, begin.ts_ns),
                    track,
                    self._annotations(end),
                    terminating_flow_ids=[terminating] if terminating else None,
                )

        for name, evs in by_name.items():
            if name in slice_events:
                continue
            definition = schema.events_by_name.get(name)
            track = self._app_tracks.get(definition.track if definition else "Application")
            if track is None:
                continue
            for event in evs:
                fid = self._flow_id(event.args.get(flow_key))
                self.writer.instant(
                    event.ts_ns,
                    track,
                    name,
                    self._annotations(event),
                    flow_ids=[fid] if fid and name in flow_stages else None,
                )

    # -- counters ----------------------------------------------------------

    def _emit_gauges(self) -> None:
        schema = self.trace.schema
        gauge_specs = {
            name: spec for name, spec in schema.metrics.items() if spec.get("type") == "gauge"
        }
        if not gauge_specs:
            return

        by_name: dict[str, list] = defaultdict(list)
        for event in self.trace.events:
            if event.is_app:
                by_name[event.name].append(event)

        for name, spec in gauge_specs.items():
            unit = spec.get("unit", "")
            source = self.trace.schema.events_by_name.get(spec["event"])
            track = self.writer.add_counter_track(
                name,
                unit=_UNIT_BY_NAME.get(unit, 0),
                unit_name=unit or None,
                description=self._gauge_description(name, spec, source),
            )
            for event in by_name.get(spec["event"], []):
                if spec["value"] in event.args:
                    self.writer.counter(event.ts_ns, track, event.args[spec["value"]])

    # -- helpers -----------------------------------------------------------

    @staticmethod
    def _gauge_description(name: str, spec: dict, source) -> str:
        text = f"{spec['value']} sampled at every {spec['event']}"
        if source is not None:
            for arg in source.args:
                if arg.name == spec["value"] and arg.description:
                    text += f"\n{arg.description}"
        return text

    def _annotations(self, event) -> dict:
        out = dict(event.args)
        definition = event.definition
        if definition is not None:
            for arg in definition.args:
                label = self.trace.schema.enum_label(arg.enum, out.get(arg.name, -1))
                if label:
                    out[arg.name] = f"{out[arg.name]} ({label})"
        return out

    @staticmethod
    def _flow_id(key: int | None) -> int | None:
        if key is None:
            return None
        return (int(key) + 1) & 0xFFFFFFFFFFFFFFFF


def _last_stage(schema) -> str | None:
    for spec in schema.flows.values():
        stages = spec.get("stages") or []
        if stages:
            return stages[-1]
    return None
