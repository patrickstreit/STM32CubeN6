"""Parser for a raw TraceX buffer dump (ThreadX 6.4.0).

Layout, per Middlewares/ST/threadx/common/inc/tx_trace.h:

    [TX_TRACE_HEADER            ]  48 bytes
    [TX_TRACE_OBJECT_ENTRY  * n ]  16 + object_name_size bytes each
    [TX_TRACE_BUFFER_ENTRY  * m ]  32 bytes each

All pointers stored in the header are absolute target addresses; the header's
`trace_base_address` equals the address of the buffer that was handed to
tx_trace_enable(). Offsets into the dump are therefore `pointer - base`, which
makes the parser independent of where the buffer was linked. Every structural
size is derived from the header rather than hard-coded.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from . import events as ev

TRACE_VALID = 0x54585442  # 'TXTB'
BUFFER_ENTRY_SIZE = 32


class TraceXFormatError(RuntimeError):
    pass


@dataclass
class TraceHeader:
    id: int
    timer_valid_mask: int
    trace_base_address: int
    registry_start_pointer: int
    object_name_size: int
    registry_end_pointer: int
    buffer_start_pointer: int
    buffer_end_pointer: int
    buffer_current_pointer: int

    @property
    def object_entry_size(self) -> int:
        return 16 + self.object_name_size


@dataclass
class TraceObject:
    available: bool
    type_id: int
    thread_pointer: int
    param_1: int
    param_2: int
    name: str

    @property
    def type_name(self) -> str:
        return ev.OBJECT_TYPES.get(self.type_id, f"type{self.type_id}")


@dataclass
class RawEvent:
    order: int
    thread_pointer: int
    thread_priority: int
    event_id: int
    raw_timestamp: int
    info: tuple[int, int, int, int]
    timestamp: int = 0  # unwrapped, monotonically increasing

    @property
    def is_isr(self) -> bool:
        return self.thread_pointer == ev.THREAD_PTR_ISR

    @property
    def is_init(self) -> bool:
        return self.thread_pointer == ev.THREAD_PTR_INIT

    @property
    def is_user(self) -> bool:
        return ev.USER_EVENT_START <= self.event_id <= ev.USER_EVENT_END


@dataclass
class TraceXDump:
    header: TraceHeader
    objects: list[TraceObject]
    events: list[RawEvent]
    byte_order: str
    wraps: int = 0
    buffer_wrapped: bool = False
    capacity: int = 0
    names: dict[int, str] = field(default_factory=dict)

    def object_name(self, pointer: int) -> str | None:
        return self.names.get(pointer)


def parse(data: bytes) -> TraceXDump:
    byte_order = _detect_byte_order(data)
    header = _parse_header(data, byte_order)

    base = header.trace_base_address
    entry_size = header.object_entry_size

    def offset(pointer: int, what: str) -> int:
        off = pointer - base
        if not 0 <= off <= len(data):
            raise TraceXFormatError(
                f"{what} pointer 0x{pointer:08X} resolves to offset {off}, "
                f"outside the {len(data)} byte dump. Was the whole buffer captured?"
            )
        return off

    objects = _parse_objects(
        data,
        byte_order,
        offset(header.registry_start_pointer, "registry start"),
        offset(header.registry_end_pointer, "registry end"),
        entry_size,
        header.object_name_size,
    )

    buf_start = offset(header.buffer_start_pointer, "buffer start")
    buf_end = offset(header.buffer_end_pointer, "buffer end")
    current = offset(header.buffer_current_pointer, "buffer current")
    capacity = (buf_end - buf_start) // BUFFER_ENTRY_SIZE

    raw_events, wrapped = _parse_events(
        data, byte_order, buf_start, buf_end, current, capacity
    )
    wraps = _unwrap_timestamps(raw_events, header.timer_valid_mask)

    names = {
        obj.thread_pointer: obj.name
        for obj in objects
        if not obj.available and obj.thread_pointer
    }

    return TraceXDump(
        header=header,
        objects=objects,
        events=raw_events,
        byte_order=byte_order,
        wraps=wraps,
        buffer_wrapped=wrapped,
        capacity=capacity,
        names=names,
    )


# ---------------------------------------------------------------------------


def _detect_byte_order(data: bytes) -> str:
    if len(data) < 4:
        raise TraceXFormatError("dump is too small to contain a TraceX header")
    if struct.unpack_from("<I", data, 0)[0] == TRACE_VALID:
        return "<"
    if struct.unpack_from(">I", data, 0)[0] == TRACE_VALID:
        return ">"
    got = struct.unpack_from("<I", data, 0)[0]
    raise TraceXFormatError(
        f"not a TraceX buffer: expected magic 0x{TRACE_VALID:08X} ('TXTB') "
        f"at offset 0, found 0x{got:08X}"
    )


def _parse_header(data: bytes, bo: str) -> TraceHeader:
    fmt = bo + "IIII" + "HH" + "IIIIIII"
    if len(data) < struct.calcsize(fmt):
        raise TraceXFormatError("dump is truncated inside the TraceX header")
    (
        hid,
        mask,
        base,
        reg_start,
        _reserved1,
        name_size,
        reg_end,
        buf_start,
        buf_end,
        buf_current,
        _r2,
        _r3,
        _r4,
    ) = struct.unpack_from(fmt, data, 0)
    return TraceHeader(
        id=hid,
        timer_valid_mask=mask or 0xFFFFFFFF,
        trace_base_address=base,
        registry_start_pointer=reg_start,
        object_name_size=name_size,
        registry_end_pointer=reg_end,
        buffer_start_pointer=buf_start,
        buffer_end_pointer=buf_end,
        buffer_current_pointer=buf_current,
    )


def _parse_objects(
    data: bytes, bo: str, start: int, end: int, entry_size: int, name_size: int
) -> list[TraceObject]:
    objects = []
    for off in range(start, end, entry_size):
        available, type_id, _r1, _r2 = struct.unpack_from(bo + "BBBB", data, off)
        thread_ptr, p1, p2 = struct.unpack_from(bo + "III", data, off + 4)
        raw_name = data[off + 16 : off + 16 + name_size]
        name = raw_name.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        objects.append(
            TraceObject(
                available=bool(available),
                type_id=type_id,
                thread_pointer=thread_ptr,
                param_1=p1,
                param_2=p2,
                name=name,
            )
        )
    return objects


def _parse_events(
    data: bytes, bo: str, start: int, end: int, current: int, capacity: int
) -> tuple[list[RawEvent], bool]:
    """Read the ring starting at the oldest entry.

    `buffer_current_pointer` points at the entry that will be overwritten next,
    i.e. the oldest one once the ring has wrapped. If it has not wrapped yet
    those entries are still marked invalid and are simply dropped.
    """
    entries: list[RawEvent] = []
    order = 0
    fmt = bo + "8I"
    start_index = (current - start) // BUFFER_ENTRY_SIZE

    for i in range(capacity):
        off = start + ((start_index + i) % capacity) * BUFFER_ENTRY_SIZE
        thread_ptr, prio, event_id, ts, i1, i2, i3, i4 = struct.unpack_from(fmt, data, off)
        if event_id == ev.INVALID_EVENT:
            continue
        entries.append(
            RawEvent(
                order=order,
                thread_pointer=thread_ptr,
                thread_priority=prio,
                event_id=event_id,
                raw_timestamp=ts,
                info=(i1, i2, i3, i4),
            )
        )
        order += 1

    wrapped = len(entries) == capacity
    return entries, wrapped


def _unwrap_timestamps(entries: list[RawEvent], mask: int) -> int:
    """Extend the 32-bit DWT cycle counter into a monotonic value.

    The ring is written in chronological order, so a timestamp that goes
    backwards means the hardware counter rolled over. At 800 MHz this happens
    roughly every 5.37 s, i.e. several times in a typical capture.
    """
    if not entries:
        return 0
    modulo = mask + 1
    wraps = 0
    previous = entries[0].raw_timestamp
    for entry in entries:
        ts = entry.raw_timestamp & mask
        if ts < previous:
            wraps += 1
        previous = ts
        entry.timestamp = ts + wraps * modulo

    base = entries[0].timestamp
    for entry in entries:
        entry.timestamp -= base
    return wraps
