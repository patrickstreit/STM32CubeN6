"""Minimal protobuf (protozero) writer.

Only the wire-format primitives needed to emit a Perfetto trace are
implemented, which keeps the converter dependency-free.
"""

from __future__ import annotations

import struct

_WIRE_VARINT = 0
_WIRE_FIXED64 = 1
_WIRE_LEN = 2


def _varint(value: int) -> bytes:
    if value < 0:
        value += 1 << 64  # two's complement, as protobuf does for int32/int64
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _tag(field: int, wire: int) -> bytes:
    return _varint((field << 3) | wire)


class Message:
    """A protobuf message being built in memory."""

    __slots__ = ("_buf",)

    def __init__(self) -> None:
        self._buf = bytearray()

    def varint(self, field: int, value: int) -> "Message":
        self._buf += _tag(field, _WIRE_VARINT) + _varint(value)
        return self

    def bool(self, field: int, value: bool) -> "Message":
        return self.varint(field, 1 if value else 0)

    def fixed64(self, field: int, value: int) -> "Message":
        self._buf += _tag(field, _WIRE_FIXED64) + struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)
        return self

    def bytes(self, field: int, value: bytes) -> "Message":
        self._buf += _tag(field, _WIRE_LEN) + _varint(len(value)) + value
        return self

    def string(self, field: int, value: str) -> "Message":
        return self.bytes(field, value.encode("utf-8"))

    def submessage(self, field: int, msg: "Message") -> "Message":
        return self.bytes(field, msg.serialize())

    def serialize(self) -> bytes:
        return bytes(self._buf)
