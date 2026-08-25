# CSI-2 probe and dual-virtual-channel preview

A minimal firmware variant of this project that brings up **only** the
DCMIPP/CSI-2 receiver. No encoder, no SD card, no FileX. It exists to answer two
questions about a CSI-2 source that the STM32 does not control:

1. **Which CSI settings work?** Lane count, lane mapping, D-PHY bitrate, which
   virtual channels are present, what data type each carries, and the frame
   geometry.
2. **Can VC0 and VC1 be shown side by side on the DK display?** Yes, with one
   caveat imposed by the silicon - see [Why the two channels
   alternate](#why-the-two-channels-alternate).

Status, from two hardware sessions so far:

- A link exists at **1250 Mbit/s per lane, 2 lanes, physical mapping** - clock,
  lane sync and a first frame, in that order. It is **marginal**: uncorrectable
  header ECC, payload CRC and D-PHY SOT errors all appear. `refine` is the
  command for picking a better profile.
- The failure that produced no clock at any of 72 settings was **start order**,
  not bitrate; see [Start order](#5a-start-order-the-source-must-come-up-after-the-receiver).
- The source takes **6 to 8 s** to start transmitting after a restart. Every
  wait for it is a timeout that ends on the clock, never a fixed delay.
- The characterisation and preview stages have not yet been exercised against a
  healthy link.

Everything below that is a measurement rather than a datasheet fact is marked as
such.

---

## 1. Build and flash

The probe is a CMake preset next to the normal `Debug` build; both live in the
same source tree and are selected by `CSI_PROBE_MODE`.

```powershell
cd Appli
cmake --preset CsiProbe
cmake --build build/CsiProbe
```

Output lands in `Appli/build/CsiProbe/` with the same signed `.bin` / `.elf`
post-build steps as the normal build.

To flash it, run the VS Code task **Flash STM32N6 VENC (Appli only)** and pick
`CsiProbe`. Only the application slot at `0x70100000` is rewritten; the FSBL
stays as it is, because Appli compile definitions do not affect it. That also
makes switching back to the encoder firmware a matter of running the same task
and picking `Debug` - no FSBL reflash either way.

`CsiProbe` is an Appli-only preset and has no FSBL counterpart, which is why it
does not appear in **Flash STM32N6 VENC (FSBL + Appli)**: that task flashes both
images from one top-level preset, where the root `CMakeLists.txt` builds
`FSBL/build/<preset>` and `Appli/build/<preset>` together.

Footprint for reference: 109 KB flash, 3.8 MB of the 16 MB PSRAM (3 MB of that
is the TraceX ring, 768 KB the framebuffer). The normal build is unchanged.

Console is COM1 at 115200 8N1, same as the main application.

---

## 2. What it does at start-up

```
BSP_CAMERA_Init()      power the camera connector, DCMIPP clocks, HAL_DCMIPP_Init
csi_probe_run()        known-good setting -> sweep if needed -> characterise
csi_probe_print_report()
preview_best_effort()  two channels side by side, or one centred, or nothing
```

`csi_probe_run()` first tries the setting the encoder application uses, 1250
Mbit/s per lane over two lanes, and restarts the source against it. That is very
often the answer, and it is worth trying first because a full sweep is expensive:
every combination needs the source restarted, which costs its reset sequence plus
its boot time. Only if that finds no link does it sweep all 21 bitrates x 2 lane
counts x 2 mappings, which takes a couple of minutes and prints an estimate
before it starts.

Example of the report it prints:

```
=== CSI-2 probe result ===
D-PHY : 2500 Mbit/s per lane, 2 lane(s), physical mapping
VC0   : 30.0 fps, DT 0x2b RAW10, 1080 lines, 2400 bytes/line -> 1920x1080
        other packets: 0x00(FRAME_START) 0x01(FRAME_END)
VC1   : 30.0 fps, DT 0x2b RAW10, 1080 lines, 2400 bytes/line -> 1920x1080
        other packets: 0x00(FRAME_START) 0x01(FRAME_END)
==========================
```

---

## 3. Console commands

| Command | Effect |
| --- | --- |
| `link [ms]` | apply the current setting, get the source restarted, report when clock, lane sync and frames appear. **Start here.** |
| `refine [n]` | compare the n bitrate profiles either side of the current one, keep the quietest |
| `source manual\|auto` | who restarts the source; manual is the default |
| `power` | drive EN_CAM / NRST_CAM once, whatever the mode |
| `scan [ms] [slow]` | sweep lanes/mapping/bitrate; `slow` restarts the source per combination |
| `probe [mbps]` | characterise; without an argument it tries the known-good setting first, then sweeps |
| `phy <mbps> [lanes] [swap]` | apply one D-PHY setting directly, no probing |
| `dt [vc]` | walk every candidate data type on one channel and print the full table; VC 0 by default. Takes a few seconds, needs no power-cycle |
| `geom [vc]` | measure lines and bytes per line of one channel, and print the monotonicity check; VC 0 by default |
| `single <vc>` | preview one channel, centred |
| `dual <vcL> <vcR>` | preview two channels side by side |
| `off` | stop the preview |
| `status` | decoded CSI status registers plus preview counters |
| `report` | reprint the last probe result |

`single` / `dual` take their geometry from the last probe result, so run `probe`
(or let start-up do it) before using them.

---

## 4. How each measurement works

The probe never configures a DCMIPP pixel pipe. That is deliberate: with an
unknown resolution, a pipe writing to memory is a buffer overrun waiting to
happen, and a pipe cannot be configured at all before the pixel format is known.
Everything below reads the CSI block only.

**D-PHY lock** - `CSI_SR1` has clock-lane-active, per-lane-active and per-lane
sync flags, plus SOT/escape/control error flags. A setting that shows
`clk-active` and lane sync without error flags is locked.

**Virtual channel presence** - `CSI_SR0` carries a start-of-frame and an
end-of-frame flag per channel. The probe starts all four channels, then polls
and clears those flags for the length of the window. Counting end-of-frame gives
both presence and frame rate.

**Data type** - the probe offers the channel **one** data type at a time and
watches whether data flows. For each of the 28 plausible long-packet types (the
CSI-2 image range plus the user-defined block) it sets the channel's filter to
accept only that type, then measures two things over a window: whether the
line/byte counter reaches line 1 byte 1, and how often the ID error flag
re-asserts after being cleared. The first reads the datapath, the second the
error path. The type that lets data through is the one the source sends.

The whole 28-row table is printed, not a summary, and the decision rule is stated
underneath. If *every* candidate accepts, the counter is not gated by the data
type filter and the column proves nothing - the table says so rather than
returning a confident wrong answer.

This replaced a method that read the data type straight out of `CSI_ERR1` after
narrowing the filter to a reserved type, so that every packet would name itself
as an ID error. **That does not work.** `CSI_ERR1`'s data type field latches and
is not re-armed by clearing the status flag in `CSI_FCR0`, so the polling loop
read one stale value thousands of times and reported it as an overwhelming
majority. The symptom was unmistakable in hindsight: every link at every bitrate,
clean or marginal, named `0x2f` and nothing else - not even the frame delimiters,
which are short packets and are not filtered by data type at all.

**Geometry** - `CSI_LB0CFGR` fires a status flag when a nominated
(line, byte) position is reached inside a frame. If "reaches N" is monotonic in
N, a binary search over 0..65535 finds the exact line count in 16 steps, and the
same over the byte counter with the line fixed to 1 gives the payload bytes per
line. Width follows from bytes per line and the bits per pixel of the data type.

That monotonicity is an assumption, so it is **checked rather than trusted**.
After each search the probe re-probes four points - 1, half, the result, and one
past it - and prints them. A search converging on something that is not a
geometry (a rate, a wrap-around, a threshold never reached) still returns a
number that looks exactly like a measurement; the four points make it fail
visibly. A result that fails its check is discarded rather than reported, because
a plausible-looking wrong resolution is worse than none.

**Interrupts are masked during every measurement.** At a mismatched bitrate the
receiver raises one error per packet; with the HAL handler attached that is an
interrupt storm which starves the console before the probe can report anything.

---

## 5. Why the two channels alternate

The DCMIPP has exactly **one** demosaicing block and **one** colour-conversion
block, and both belong to PIPE1. The register map shows it directly: `P1DMCR`,
`P1CCxx` and `P1YUVxx` exist, and there is no `P2DMCR`, `P2CCxx` or `P2YUVxx`.
PIPE2 has only crop, decimation, downsize, gamma and the pixel packer
(`Drivers/CMSIS/Device/ST/STM32N6xx/Include/stm32n657xx.h`, DCMIPP register
block). `HAL_DCMIPP_PIPE_SetISPRawBayer2RGBConfig()` accordingly only accepts
`DCMIPP_PIPE1`.

PIPE2 can run in one of two ways
(`HAL_DCMIPP_PIPE_CSI_EnableShare` / `DisableShare`, bit `P1FSCR.PIPEDIFF`):

- **shared** (`PIPEDIFF = 0`, the reset default): PIPE2 receives PIPE1's flow, so
  the same virtual channel, and only scales it differently. This is what
  `lcd_app.c` uses today.
- **independent** (`PIPEDIFF = 1`): PIPE2 selects its own virtual channel and
  data type, but then gets the raw Bayer mosaic with no demosaicing available.

So **two RAW Bayer virtual channels cannot both be turned into colour at the same
time.** The options are:

| Approach | Both in colour | Frame rate | Implemented |
| --- | --- | --- | --- |
| PIPE1 alternating between VC0 and VC1 | yes | half each | **yes** |
| PIPE1 = VC0 colour, PIPE2 = VC1 raw mono | no, right tile is a Bayer-textured greyscale | full | no |
| Two DCMIPP instances | - | - | there is only one |

The alternating scheme is what `csi_preview.c` implements. In the PIPE1
frame-complete interrupt it rewrites `P1FSCR.VC` and the destination address:

```c
MODIFY_REG(DCMIPP->P1FSCR, DCMIPP_P1FSCR_VC, next_vc << DCMIPP_P1FSCR_VC_Pos);
HAL_DCMIPP_PIPE_SetMemoryAddress(&hcamera_dcmipp, DCMIPP_PIPE1,
                                 DCMIPP_MEMORY_ADDRESS_0, next_tile_address);
```

Both writes are single register stores, so they are safe from an ISR.

**This is the one assumption in the module that needs hardware to confirm.** The
DCMIPP keeps a shadow and a current copy of the flow selection (`P1FSCR` versus
`P1CFSCR`), which is what makes a per-frame switch plausible: the shadow is
latched at the next frame start, exactly the boundary the interrupt sits on. If
the hardware does not honour it, `status` will show one tile counter stuck at
zero and `P1CFSCR` not alternating. The fallback would be to stop and restart the
pipe around each switch, which costs a frame but is unambiguous.

The tiles are written **straight into the LTDC framebuffer** with no copy: the
pixel pipe pitch is set to the full screen width (1600 bytes) rather than the
tile width, so the pipe fills a 400x225 sub-rectangle at whatever address it is
given. Left tile at x=0, right tile at x=400, both vertically centred.

Colour rendering uses demosaic -> gamma -> RGB565 packer. The YUV matrix of the
encoder path stays disabled, because the packer needs RGB input to emit RGB565.
Gamma is on because without any exposure control a linear raw frame is close to
black on screen.

---

## 5a. Start order: the source must come up after the receiver

Observed on hardware: the link only works if the CrossLink is re-powered *after*
the DCMIPP pipe is already running. A D-PHY transmitter that starts while the
receiver is still in reset is never picked up.

This has a consequence that is easy to miss. `HAL_DCMIPP_CSI_SetConfig()` takes
the D-PHY through reset on **every** call, so a bitrate sweep resets the receiver
once per combination. Without restarting the source each time, a sweep cannot
find anything even when the settings are right - which is exactly what the first
hardware run showed: `clk -` in all 72 rows, no clock at any bitrate.

### Why the restart is unavoidable from this side

It is **not** a missing back-channel. CSI-2 high-speed traffic is strictly
one-way and source-synchronous: the receiver has no protocol path to ask the
transmitter for anything. The bidirectional parts of D-PHY - low-power escape,
ULPS, lane turnaround - are optional and unused here, and there is no CCI/I2C
link to the CrossLink either. The source is not mishandling a handshake; there is
no handshake.

It is a receiver-initialisation requirement. A D-PHY RX lane leaves its
initialisation state only after it has observed the lane in **Stop state
(LP-11)**. `HAL_DCMIPP_CSI_SetConfig()` clears `CSI_CR.CSIEN`, clears `CSI_PCR`,
programs the frequency range into the Synopsys PHY over its test interface, and
finally releases the digital reset with `CSI_PRCR.PEN`. Nothing in that sequence
waits for the lanes to be idle, because nothing can: if the transmitter is
mid-burst when the reset is released, the receiver sees high-speed levels and
never gets its LP-11.

The **data** lanes would recover on their own - they return to LP-11 after every
line. The **clock** lane is the one that does not, if the transmitter runs a
continuous high-speed clock, which is the common default. Then the clock lane
never presents LP-11 again once streaming has started, and only removing power
produces one. That matches what the board reports: `ACTCLF` appears only after a
source restart.

So the one lever that would remove the manual cycle is on the source: if the
CrossLink's CSI-2 transmitter can be switched to **non-continuous clock mode**
(clock lane drops to LP-11 during blanking), the receiver should re-lock at the
next frame. From the STM32 side there is nothing to fix - the register map
exposes only `CSI_PCR` (lane enables, power-down) and `CSI_PRCR.PEN` (reset),
with no stop-state override.

Note also, while reading that sequence: `HAL_DCMIPP_CSI_SetConfig()` writes both
bytes of the DLL oscillation target to PHY register `0xe3`, where the comment
directly above says `0xe3 & 0xe2`. Register `0xe2` is never written. This only
matters above 1500 Mbit/s, where `osc_freq_target` stops being constant - which
is the range this board runs in.

### Who restarts the source

`source manual` (**the default**) prints a prompt and waits for the clock to
appear. A source on a bench supply has no line back to the board, so this is the
realistic case; the wait is a timeout of 90 s, not a delay, and ends the moment
there is a clock.

`source auto` drives EN_CAM / NRST_CAM instead. That only reaches a module
powered from the camera connector.

Two consequences of the manual default:

- **`scan slow` degrades to one restart up front.** A restart per combination
  means 84 power-cycles by hand, which is not a workflow. The probe says so and
  falls back rather than starting something nobody will finish. Whether the sweep
  is meaningful afterwards is visible in its own output: if only the first rows
  carry frames, the receiver cannot re-acquire a free-running transmitter and the
  sweep is not a usable instrument for this source.
- **`refine` is the tool for choosing a bitrate.** It walks the few profiles
  around the current one - five power-cycles, not eighty-four - and ranks them by
  error count. That is the question worth asking once a link exists at all.

The probe also avoids prompting when it does not need to: `csi_probe_apply_phy()`
is a no-op when the requested setting is already programmed, and a restart is
only requested if the D-PHY was actually reprogrammed or there is no clock. A
full start-up run therefore asks for one power-cycle, not three.

`link` is the command to reach for first. It applies the current setting, gets
the source restarted, and reports when each stage appeared:

```
CSI: clock active after 812 ms, lane sync after 815 ms, first frame after 851 ms
```

Those three timestamps separate the failure modes:

| Symptom | Meaning |
| --- | --- |
| no clock | nothing is transmitting, or the rail never came up. The bitrate setting is irrelevant until this changes |
| clock, no lane sync | this is where the bitrate profile matters |
| lane sync, no end of frame | lane mapping, or the source sends no frame start/end short packets |

`link manual` skips the automatic restart, for power-cycling by hand.

### The camera reset line was never driven

`BSP_CAMERA_HwReset()` initialises and writes `NRST_CAM_PORT` (GPIOC, pin 8) but
only enables the GPIOD and GPIOO clocks - GPIOO is not used by the function at
all, and GPIOC is left unclocked. The `HAL_GPIO_Init()` and both writes on that
port therefore do nothing, and the camera module is never reset from firmware.

In the full application the SD card driver happens to enable GPIOC first, which
masks it. In this probe build nothing else touches GPIOC, so it does not. This is
upstream BSP code (`Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_camera.c`,
identical there); the fix is in the project's patched copy under
`Appli/Core/Src/Patch/`, and adds nothing but the missing clock enable.

Note also that the pin comments in that function contradict the BSP header: the
header names GPIOC/8 `NRST_CAM` and GPIOD/2 `EN_CAM`, while the comments call
GPIOC/8 the "MB1723 2V8 signal". The write order was left exactly as upstream has
it - only the clock was corrected - because which label is right cannot be
settled from the source tree alone.

---

## 6. Things worth knowing before debugging

**Only two data lanes exist.** The HAL defines `DCMIPP_CSI_ONE_DATA_LANE` and
`DCMIPP_CSI_TWO_DATA_LANES` and nothing else; there is no register encoding for
four. A four-lane source has to be reconfigured to two lanes at the transmitter.

**The Bayer pattern cannot be probed.** It is a property of the sensor, not of
the CSI-2 stream. `csi_preview.c` assumes RGGB, matching the IMX335 this board
was designed around. If the colours come out swapped, that constant is the thing
to change.

**YUV sources cannot be previewed as RGB565.** The pixel packer converts RGB to
YUV, not the other way round, and there is no YUV-to-RGB block ahead of it. The
preview refuses such a source with an explicit message rather than showing
garbage.

**Possible HAL bug in the D-PHY DLL programming.** In
`Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_dcmipp.c:734-737` the comment
says "set reg @0xe3 & reg @0xe2 value DLL target oscilation freq", but both
writes target `0xe3`:

```c
DCMIPP_CSI_WritePHYReg(csi_instance, 0x00, 0xe3, ...osc_freq_target >> 8);
DCMIPP_CSI_WritePHYReg(csi_instance, 0x00, 0xe3, ...osc_freq_target & 0xFFU);
```

Register `0xe2` is never written, and `0xe3` ends up holding the low byte. On the
Synopsys D-PHY those two registers are the low and high parts of
`osc_freq_target`. This has not been proven to cause a problem - ST presumably
validated the IMX335 path at 1600 Mbit/s - but it is the first thing to look at
if a bitrate that should lock turns out to be marginal. It is vendor code; this
branch does not patch it.

---

## 6a. Reading the numbers

**A report belongs to the setting it was measured at.** `csi_probe_report_t`
carries the D-PHY setting alongside the measurements, and every command that
changes that setting - `phy`, `link`, `refine`, `scan` - discards the report.
`report` on a discarded one says so instead of reprinting.

This was not always true, and the failure was instructive: the report printed the
*live* setting above measurements cached from an earlier run. Changing the
bitrate and reprinting produced a plausible-looking result for a setting that had
never been measured - `ecc 48 / 6755 corrected / dphy 568` appeared identically
under 2500, 2000, 1450 and 1550 Mbit/s. Keeping the two in one struct makes that
mixture unrepresentable rather than merely discouraged.

**"20!" is not twenty frames.** In the sweep and refine tables a plain number is
frames that *finished*; `N!` means N frames started and none finished. A row
showing `20!` delivered nothing usable and ranks below a row with a single real
frame, which is why `refine` can pick a profile whose row looks worse.

**Error counts are polling samples, not packet counts.** They are only meaningful
relative to each other, across windows of the same length. One window per profile
and one power-cycle each is a single sample: treat a small difference between
neighbouring profiles as noise. The first refine runs bore this out - 1600 Mbit/s
showed 13509 ECC errors in one pass and its neighbour 1550 showed none, which is
not a plausible property of a D-PHY frequency band.

**A number without its check is not a measurement.** Two of the probe's answers
looked like measurements for three hardware runs and were not: the data type came
from a latched register that was never re-armed, and the geometry came from a
binary search whose monotonicity assumption was never tested. Both now print the
evidence they rest on - the full candidate table, the four verification probes -
and both discard a result that fails its own check. Where a summary and a raw
table disagree about how much they claim, print the table.

**Errors the probe caused itself are suppressed.** Stopping a virtual channel
part way through a frame raises sync and SOT errors; those used to surface as
`DCMIPP global error, ErrorCode=0x000c8900` the moment the CSI interrupt was
re-enabled, in the middle of a measurement that had counted zero. `csi_stop_all_vc()`
now clears the flags and the HAL error code after stopping.

---

## 7. Not done

- The characterisation and preview stages have not been seen against a healthy
  link. Every number in the example report is illustrative.
- **The geometry measurement is still unvalidated.** Across clean links at 1550,
  2000 and 2500 Mbit/s it returned 1345 lines and 319 bytes per line, where a
  1920x1080 RAW10 frame should give 1080 and 2400. The reproducibility rules out
  noise: 1345/319 is a real boundary of *something*. It was measured with a data
  type that has since turned out to be bogus, so the first thing to do is repeat
  it now that the type is identified properly - and read the monotonicity line
  that the search now prints, which says whether the number means anything at
  all.
- The data type walk assumes the line/byte counter only counts packets the filter
  accepted. If it turns out not to be gated that way, every candidate will show
  `data yes` and the walk will say so; the rejection-count column is then the
  only usable signal, and it has never been validated against a known source.
- `refine` takes one sample per profile. Ranking two neighbouring profiles needs
  repeats, and each repeat costs a manual power-cycle.
- The scan's own error reporting was noisy on the first run: `HAL_DCMIPP_CSI_SetConfig()`
  latches SOT/control flags while it drives the D-PHY through reset, which showed
  up as `PHY` on a random-looking subset of bitrates. There is now a settle delay
  and a flag clear after every apply, but that has not been re-measured.
- The byte counter is assumed to count within a line when the line counter is
  fixed to 1. If `bytes/line` comes back implausible, that assumption is where to
  look; the raw value is printed alongside the derived width for exactly that
  reason.
- No TraceX instrumentation events were added. A one-shot scan report belongs on
  the console, and `Tools/instrumentation/instrumentation.yaml` is deliberately
  left untouched so an existing `.trx` still decodes against the current schema.
- The greyscale-second-channel variant (PIPE2 with `PIPEDIFF = 1`) is described
  above but not implemented.
