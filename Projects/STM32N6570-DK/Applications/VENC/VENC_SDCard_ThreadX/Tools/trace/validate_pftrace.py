"""Structural validation of a generated .pftrace without any protobuf runtime.

Walks the wire format, checks that every TracePacket decodes cleanly and
reports what the trace contains. Used by the self-test and handy for
debugging the writer.
"""

from __future__ import annotations

import struct
import sys
from collections import Counter
from pathlib import Path

WIRE_VARINT, WIRE_FIXED64, WIRE_LEN, WIRE_FIXED32 = 0, 1, 2, 5


def read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated varint")
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def iter_fields(buf: bytes):
    pos = 0
    while pos < len(buf):
        tag, pos = read_varint(buf, pos)
        field, wire = tag >> 3, tag & 7
        if wire == WIRE_VARINT:
            value, pos = read_varint(buf, pos)
        elif wire == WIRE_FIXED64:
            value = struct.unpack_from("<Q", buf, pos)[0]
            pos += 8
        elif wire == WIRE_FIXED32:
            value = struct.unpack_from("<I", buf, pos)[0]
            pos += 4
        elif wire == WIRE_LEN:
            length, pos = read_varint(buf, pos)
            value = buf[pos : pos + length]
            if len(value) != length:
                raise ValueError(f"truncated length-delimited field {field}")
            pos += length
        else:
            raise ValueError(f"unsupported wire type {wire} for field {field}")
        yield field, wire, value


def validate(data: bytes) -> dict:
    stats = Counter()
    timestamps = []
    track_names = []

    for field, wire, value in iter_fields(data):
        if field != 1 or wire != WIRE_LEN:
            raise ValueError(f"top level must be repeated Trace.packet, saw field {field}")
        stats["packets"] += 1
        ts = None
        for pfield, _pwire, pvalue in iter_fields(value):
            if pfield == 8:
                ts = pvalue
            elif pfield == 60:  # track_descriptor
                stats["track_descriptors"] += 1
                for dfield, _dw, dvalue in iter_fields(pvalue):
                    if dfield == 2:
                        track_names.append(dvalue.decode("utf-8", "replace"))
                    elif dfield in (3, 4, 8):
                        for sfield, _sw, svalue in iter_fields(dvalue):
                            if sfield in (5, 6) and isinstance(svalue, bytes):
                                track_names.append(svalue.decode("utf-8", "replace"))
            elif pfield == 11:  # track_event
                stats["track_events"] += 1
                for efield, _ew, evalue in iter_fields(pvalue):
                    if efield == 9:
                        stats[f"type_{evalue}"] += 1
                    elif efield == 47:
                        stats["flow_ids"] += 1
                    elif efield == 48:
                        stats["terminating_flow_ids"] += 1
        if ts is not None:
            timestamps.append(ts)

    return {
        "stats": stats,
        "track_names": track_names,
        "timestamps": timestamps,
    }


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate_pftrace.py <file.pftrace>", file=sys.stderr)
        return 2
    result = validate(Path(sys.argv[1]).read_bytes())
    s = result["stats"]
    print(f"packets            : {s['packets']}")
    print(f"track descriptors  : {s['track_descriptors']}")
    print(f"track events       : {s['track_events']}")
    print(f"  slice begin      : {s['type_1']}")
    print(f"  slice end        : {s['type_2']}")
    print(f"  instant          : {s['type_3']}")
    print(f"  counter          : {s['type_4']}")
    print(f"flow ids           : {s['flow_ids']} (+{s['terminating_flow_ids']} terminating)")
    print(f"tracks             : {', '.join(result['track_names'])}")
    ts = result["timestamps"]
    if ts:
        print(f"time span          : {(max(ts) - min(ts)) / 1e9:.3f} s")
    print("Structurally valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
