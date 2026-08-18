# TraceX → Perfetto instrumentation

Single entry point for humans **and** agents working on this instrumentation.
Read the *Orientation* table first; it removes the need to search the tree.

TraceX is the only recorder and the only time base. Application events are
written as TraceX **user events** into the same ring buffer, with the same
timestamps, as the native ThreadX and FileX events. There is no second ring
buffer, no second clock domain, no streaming backend.

```
STM32N6570-DK                                   Host
─────────────                                   ────
ThreadX  ┐                                      run1.trx
FileX    ├─► TraceX ring ──► SWD upload ─────►   │
INSTR_EVENT ┘ (3 MiB PSRAM)                      ├─ tracex/parser.py           raw records
                                                 ├─ instrumentation/model.py   normalized events
                                                 ├─ instrumentation/metrics.py metrics
                                                 └─ pftrace/exporter.py  ─► run1.pftrace
```

---

## 1. Orientation — every file that matters

### Target (firmware)

| File | Purpose | Touch when… |
| --- | --- | --- |
| `Tools/instrumentation/instrumentation.yaml` | **Single source of truth.** Events, args, metrics, flows, slices. | always, first |
| `Tools/instrumentation/gen_instrumentation.py` | Generator + schema hash | changing generation rules |
| `Appli/Core/Inc/generated/instr_ids.h` | *Generated.* `instr_id_t`, `INSTR_SCHEMA_*` | never by hand |
| `Appli/Core/Inc/instrumentation.h` | `INSTR_EVENT()` macro, `INSTR_Init()` | rarely |
| `Appli/Core/Src/instrumentation.c` | **Event filter mask**, schema marker | changing which categories are traced |
| `Appli/AZURE_RTOS/App/app_azure_rtos.c` | `tx_trace_enable()` + `INSTR_Init()`, buffer size | changing ring size |
| `Appli/Core/Src/venc_app.c` | capture ISR + encode instrumentation | adding VENC/capture events |
| `Appli/Core/Src/sdcard_app.c` | SD writer instrumentation | adding storage events |
| `Appli/CMakeLists.txt` | `TX_ENABLE_EVENT_TRACE`, `FX_ENABLE_EVENT_TRACE`, source list | adding a source file |

### Host (analyzer)

| File | Purpose |
| --- | --- |
| `Tools/dump_tracex.ps1` | Reads `tracex_buffer` over SWD (address resolved from the `.map`) |
| `Tools/trace/trace_convert.py` | **CLI.** dump → statistics + `.pftrace` |
| `Tools/trace/tracex/parser.py` | TraceX binary layout, ring order, timestamp unwrap |
| `Tools/trace/tracex/events.py` | Native ThreadX/FileX event id → name + info-field labels |
| `Tools/trace/instrumentation/schema.py` | Loads generated `instrumentation.json` |
| `Tools/trace/instrumentation/model.py` | Normalized, recorder-independent event model |
| `Tools/trace/instrumentation/metrics.py` | Metric engine (rates, deltas, distributions, gauges) |
| `Tools/trace/pftrace/protozero.py` | Minimal protobuf encoder (no dependencies) |
| `Tools/trace/pftrace/writer.py` | Perfetto packet/track primitives |
| `Tools/trace/pftrace/exporter.py` | Normalized events → tracks, slices, counters, flows |
| `Tools/trace/selftest.py` | End-to-end test on a synthetic buffer, **no hardware** |
| `Tools/trace/validate_pftrace.py` | Structural check of the emitted protobuf |
| `Tools/trace/perfetto_query_snippets.sql` | Ready-made Perfetto SQL queries |

Only `instrumentation.yaml` plus the relevant `*_app.c` need editing to add a
measurement point. Everything else is generic.

---

## 2. Workflow (human)

```powershell
# 0. after editing instrumentation.yaml
python Tools/instrumentation/gen_instrumentation.py

# 1. build + flash
cmake --build build/Debug
#    then: VS Code task "Flash STM32N6 VENC (Debug)"

# 2. let the application run for the period you want to see (ring holds ~86 s)

# 3. dump the ring over SWD, without resetting the target
./Tools/dump_tracex.ps1 -Output Tools/trace/out/run1.trx

# 4. convert + summarise
python Tools/trace/trace_convert.py Tools/trace/out/run1.trx
```

Open `Tools/trace/out/run1.pftrace` at <https://ui.perfetto.dev>.
The `.trx` stays raw TraceX data, so the ST TraceX tooling still reads it.

Useful flags: `--no-native-instants` (smaller file, app events only),
`--no-perfetto` (statistics only), `--cpu-hz`, `--strict-schema`.

**Notes that cost time if unknown**
- `dump_tracex.ps1` must use `mode=HOTPLUG`. The buffer is in external PSRAM,
  only reachable while the running application keeps XSPI memory-mapped. A
  reset loses the mapping *and* the trace.
- `STM32_Programmer_CLI -u` only accepts `.bin/.hex/.srec/.s19`; the script
  uploads to a temp `.bin` and renames.
- `SCHEMA_INFO` is emitted once at startup. On a run longer than ~86 s it has
  been overwritten and the converter reports the schema as *unverified*.
  Expected, not an error.

---

## 3. Recipe — add a measurement point

Five steps. Nothing outside these files needs changing.

**1. Declare the event** in `Tools/instrumentation/instrumentation.yaml`:

```yaml
  SD_WRITE_BLOCKS:
    id: 0x1204                  # unique, 4096..65535, grouped by subsystem
    track: Storage              # Perfetto track it appears on
    description: HAL_SD_WriteBlocks_DMA() kicked off.
    args:                       # max 4
      - { name: frame_id,    type: u32 }
      - { name: start_block, type: u32 }
      - { name: block_count, type: u32 }
      - { name: hal_status,  type: i32, description: "0 == HAL_OK" }
```

> A TraceX record always reserves four `ULONG` information fields. Unused
> fields save nothing. **Fill them with everything cheaply available** instead
> of emitting a second event.

**2. Regenerate**

```powershell
python Tools/instrumentation/gen_instrumentation.py
```

**3. Emit it** at the call site:

```c
#include "instrumentation.h"

INSTR_EVENT(INSTR_ID_SD_WRITE_BLOCKS, frame_id, start_block, count, status);
```

No strings, no `printf`, no arithmetic — ISR-safe and allocation-free.
If the file is new, add it to `APP_Application_Src` in `Appli/CMakeLists.txt`.

**4. Optional — derive metrics** (host only, no reflash needed):

```yaml
metrics:
  sd_block_latency:
    type: delta                 # start/end paired on `key`
    start: SD_WRITE_BLOCKS
    end: SD_WRITE_CPLT
    key: frame_id
    unit: us
```

Metric types: `event_rate`, `event_sum_rate`, `period`, `delta`,
`distribution`, `gauge`.

**5. Optional — draw it in Perfetto**:

```yaml
slices:                         # a duration bar on a track
  SD_BLOCKS:
    track: Storage
    begin: SD_WRITE_BLOCKS
    end: SD_WRITE_CPLT
    key: frame_id
    name: sd blocks

flows:                          # arrows following a frame across tracks
  frame_pipeline:
    key: frame_id
    stages: [FRAME_CAPTURED, VENC_SUBMITTED, VENC_DONE,
             FRAME_WRITE_BEGIN, FRAME_WRITTEN]
```

Events not consumed by a `slices` entry are drawn as instants automatically.
Every `type: gauge` metric becomes a counter track automatically.

Verify without hardware: `python Tools/trace/selftest.py`.

### Changing which categories are recorded

Edit `INSTR_TRACE_FILTER_MASK` in `Appli/Core/Src/instrumentation.c`. The mask
lists the categories to **suppress**. Overridable per build with
`-DINSTR_TRACE_FILTER_MASK=...`.

---

## 4. Invariants — do not break these

1. **No second recorder, no second ring buffer, no second time base.** TraceX
   only. Revisit only with measured evidence of a TraceX limitation.
2. **No statistics on the target.** No counters, gauges, min/max/avg, periods
   or histograms in firmware. If it can be reconstructed from events, it is a
   host concern.
3. **Event ids are never written twice by hand.** Firmware and host both derive
   from `instrumentation.yaml`. Never hard-code an id in the Python converter.
4. **Hot path stays thin.** `INSTR_EVENT` only. No strings or formatting.
5. **`frame_id` is the correlation key**, and it is the existing capture counter
   `frame_received`. Do not introduce a parallel id scheme.
6. **Never edit generated files** (`instr_ids.h`, `instrumentation.json`).
   `gen_instrumentation.py --check` is the staleness gate.
7. **Timestamps must be unwrapped.** `DWT->CYCCNT` is 32-bit and wraps every
   ~5.37 s at 800 MHz. The parser handles it; keep it that way.
8. **Correlated pairing walks time order.** `frame_id` repeats when the encoder
   restarts and resets the counter; collecting all begins first yields negative
   durations. See `metrics.pair_events()`.

### Schema hash semantics

The hash covers **only what decoding a record depends on**: event ids plus the
name, type and order of the four information fields.

| Change | Hash | Reflash |
| --- | --- | --- |
| event id, arg name/type/order | changes | **yes** |
| `description`, `unit`, `enum` | unchanged | no |
| `metrics`, `flows`, `slices`, `track` | unchanged | no |

Deliberate: it lets you add a metric or retune a Perfetto track and re-analyse
an **existing** dump from already-flashed firmware.

`description` is not dead documentation — it is emitted as
`TrackDescriptor.description` and shown behind the track's help button in
Perfetto. In YAML flow mappings, quote text containing a comma:
`description: "FileX status, 0 == FX_SUCCESS"`.

---

## 5. Event filter (current state)

Set in `Appli/Core/Src/instrumentation.c` via `tx_trace_event_filter()`.

| Category | State | Rationale |
| --- | --- | --- |
| `TX_TRACE_INTERNAL_EVENTS` | kept | thread resume/suspend + ISR enter/exit → scheduler timeline |
| `TX_TRACE_QUEUE_EVENTS` | kept | `enc_frame_queue` hand-off and depth |
| `TX_TRACE_THREAD_EVENTS` | kept | create/sleep/terminate; low rate, high value |
| `TX_TRACE_USER_EVENTS` | kept | the application events |
| `FX_TRACE_FILE_EVENTS` | kept | `fx_file_write` size and result |
| `FX_TRACE_MEDIA_EVENTS` | kept | media flush/close — SD stall analysis |
| `TX_TRACE_BLOCK_POOL_EVENTS` | filtered | startup only (thread stacks) |
| `TX_TRACE_BYTE_POOL_EVENTS` | filtered | startup only |
| `TX_TRACE_EVENT_FLAGS_EVENTS` | filtered | 2×/frame, adds nothing over `FRAME_CAPTURED` |
| `TX_TRACE_INTERRUPT_CONTROL` | filtered | very high rate, no analytical value |
| `TX_TRACE_MUTEX_EVENTS` | filtered | not on the data path |
| **`TX_TRACE_SEMAPHORE_EVENTS`** | **filtered** | ⚠️ **see §8 — required for write-stall analysis** |
| `TX_TRACE_TIME_EVENTS` | filtered | tick bookkeeping |
| `TX_TRACE_TIMER_EVENTS` | filtered | tick bookkeeping |
| `FX_TRACE_INTERNAL_EVENTS` | filtered | per-sector driver I/O would dominate the ring |
| `FX_TRACE_DIRECTORY_EVENTS` | filtered | only on create/rotate |

`FX_ENABLE_EVENT_TRACE` had to be added to the Appli compile definitions —
FileX events were not recorded at all before this work.

---

## 6. Perfetto mapping

| Concept | Source |
| --- | --- |
| thread tracks + `running` slices | reconstructed from native records (limitation below) |
| ISR slices | `TX_ISR_ENTER` / `TX_ISR_EXIT`, nested |
| application slices | `slices:` block |
| instant events | all remaining application + native events |
| counter tracks | every `type: gauge` metric |
| flows | `flows:` block, keyed on `frame_id` |
| track help text | `description:` fields |

Output is native Perfetto protobuf via a dependency-free protozero encoder. The
only intermediate representation is the in-memory normalized event model, which
is what would make a different recorder pluggable later.

**Known limitation — scheduler reconstruction.** TraceX has no context-switch
record. The exporter combines (a) the thread pointer carried by every record and
(b) the *next thread* field of `TX_THREAD_RESUME`/`TX_THREAD_SUSPEND`. A thread
that runs without emitting any traced call is attributed to the previously
observed thread until the next record. Closing this would need a record emitted
from `_tx_thread_schedule` — deliberately not done.

---

## 7. Measured baseline (720p30 + SD recording, 86 s capture)

| Property | Value |
| --- | --- |
| Record size | 32 B (`TX_TRACE_BUFFER_ENTRY`) |
| Buffer | 3 MiB `.psram_bss` @ `0x90C3F800`, 98'254 records |
| Trace clock | `DWT->CYCCNT` @ 800 MHz, wraps ≈5.37 s |
| Event rate | ≈1'140 /s → **86 s of history** |
| SWD upload | ≈8 s for 3 MiB |

| Producer | Rate | Share |
| --- | --- | --- |
| `TX_THREAD_RESUME` | 467 /s | 40.9 % |
| `TX_THREAD_SUSPEND` | 467 /s | 40.9 % |
| all application events combined | ≈130 /s | **11 %** |

`TX_TRACE_INTERNAL_EVENTS` is 82 % of the ring but is kept: it is the only
source for the thread timeline, and 86 s is ample. Drop it first if longer
history is ever needed (→ roughly 8 minutes).

**Adding application events is cheap** — they cost 11 % of the ring today.

### Pipeline behaviour observed

```
venc_duration        mean  23.7 ms   p99  24.7 ms   max   209.8 ms
write_duration       mean  25.1 ms   p99 380.4 ms   max  1831.0 ms   ← stalls
capture_to_written   mean 200.3 ms   p99   1.74 s   max     2.55 s
venc_queue_level     high-watermark 16   (capacity 15 → saturated)
capture_backlog      high-watermark 39
frames captured 2'463 → encoded 2'198, dropped 17
```

The SD write path is the bottleneck; its stalls back-pressure the encoder,
which is why `captured` exceeds `encoded`.

---

## 8. Next task — locate the cause of the write stalls

Goal: explain `write_duration` p99 380 ms / max 1.8 s.

### Established call path

```
sdcard_app.c            VENC_FileX_write()
  └─ fx_file_write()                                    [FileX]
      └─ fx_stm32_sd_driver()      Middlewares/ST/filex/common/drivers/fx_stm32_sd_driver.c:52
          ├─ check_sd_status()                                                      :35   busy-wait, 10 s timeout
          │    └─ fx_stm32_sd_get_status()          Appli/Core/Src/fx_stm32_sd_driver_glue.c:83
          │         └─ HAL_SD_GetCardState() != TRANSFER                            :96
          └─ sd_write_data()                                                        :285
              ├─ per-sector loop when unaligned                                     :355  ← slow path
              ├─ fx_stm32_sd_write_blocks()                                         :365
              │    └─ HAL_SD_WriteBlocks_DMA()                              glue    :150  returns immediately
              └─ FX_STM32_SD_WRITE_CPLT_NOTIFY()                                    :373  ← BLOCKS HERE
                   └─ tx_semaphore_get(&sd_tx_semaphore, 10 s)
                                              Appli/Core/Inc/fx_stm32_sd_driver.h   :148

  IRQ: SDMMC2_IRQHandler()                                                  glue    :253
        └─ HAL_SD_IRQHandler() → HAL_SD_TxCpltCallback()                    glue    :163
             └─ tx_semaphore_put(&sd_tx_semaphore)                          glue    :171
```

### Hypotheses to discriminate

| # | Hypothesis | Distinguishing evidence |
| --- | --- | --- |
| H1 | Card-internal busy (wear levelling / erase) | long gap between `WriteBlocks_DMA` and `TxCpltCallback` |
| H2 | `check_sd_status()` polling before the write | time consumed *before* the DMA even starts |
| H3 | Unaligned buffer → per-sector loop | many small transfers per `fx_file_write` |
| H4 | `fx_media_flush()` on file rotation (every 300 frames) | stalls correlate with `FILE_ROTATED` |
| H5 | Lost IRQ / DMA stall | semaphore wait ends near the 10 s timeout, `FX_IO_ERROR` |

### Step 1 — costs nothing, do it first

**`TX_TRACE_SEMAPHORE_EVENTS` is currently filtered out**, so the single most
important wait in the system is invisible. Remove it from
`INSTR_TRACE_FILTER_MASK` in `Appli/Core/Src/instrumentation.c`, rebuild, flash,
recapture. `TX_SEMAPHORE_GET` / `TX_SEMAPHORE_PUT` on `sd_tx_semaphore` then
immediately separate H1/H5 from H2/H3/H4 — **no new events, no YAML change**.

Consider also temporarily enabling `FX_TRACE_INTERNAL_EVENTS`, whose
`FX_INTERNAL_IO_DRIVER_WRITE` (media ptr, sector, sector count, buffer) directly
settles H3. It is high-rate, so pair it with a shorter capture.

### Step 2 — add events only for what is still ambiguous

Suggested ids (`0x12xx` = storage): `SD_WRITE_BLOCKS` (start_block, block_count,
hal_status), `SD_WRITE_CPLT` (from `HAL_SD_TxCpltCallback`), `SD_STATUS_WAIT`
(poll iterations, elapsed). Follow §3.

`fx_stm32_sd_driver_glue.c` is already compiled into the application; adding
`#include "instrumentation.h"` there is all that is required.

Relevant context for that file:
- `sd_tx_semaphore` / `sd_rx_semaphore` declared at `glue:15-16`
- media sector cache is a **single 512-byte sector** (`app_filex.c:59`)
- `fx_media_flush()` at `app_filex.c:260`, called from file close
- `FX_STM32_SD_DEFAULT_TIMEOUT` = 10 s (`fx_stm32_sd_driver.h:47`)

### Step 3 — correlate in Perfetto

`Tools/trace/perfetto_query_snippets.sql` has ready queries for slowest writes,
stall-vs-rotation correlation and queue depth at stall time.

---

## 9. Development

```powershell
python Tools/trace/selftest.py                    # synthetic buffer, no hardware
python Tools/trace/selftest.py --keep out/demo    # also write .trx + .pftrace
python Tools/trace/validate_pftrace.py out/demo.pftrace
python Tools/instrumentation/gen_instrumentation.py --check   # CI staleness gate
```

`selftest.py` builds a byte-accurate TraceX image including a 32-bit clock wrap
and a ring wrap, then asserts the parser, the metric engine and the protobuf
output. Run it after every change to the host chain.
