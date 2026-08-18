#!/usr/bin/env python3
"""Self-test: build a synthetic TraceX buffer image and run the full chain.

The image is laid out byte-for-byte the way tx_trace_enable() lays out the real
buffer, so this exercises the parser, the metric engine and the Perfetto writer
without hardware.

    python selftest.py [--keep out.pftrace]
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from instrumentation import metrics as metric_engine  # noqa: E402
from instrumentation.model import normalize  # noqa: E402
from instrumentation.schema import Schema  # noqa: E402
from pftrace.exporter import PerfettoExporter  # noqa: E402
from tracex import parser as tracex_parser  # noqa: E402

BASE = 0x90200000
NAME_SIZE = 32
OBJECT_ENTRY_SIZE = 16 + NAME_SIZE
HEADER_SIZE = 48
ENTRY_SIZE = 32
CPU_HZ = 800_000_000

VENC_THREAD = 0x34001000
SD_THREAD = 0x34002000

TX_ISR_ENTER, TX_ISR_EXIT = 3, 4
TX_QUEUE_SEND, TX_QUEUE_RECEIVE = 69, 68
FX_FILE_WRITE = 254


class Builder:
    def __init__(self, capacity: int, registry_entries: int = 8) -> None:
        self.capacity = capacity
        self.registry_entries = registry_entries
        self.objects: list[tuple[int, int, str, int, int]] = []
        self.events: list[tuple[int, int, int, int, tuple]] = []

    def add_object(self, type_id: int, pointer: int, name: str, p1: int = 0, p2: int = 0) -> None:
        self.objects.append((type_id, pointer, name, p1, p2))

    def add_event(self, thread_ptr, prio, event_id, ts, info) -> None:
        self.events.append((thread_ptr, prio, event_id, ts & 0xFFFFFFFF, info))

    def build(self) -> bytes:
        reg_start = BASE + HEADER_SIZE
        reg_end = reg_start + self.registry_entries * OBJECT_ENTRY_SIZE
        buf_start = reg_end
        buf_end = buf_start + self.capacity * ENTRY_SIZE

        n = len(self.events)
        wrapped = n >= self.capacity
        kept = self.events[-self.capacity:] if wrapped else self.events
        # Oldest entry sits at the current pointer once the ring has wrapped.
        write_index = (n % self.capacity) if wrapped else n
        current = buf_start + (write_index % self.capacity) * ENTRY_SIZE if wrapped else buf_start

        out = bytearray(buf_end - BASE)
        struct.pack_into(
            "<IIIIHHIIIIIII",
            out,
            0,
            0x54585442,
            0xFFFFFFFF,
            BASE,
            reg_start,
            0,
            NAME_SIZE,
            reg_end,
            buf_start,
            buf_end,
            current,
            0xAAAAAAAA,
            0xBBBBBBBB,
            0xCCCCCCCC,
        )

        for i in range(self.registry_entries):
            off = (reg_start - BASE) + i * OBJECT_ENTRY_SIZE
            if i < len(self.objects):
                type_id, pointer, name, p1, p2 = self.objects[i]
                struct.pack_into("<BBBB", out, off, 0, type_id, 0, 0)
                struct.pack_into("<III", out, off + 4, pointer, p1, p2)
                out[off + 16 : off + 16 + len(name)] = name.encode("ascii")
            else:
                struct.pack_into("<BBBB", out, off, 1, 0, 0, 0)

        for i in range(self.capacity):
            off = (buf_start - BASE) + i * ENTRY_SIZE
            struct.pack_into("<8I", out, off, 0, 0, 0xFFFFFFFF, 0, 0, 0, 0, 0)

        start_slot = write_index if wrapped else 0
        for i, (thread_ptr, prio, event_id, ts, info) in enumerate(kept):
            slot = (start_slot + i) % self.capacity
            off = (buf_start - BASE) + slot * ENTRY_SIZE
            struct.pack_into("<8I", out, off, thread_ptr, prio, event_id, ts, *info)

        return bytes(out)


def synthesize(schema: Schema, frames: int = 200, capacity: int = 4096) -> bytes:
    ids = {name: ev.id for name, ev in schema.events_by_name.items()}
    b = Builder(capacity=capacity)
    b.add_object(1, VENC_THREAD, "VENC App Thread", 0x34010000, 8000)
    b.add_object(1, SD_THREAD, "SDCard App Thread", 0x34020000, 4000)
    b.add_object(3, 0x34003000, "ENC frame queue", 15, 5)

    us = CPU_HZ // 1_000_000
    t = 1_000_000  # start mid-range so the counter wraps during the run

    b.add_event(0xF0F0F0F0, 0, ids["SCHEMA_INFO"], t,
                (schema.version, schema.hash, CPU_HZ, 100))
    t += 1000 * us

    frame_period_us = 33_333
    for frame_id in range(1, frames + 1):
        jitter = (frame_id % 7) * 120
        t += (frame_period_us + jitter) * us

        # Capture ISR
        b.add_event(0xFFFFFFFF, VENC_THREAD, TX_ISR_ENTER, t, (0, 78, 1, 0))
        b.add_event(0xFFFFFFFF, VENC_THREAD, ids["FRAME_CAPTURED"], t + 2 * us,
                    (frame_id, 1, frame_id - 1, 0))
        b.add_event(0xFFFFFFFF, VENC_THREAD, TX_ISR_EXIT, t + 5 * us, (0, 78, 1, 0))

        if frame_id % 97 == 0:
            b.add_event(VENC_THREAD, 12, ids["FRAME_DROPPED"], t + 20 * us,
                        (frame_id, frame_id // 97, 1, 0))
            continue

        coding = 0 if frame_id % 30 == 1 else 1
        queue_level = (frame_id % 4)
        encode_us = 8000 + (frame_id % 11) * 380
        size = 24_000 if coding == 0 else 6_000 + (frame_id % 13) * 220

        b.add_event(VENC_THREAD, 12, ids["VENC_SUBMITTED"], t + 30 * us,
                    (frame_id, coding, queue_level, 65536))
        te = t + (30 + encode_us) * us
        b.add_event(VENC_THREAD, 12, ids["VENC_DONE"], te,
                    (frame_id, size, queue_level + 1, coding))
        b.add_event(VENC_THREAD, 12, TX_QUEUE_SEND, te + 2 * us,
                    (0x34003000, 0, 0, queue_level + 1))

        # SD writer picks it up.
        tw = te + 200 * us
        b.add_event(SD_THREAD, 12, TX_QUEUE_RECEIVE, tw, (0x34003000, 0, 0, queue_level))
        b.add_event(SD_THREAD, 12, ids["FRAME_WRITE_BEGIN"], tw + 5 * us,
                    (frame_id, size, frame_id // 300, frame_id % 300))
        write_us = 1500 + (frame_id % 17) * 260 + (9000 if frame_id % 61 == 0 else 0)
        tend = tw + (5 + write_us) * us
        b.add_event(SD_THREAD, 12, FX_FILE_WRITE, tend - 3 * us, (0x34004000, 0, size, size))
        b.add_event(SD_THREAD, 12, ids["FRAME_WRITTEN"], tend,
                    (frame_id, size, 0, frame_id // 300))

        if frame_id % 300 == 0:
            b.add_event(SD_THREAD, 12, ids["FILE_ROTATED"], tend + 10 * us,
                        (frame_id // 300, 300, 0, 0))

    return b.build()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--keep", type=Path, help="Also write the generated dump and .pftrace here")
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--capacity", type=int, default=4096)
    args = ap.parse_args()

    schema = Schema.load()
    data = synthesize(schema, frames=args.frames, capacity=args.capacity)

    dump = tracex_parser.parse(data)
    trace = normalize(dump, schema)
    results = metric_engine.compute(trace)

    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    check(dump.header.id == 0x54585442, "header magic not recognised")
    check(trace.cpu_hz == CPU_HZ, f"cpu_hz {trace.cpu_hz} != {CPU_HZ}")
    if not dump.buffer_wrapped:
        # SCHEMA_INFO only survives while the ring still holds the start of the run.
        check(trace.schema_ok, "schema marker mismatch")
        check(trace.firmware_schema_hash == schema.hash, "schema marker not found")
    if results.duration_s > 5.4:
        check(dump.wraps >= 1, "expected at least one 32-bit timestamp wrap")
    check(
        all(a.ts_cycles <= b.ts_cycles for a, b in zip(trace.events, trace.events[1:])),
        "unwrapped timestamps are not monotonic",
    )
    check("VENC App Thread" in trace.threads.values(), "thread registry not resolved")
    check(results.counts.get("FRAME_CAPTURED", 0) > 0, "no FRAME_CAPTURED events")

    venc = results.distributions.get("venc_duration")
    check(venc is not None and venc.count > 0, "venc_duration produced no samples")
    if venc and venc.count:
        check(7500 <= venc.mean <= 13000, f"venc_duration mean {venc.mean:.0f} us out of range")

    write = results.distributions.get("write_duration")
    check(write is not None and write.count > 0, "write_duration produced no samples")

    period = results.distributions.get("frame_period")
    check(period is not None and 30000 <= period.mean <= 37000,
          "frame_period mean outside the expected 33 ms band")

    check(results.rates.get("capture_fps", 0) > 20, "capture_fps implausible")
    check(results.gauges.get("bitstream_fill") is not None, "bitstream_fill gauge missing")

    blob = PerfettoExporter(trace).build().serialize()
    check(len(blob) > 1000, "Perfetto output suspiciously small")
    check(blob[0] == 0x0A, "Perfetto output does not start with the Trace.packet tag")

    from validate_pftrace import validate

    try:
        pf = validate(blob)
    except ValueError as exc:
        failures.append(f"Perfetto output is not valid protobuf: {exc}")
    else:
        s = pf["stats"]
        check(s["type_1"] == s["type_2"], "unbalanced slice begin/end events")
        check(s["type_3"] > 0, "no instant events emitted")
        check(s["type_4"] > 0, "no counter samples emitted")
        check(s["flow_ids"] > 0, "no frame flows emitted")
        check("ISR" in pf["track_names"], "ISR track missing")
        check("VENC App Thread" in pf["track_names"], "VENC thread track missing")

    from trace_convert import print_summary
    print_summary(trace, results, dump, sys.stdout)

    if args.keep:
        args.keep.parent.mkdir(parents=True, exist_ok=True)
        args.keep.with_suffix(".trx").write_bytes(data)
        args.keep.with_suffix(".pftrace").write_bytes(blob)
        print(f"Wrote {args.keep.with_suffix('.trx')} and {args.keep.with_suffix('.pftrace')}")

    if failures:
        print("\nSELF-TEST FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("\nSelf-test passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
