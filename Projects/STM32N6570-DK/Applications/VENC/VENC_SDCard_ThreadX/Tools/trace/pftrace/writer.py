"""Perfetto native trace writer built on the protozero primitives.

Field numbers are taken from the Perfetto protos:
    trace_packet.proto, track_descriptor.proto, track_event.proto,
    thread_descriptor.proto, process_descriptor.proto, counter_descriptor.proto
"""

from __future__ import annotations

import itertools

from .protozero import Message

# --- TracePacket ------------------------------------------------------------
_TP_TIMESTAMP = 8
_TP_TRACK_EVENT = 11
_TP_TRACK_DESCRIPTOR = 60
_TP_TRUSTED_PACKET_SEQUENCE_ID = 10

# --- TrackDescriptor --------------------------------------------------------
_TD_UUID = 1
_TD_NAME = 2
_TD_PROCESS = 3
_TD_THREAD = 4
_TD_PARENT_UUID = 5
_TD_COUNTER = 8
_TD_DESCRIPTION = 14

# --- ProcessDescriptor / ThreadDescriptor -----------------------------------
_PD_PID = 1
_PD_PROCESS_NAME = 6
_THD_PID = 1
_THD_TID = 2
_THD_THREAD_NAME = 5

# --- CounterDescriptor ------------------------------------------------------
_CD_UNIT = 3
_CD_UNIT_NAME = 6

# --- TrackEvent -------------------------------------------------------------
_TE_DEBUG_ANNOTATIONS = 4
_TE_TYPE = 9
_TE_TRACK_UUID = 11
_TE_NAME = 23
_TE_COUNTER_VALUE = 30
_TE_FLOW_IDS = 47
_TE_TERMINATING_FLOW_IDS = 48

TYPE_SLICE_BEGIN = 1
TYPE_SLICE_END = 2
TYPE_INSTANT = 3
TYPE_COUNTER = 4

# --- DebugAnnotation --------------------------------------------------------
_DA_UINT_VALUE = 3
_DA_INT_VALUE = 4
_DA_STRING_VALUE = 6
_DA_NAME = 10

UNIT_UNSPECIFIED = 0
UNIT_TIME_NS = 1
UNIT_COUNT = 2
UNIT_SIZE_BYTES = 3

_SEQUENCE_ID = 1


class PerfettoTraceWriter:
    """Accumulates TracePackets and serialises them as a `.pftrace` file."""

    def __init__(self, process_name: str = "STM32N6570-DK", pid: int = 1) -> None:
        self._packets: list[bytes] = []
        self._uuids = itertools.count(1)
        self._pid = pid
        self.process_uuid = next(self._uuids)

        desc = Message()
        desc.varint(_TD_UUID, self.process_uuid)
        desc.submessage(
            _TD_PROCESS,
            Message().varint(_PD_PID, pid).string(_PD_PROCESS_NAME, process_name),
        )
        self._emit_descriptor(desc)

    # -- track creation ----------------------------------------------------

    def add_thread_track(self, name: str, tid: int) -> int:
        uuid = next(self._uuids)
        desc = Message()
        desc.varint(_TD_UUID, uuid)
        desc.varint(_TD_PARENT_UUID, self.process_uuid)
        desc.submessage(
            _TD_THREAD,
            Message()
            .varint(_THD_PID, self._pid)
            .varint(_THD_TID, tid)
            .string(_THD_THREAD_NAME, name),
        )
        self._emit_descriptor(desc)
        return uuid

    def add_track(
        self,
        name: str,
        parent_uuid: int | None = None,
        description: str | None = None,
    ) -> int:
        uuid = next(self._uuids)
        desc = Message()
        desc.varint(_TD_UUID, uuid)
        desc.varint(_TD_PARENT_UUID, self.process_uuid if parent_uuid is None else parent_uuid)
        desc.string(_TD_NAME, name)
        if description:
            desc.string(_TD_DESCRIPTION, description)
        self._emit_descriptor(desc)
        return uuid

    def add_counter_track(
        self,
        name: str,
        unit: int = UNIT_UNSPECIFIED,
        unit_name: str | None = None,
        description: str | None = None,
    ) -> int:
        uuid = next(self._uuids)
        counter = Message()
        if unit:
            counter.varint(_CD_UNIT, unit)
        if unit_name:
            counter.string(_CD_UNIT_NAME, unit_name)

        desc = Message()
        desc.varint(_TD_UUID, uuid)
        desc.varint(_TD_PARENT_UUID, self.process_uuid)
        desc.string(_TD_NAME, name)
        if description:
            desc.string(_TD_DESCRIPTION, description)
        desc.submessage(_TD_COUNTER, counter)
        self._emit_descriptor(desc)
        return uuid

    # -- events ------------------------------------------------------------

    def slice_begin(
        self,
        ts_ns: int,
        track_uuid: int,
        name: str,
        annotations: dict | None = None,
        flow_ids: list[int] | None = None,
    ) -> None:
        ev = Message()
        ev.varint(_TE_TYPE, TYPE_SLICE_BEGIN)
        ev.varint(_TE_TRACK_UUID, track_uuid)
        ev.string(_TE_NAME, name)
        self._add_annotations(ev, annotations)
        self._add_flows(ev, flow_ids, terminating=False)
        self._emit_event(ts_ns, ev)

    def slice_end(
        self,
        ts_ns: int,
        track_uuid: int,
        annotations: dict | None = None,
        flow_ids: list[int] | None = None,
        terminating_flow_ids: list[int] | None = None,
    ) -> None:
        ev = Message()
        ev.varint(_TE_TYPE, TYPE_SLICE_END)
        ev.varint(_TE_TRACK_UUID, track_uuid)
        self._add_annotations(ev, annotations)
        self._add_flows(ev, flow_ids, terminating=False)
        self._add_flows(ev, terminating_flow_ids, terminating=True)
        self._emit_event(ts_ns, ev)

    def instant(
        self,
        ts_ns: int,
        track_uuid: int,
        name: str,
        annotations: dict | None = None,
        flow_ids: list[int] | None = None,
        terminating_flow_ids: list[int] | None = None,
    ) -> None:
        ev = Message()
        ev.varint(_TE_TYPE, TYPE_INSTANT)
        ev.varint(_TE_TRACK_UUID, track_uuid)
        ev.string(_TE_NAME, name)
        self._add_annotations(ev, annotations)
        self._add_flows(ev, flow_ids, terminating=False)
        self._add_flows(ev, terminating_flow_ids, terminating=True)
        self._emit_event(ts_ns, ev)

    def counter(self, ts_ns: int, track_uuid: int, value: int) -> None:
        ev = Message()
        ev.varint(_TE_TYPE, TYPE_COUNTER)
        ev.varint(_TE_TRACK_UUID, track_uuid)
        ev.varint(_TE_COUNTER_VALUE, int(value))
        self._emit_event(ts_ns, ev)

    # -- output ------------------------------------------------------------

    def serialize(self) -> bytes:
        # Trace.packet is field 1, repeated TracePacket.
        out = bytearray()
        for packet in self._packets:
            out += b"\x0a" + _len_prefix(len(packet)) + packet
        return bytes(out)

    def write(self, path) -> None:
        with open(path, "wb") as fh:
            fh.write(self.serialize())

    # -- internals ---------------------------------------------------------

    def _emit_descriptor(self, desc: Message) -> None:
        packet = Message()
        packet.submessage(_TP_TRACK_DESCRIPTOR, desc)
        packet.varint(_TP_TRUSTED_PACKET_SEQUENCE_ID, _SEQUENCE_ID)
        self._packets.append(packet.serialize())

    def _emit_event(self, ts_ns: int, ev: Message) -> None:
        packet = Message()
        packet.varint(_TP_TIMESTAMP, int(ts_ns))
        packet.submessage(_TP_TRACK_EVENT, ev)
        packet.varint(_TP_TRUSTED_PACKET_SEQUENCE_ID, _SEQUENCE_ID)
        self._packets.append(packet.serialize())

    @staticmethod
    def _add_annotations(ev: Message, annotations: dict | None) -> None:
        if not annotations:
            return
        for key, value in annotations.items():
            ann = Message().string(_DA_NAME, str(key))
            if isinstance(value, str):
                ann.string(_DA_STRING_VALUE, value)
            elif isinstance(value, int) and value < 0:
                ann.varint(_DA_INT_VALUE, value)
            else:
                ann.varint(_DA_UINT_VALUE, int(value))
            ev.submessage(_TE_DEBUG_ANNOTATIONS, ann)

    @staticmethod
    def _add_flows(ev: Message, flow_ids: list[int] | None, terminating: bool) -> None:
        if not flow_ids:
            return
        field = _TE_TERMINATING_FLOW_IDS if terminating else _TE_FLOW_IDS
        for fid in flow_ids:
            ev.fixed64(field, fid)


def _len_prefix(length: int) -> bytes:
    out = bytearray()
    while True:
        byte = length & 0x7F
        length >>= 7
        if length:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)
