"""Recorder-independent normalized event model.

A different recorder could be plugged in later by producing the same
NormalizedEvent stream; the metric engine and the Perfetto exporter only ever
see this model.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from tracex import events as txev
from tracex.parser import TraceXDump

from .schema import EventDef, Schema

CTX_ISR = "ISR"
CTX_INIT = "Initialization"


@dataclass
class NormalizedEvent:
    order: int
    ts_cycles: int
    ts_ns: int
    event_id: int
    name: str
    source: str  # "threadx" | "filex" | "app" | "unknown"
    context: str  # thread name, "ISR" or "Initialization"
    thread_pointer: int
    args: dict[str, int] = field(default_factory=dict)
    raw_info: tuple[int, int, int, int] = (0, 0, 0, 0)
    definition: EventDef | None = None

    @property
    def is_app(self) -> bool:
        return self.source == "app"


@dataclass
class NormalizedTrace:
    events: list[NormalizedEvent]
    threads: dict[int, str]
    cpu_hz: int
    schema: Schema
    dump: TraceXDump
    schema_ok: bool
    firmware_schema_version: int | None
    firmware_schema_hash: int | None

    @property
    def duration_ns(self) -> int:
        return self.events[-1].ts_ns - self.events[0].ts_ns if self.events else 0


def normalize(dump: TraceXDump, schema: Schema, cpu_hz: int | None = None) -> NormalizedTrace:
    """Turn a TraceX dump into the normalized model.

    The clock frequency is taken from the SCHEMA_INFO event when it is still
    present in the ring; otherwise the caller's value or the schema default.
    """
    fw_version, fw_hash, fw_cpu_hz = _read_schema_marker(dump, schema)

    if cpu_hz is None:
        cpu_hz = fw_cpu_hz or schema.default_cpu_hz

    schema_ok = fw_hash is None or (fw_hash == schema.hash and fw_version == schema.version)

    threads = _thread_names(dump)
    objects = _object_names(dump)
    current_thread = 0
    normalized: list[NormalizedEvent] = []

    for raw in dump.events:
        if raw.is_isr:
            context = CTX_ISR
            # For ISR records the second field holds the interrupted thread.
            if raw.thread_priority:
                current_thread = raw.thread_priority
        elif raw.is_init:
            context = CTX_INIT
        else:
            current_thread = raw.thread_pointer
            context = threads.get(raw.thread_pointer, f"thread@0x{raw.thread_pointer:08X}")

        definition = schema.events_by_id.get(raw.event_id)
        if definition is not None:
            name = definition.name
            source = "app"
            args = definition.decode(raw.info)
        else:
            native = txev.lookup(raw.event_id)
            if native is not None:
                name, labels = native
                source = "filex" if 200 <= raw.event_id < 300 else "threadx"
                args = {label: value for label, value in zip(labels, raw.info) if label}
                for label, value in zip(labels, raw.info):
                    if not label:
                        continue
                    if label.endswith("_ptr"):
                        ptr_name = objects.get(value)
                        if ptr_name is not None:
                            args[f"{label}_name"] = ptr_name
            else:
                name = f"EVENT_{raw.event_id}"
                source = "unknown"
                args = {f"info{i + 1}": v for i, v in enumerate(raw.info)}

        normalized.append(
            NormalizedEvent(
                order=raw.order,
                ts_cycles=raw.timestamp,
                ts_ns=raw.timestamp * 1_000_000_000 // cpu_hz,
                event_id=raw.event_id,
                name=name,
                source=source,
                context=context,
                thread_pointer=current_thread,
                args=args,
                raw_info=raw.info,
                definition=definition,
            )
        )

    return NormalizedTrace(
        events=normalized,
        threads=threads,
        cpu_hz=cpu_hz,
        schema=schema,
        dump=dump,
        schema_ok=schema_ok,
        firmware_schema_version=fw_version,
        firmware_schema_hash=fw_hash,
    )


def _thread_names(dump: TraceXDump) -> dict[int, str]:
    names: dict[int, str] = {}
    for obj in dump.objects:
        if obj.available or not obj.thread_pointer:
            continue
        if obj.type_name == "thread":
            names[obj.thread_pointer] = obj.name or f"thread@0x{obj.thread_pointer:08X}"
    return names


def _object_names(dump: TraceXDump) -> dict[int, str]:
    names: dict[int, str] = {}
    for obj in dump.objects:
        if obj.available or not obj.thread_pointer:
            continue
        names[obj.thread_pointer] = obj.name or f"{obj.type_name}@0x{obj.thread_pointer:08X}"
    return names


def _read_schema_marker(
    dump: TraceXDump, schema: Schema
) -> tuple[int | None, int | None, int | None]:
    marker = schema.events_by_name.get("SCHEMA_INFO")
    if marker is None:
        return None, None, None
    for raw in dump.events:
        if raw.event_id == marker.id:
            values = marker.decode(raw.info)
            return (
                values.get("schema_version"),
                values.get("schema_hash"),
                values.get("cpu_hz") or None,
            )
    return None, None, None
