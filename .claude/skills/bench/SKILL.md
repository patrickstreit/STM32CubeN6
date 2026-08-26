---
name: bench
description: Drive the STM32N6570-DK VENC bench from the local machine - build, flash, power-cycle the CrossLink CSI source, run console measurements, and decode TraceX traces. Use for any task in Projects/STM32N6570-DK/Applications/VENC/VENC_SDCard_ThreadX that needs the real hardware (Phase M of PLAN.md, encoder timing, capture tests).
---

# STM32N6 VENC bench workflow

Working directory for everything below:
`Projects/STM32N6570-DK/Applications/VENC/VENC_SDCard_ThreadX`.
The task list and acceptance criteria live in `PLAN.md` there; record results in
that file. Deep background on the CSI source and every pitfall listed here:
`Tools/csi/README.md`.

## Build and flash

Two CMake presets share one source tree, selected by `CSI_PROBE_MODE`:
`Debug` (encoder firmware) and `CsiProbe` (CSI probe/preview firmware).

```powershell
cd Appli
cmake --preset CsiProbe        # or Debug
cmake --build build/CsiProbe
```

Flash + debug non-interactively (ST-LINK_gdbserver + GDB, paths auto-discovered
from the STM32Cube bundle):

```powershell
./Tools/debug/stm32n6-gdb.ps1 -Build -FlashAppli -Preset CsiProbe
./Tools/debug/stm32n6-gdb.ps1 -NoFsblLoad -Interactive   # attach to what runs
./Tools/debug/stm32n6-gdb.ps1 -DryRun                    # print commands only
```

`-Ex '<gdb command>'` runs commands at the entry point; without `-Interactive`
the script detaches (resuming the target) and stops the server — safe for
scripted use. `-FlashAppli` rewrites only the application slot at `0x70100000`;
the FSBL stays, so switching between `Debug` and `CsiProbe` firmware is one
flash, no FSBL work.

## Powering the CSI source (CrossLink)

The CSI-2 source must come up AFTER the DCMIPP receiver (D-PHY clock lane never
re-presents LP-11 once streaming — see csi README §5a). Restarts are done by
cutting its power rail via a PPK2:

- `dutpower serve` must run **in its own terminal, left running** — the PPK2
  ties the rail to the serial connection; the rail lives exactly as long as
  that process. `dutpower cycle` exits with code 5 if serve is missing.
- The source takes 7–9 s to start transmitting after power returns; firmware
  waits are timeouts (up to 90 s), never fixed delays.
- The firmware prints a restart prompt; `csi-console.ps1 -OnPromptCommand
  'dutpower cycle'` answers it automatically — no human in the loop.

## Console automation

COM1, 115200 8N1. The board's UART drops characters when a command arrives
while it prints, so never write to the port raw — `Tools/debug/csi-console.ps1`
paces characters and retries on `unknown command`:

```powershell
./Tools/debug/csi-console.ps1 -OnPromptCommand 'dutpower cycle' `
    -WaitFor "first frame after" -Sequence "link 280000"
./Tools/debug/csi-console.ps1 -Sequence "probe 2500; single 0"
```

CsiProbe console commands (details in csi README §3): `link`, `probe`, `vcs`,
`dt`, `geom`, `bayer`, `errors`, `grab`, `single`, `dual`, `status`, `report`.
Measurement commands added for PLAN.md Phase M (`phase`, `mux`) follow the same
report convention: a block with a fixed prefix (`=== <NAME> RESULT ===`,
key=value lines, `=== END ===`) so results parse mechanically. `single`/`dual`
need a prior `probe` for geometry. Only ONE power-cycle is needed per session:
`link` first, everything after runs on the live link.

## Traces (TraceX → Perfetto)

Instrumentation IDs are defined in `Tools/instrumentation/instrumentation.yaml`
(regenerate headers with `Tools/instrumentation/gen_instrumentation.py`; keep
the schema hash in sync or existing `.trx` files stop decoding).

```powershell
./Tools/dump_tracex.ps1                          # pull the TraceX ring
python Tools/trace/trace_convert.py <dump.trx>   # -> Perfetto .pftrace
python -m Tools.trace.instrumentation.metrics    # summary metrics
```

`Tools/trace/perfetto_query_snippets.sql` holds ready-made queries (encode
duration, SD wait, frame intervals). `Tools/trace/selftest.py` and
`validate_pftrace.py` check the toolchain without hardware.

## Pitfalls (each cost a session to learn — csi README §8)

- **The FSBL is a RAM image**: the board boots the ROM bootloader; GDB `load`
  is what starts the system. Flashing only the Appli leaves a silent board.
- **The gdb server resets the target on connect** unless given `-g`;
  `stm32n6-gdb.ps1` passes it whenever not loading. Careless attach = a lost
  power-cycle.
- **The debugger cannot read PSRAM** (`0x90000000`) — trying kills the
  session. Print from firmware instead.
- CSI status flags are sticky and `CSI_ERR1` never re-arms — use the `errors`
  command for rates, never a one-shot `status` read.
- The permanent `IDERR` while a pipe consumes `0x2b` is explained and harmless
  (the channel also carries `0x12`/`0x2f` packets nobody claims).
- Cutting the rail only power-cycles if nothing back-feeds it: verify with
  `dutpower off` + `dutpower measure --seconds 1` ≈ 0.
