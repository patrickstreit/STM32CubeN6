# CSI-2 probe and dual-virtual-channel preview

A minimal firmware variant of this project that brings up **only** the
DCMIPP/CSI-2 receiver. No encoder, no SD card, no FileX. It exists to answer two
questions about a CSI-2 source that the STM32 does not control:

1. **Which CSI settings work?** Lane count, lane mapping, D-PHY bitrate, which
   virtual channels are present, what data type each carries, and the frame
   geometry.
2. **Can VC0 and VC1 be shown side by side on the DK display?** The receiver
   side is finished and proven; what is missing is a second channel. The two
   tiles alternate rather than run at once, for a reason imposed by the silicon -
   see [Why the two channels alternate](#why-the-two-channels-alternate).

Status, from several hardware sessions:

- The link is **clean at 2500 Mbit/s per lane, 2 lanes, physical mapping**: no
  ECC, CRC or D-PHY errors over 500 ms. 1250 Mbit/s locks as well but is
  **marginal** - around 200 uncorrectable header ECC errors per 500 ms - and so
  are 1200, 1450 and 1550. 2500 is therefore the known-good setting.
- **VC0 carries 1920x1080 RAW10 (0x2b) at 48-50 fps.** Confirmed twice over: by
  the line and byte counters, and by dumping the payload - `grab` returned
  16-bit little-endian values in the 0..1023 range, 3840 bytes per line, which is
  1920 pixels unpacked to 16-bit words.
- **The channel carries two more kinds of packet besides the picture**: two lines
  of embedded data (`0x12`, 320 bytes each, the 0x55/0x5a/0xa5 encoding sensors
  use for register dumps) and 264 lines tagged `0x2f` RAW20 of 168 bytes that are
  almost entirely zero. Neither is a second picture. What the source means by the
  RAW20 lines is a question for the transmitter.
- **VC1, VC2 and VC3 carry nothing.** `vcs` sees no frame start and no long
  packet on them, and a full data type walk on VC1 rejects nothing either -
  there are no packets to reject. The side-by-side goal is short of a second
  source at the transmitter, not short of a way to find it.
- The failure that produced no clock at any of 72 settings was **start order**,
  not bitrate; see [Start order](#5a-start-order-the-source-must-come-up-after-the-receiver).
- The source takes **7 to 9 s** to start transmitting after its power is
  restored. Every wait for it is a timeout that ends on the clock, never a fixed
  delay - which is what makes a 5-minute window cost nothing when the operator is
  quick and still work when they are not.
- **`dual` works, with one channel.** `dual 0 0` fills both tiles from VC0 at
  153 against 152 frames, so the per-frame address swap is proven. Only the
  virtual-channel half of the switch is still untested, for want of a VC1.
- `single 0` shows the picture on the display, downsized to 400x225. The Bayer
  pattern is **RGGB**, established by stepping through all four with `bayer` and
  looking at the screen - nothing in a CSI-2 stream states it. Colours land in
  the right place; saturation is poor, which is expected and explained below.

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

Without VS Code, `Tools/debug/stm32n6-gdb.ps1` does build, flash and debug from a
console. It is the same sequence as the launch configuration - ST-LINK_gdbserver,
GDB on the FSBL, Appli symbols added on top, run to `BOOT_Application` - with the
tool paths discovered from the STM32Cube bundle directory:

```powershell
./Tools/debug/stm32n6-gdb.ps1 -Build -FlashAppli -Preset CsiProbe
./Tools/debug/stm32n6-gdb.ps1 -NoFsblLoad -Interactive      # attach to what is flashed
./Tools/debug/stm32n6-gdb.ps1 -DryRun                       # print the commands, touch nothing
```

Anything passed to `-Ex` runs as a GDB command once the target is at the entry
point; without `-Interactive` the script then detaches - which resumes the target -
and stops the server, so it can be used non-interactively.

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
print_help()           and then wait for a command
```

**Nothing is measured until it is asked for.** Every measurement costs a manual
source power-cycle, so a probe at boot spends one on a setting the operator may
not want - and it did, on 1250 Mbit/s, for 90 s of waiting each reset. The D-PHY
is left unprogrammed until the first command, which also means no report can
show numbers that were never measured.

`probe <mbps>` characterises one setting. `probe` without an argument tries the
known-good setting first - 2500 Mbit/s per lane over two lanes - and only if
that finds no link does it sweep all 21 bitrates x 2 lane counts x 2 mappings,
which takes a couple of minutes and prints an estimate before it starts.

Example of the report it prints:

```
=== CSI-2 probe result ===
D-PHY : 2500 Mbit/s per lane, 2 lane(s), physical mapping
Link  : clean over 500 ms, no ecc/crc/dphy errors
VC0   : 48.0 fps, DT 0x2b RAW10, 1080 lines, 2400 bytes/line -> 1920x1080
        2 other data type(s) also accepted: 0x12(EMBEDDED) 0x2f(RAW20)
==========================
```

That is a real report from this source, not an illustration.

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
| `vcs [ms]` | ask each of VC0..VC3 in turn whether anything arrives on it, with the data type filter wide open. Finds channels that send no frame delimiters, which a probe run cannot. Needs a link but no power-cycle |
| `dt [vc]` | walk every candidate data type on one channel and print the full table; VC 0 by default. Takes a few seconds, needs no power-cycle |
| `geom [vc]` | measure lines and bytes per line of one channel, and print the monotonicity check; VC 0 by default |
| `bayer <0-3>` | which corner of the Bayer cell is red - 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR. Takes effect on the next frame without stopping the pipe, so the four can be compared on a live picture |
| `errors [ms]` | clear every CSI flag, then count what comes back next to the frame count over the same window |
| `grab <vc> <dt> [n]` | dump the raw payload of one data type through the DCMIPP dump pipe and hexdump the first n bytes. Answers what a data type carries, not just how much of it there is |
| `single <vc>` | preview one channel, centred |
| `dual <vcL> <vcR>` | preview two channels side by side |
| `off` | stop the preview |
| `status` | decoded CSI status registers plus preview counters |
| `report` | reprint the last probe result |

`single` / `dual` take their geometry from the last probe result, so `probe` has
to have run before them.

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

That test only sees channels that send **frame delimiters**. A channel carrying
long packets without a frame start - metadata, embedded data, a generator that
was configured for a continuous stream - is invisible to it, and so is a channel
whose delimiters the receiver rejects. `vcs` closes that gap: it takes each
channel in turn, opens its data type filter completely, arms the line counter on
that channel and watches for the length of a window. Long packets with no frame
start show up as data with `frame starts: no`, and the command says so.

What neither test can see is a source using the **extended virtual channels**
CSI-2 v2.0 added. This receiver has start, stop, status and filtering registers
for VC0..VC3 only - there is no `VCX` field anywhere in the CSI register block -
so a transmitter sending on VC4..VC15 produces silence here, not an error. `vcs`
says that out loud when it finds only one channel, because "we found one" and
"there is only one" are different statements.

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

**When more than one data type accepts, the line each one describes decides what
that means.** This channel accepts three: `0x2b` (RAW10), `0x12` (embedded data)
and `0x2f` (RAW20). Several acceptances mean either several kinds of packet, or
one kind the filter cannot tell apart from another - and two data types that
select the same packets must report the same bytes per line. These do not:

| data type | lines per frame | bytes per line |
| --- | --- | --- |
| `0x2b` RAW10 | 1080 | 2400 |
| `0x2f` RAW20 | 264 | 168 |
| `0x12` embedded | 2 | 320 |

The obvious test - enable two data types at once and see whether the counts add
up - was tried first and **does not work**. The line/byte counter fires once per
frame at line 1 byte 1, and packets of different data types share one frame here,
so the count is the frame rate whether one type is enabled or two. It read "5
hits" in every combination. That test is gone; the shape is what discriminates,
and it needs no assumption about framing.

The candidate list covers `0x10` to `0x13` as well as the image types. Leaving
those out is what hid the embedded data: the walk reported two accepted types on
a channel that carries three, because `0x12` was never offered to it.

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

Two details the hardware settled. The counters match on an **index**, so the last
line of a 1080-line frame is 1079 and the last byte of a 2400-byte line is 2399;
the searches return indices and one is added afterwards, after the check below,
so the check compares like for like. And the channel is filtered to the image
data type for the duration - `HAL_DCMIPP_CSI_SetVCConfig()` sets `ALLDT`, and
then the line counter counts every long packet including embedded data and
blanking. With `ALLDT` a 1920x1080 source measured 1345 lines of 320 bytes;
filtered, 1080 lines of 2400.

That monotonicity is an assumption, so it is **checked rather than trusted**.
After each search the probe re-probes four points - 1, half, the result, and one
past it - and prints them. A search converging on something that is not a
geometry (a rate, a wrap-around, a threshold never reached) still returns a
number that looks exactly like a measurement; the four points make it fail
visibly. A result that fails its check is discarded rather than reported, because
a plausible-looking wrong resolution is worse than none.

**Payload** - the counters say how much arrives and in what shape; they cannot say
what is in it. `grab` can. PIPE0 of the DCMIPP is the dump pipe - no ISP, no pixel
packer, the received bytes in order - so pointing it at one data type and reading
the first bytes back is the direct answer. It also brings a **second, independent
data type filter**: the CSI virtual channel filter and the pipe's own `DTIDA`
comparison are separate hardware, so a pair the first cannot separate may still be
separated by the second.

Two things about that pipe are worth knowing, both learned the hard way:

- **The dump limit register is an event, not a wall.** `P0DCLMTR` set to 64 kB did
  not stop the transfer: the pipe reported 4147200 bytes dumped - a full frame -
  and wrote all of it, through the buffer and into the trace ring behind it. The
  HAL name says so once you know what to look for: `HAL_DCMIPP_PIPE_EnableLimitEvent()`
  enables an interrupt. What does bound the capture is the pipe's **crop**, set to
  four lines; the buffer is sized for a whole 1080p frame so that a source getting
  past the crop still cannot reach anything else.
- **The byte counter is not cleared when a frame does not complete.** A data type
  that delivers nothing reports the previous capture's count. The honest test is
  the buffer's fill pattern, which is what `grab` checks before printing a dump -
  and it is also what separates "no packets" from "packets full of zeros", a
  distinction this source actually makes.

### Why the picture looks washed out

There is no exposure control and no white balance anywhere in this path, and
there cannot be: both are sensor functions, driven over I2C, and no sensor is
reachable from the STM32 here - the stream is generated externally. What the
preview does is the minimum that makes a linear raw frame visible at all:
demosaic, then the DCMIPP gamma curve.

Missing, in the order that would help most, all of them DCMIPP blocks that need
no sensor:

- **black level** (`HAL_DCMIPP_PIPE_SetISPBlackLevelCalibrationConfig`) - a raw
  sensor's black sits above zero, and not subtracting it is what greys out the
  darks and flattens saturation most.
- **per-channel gain** (`HAL_DCMIPP_PIPE_SetISPExposureConfig`) - the white
  balance knob, one multiplier per colour, and the brightness knob with it.
- **colour conversion matrix** (`HAL_DCMIPP_PIPE_SetISPColorConversionConfig`) -
  a 3x3 that turns sensor primaries into display primaries. This is the one that
  actually adds saturation rather than just brightness.

None of this is needed to answer what the probe exists to answer, so none of it
is implemented. It is listed because "the colours are right but the picture is
flat" is the expected outcome here, not a symptom of something being wrong.

### What the data type error means

The preview sets `CSI_SR0.IDERRF` permanently - around 13000 polling samples per
second - and `CSI_ERR1` names `0x2f`. It looks like a fault and is not one. Three
measurements pin it down, and none of them needed guessing at the register map:

| virtual channel filter | pipe | data type errors per second | frames |
| --- | --- | --- | --- |
| all types, format BPP8 | none | 0 | 49 |
| all types, format BPP10 | none | 0 | 50 |
| all types, format BPP10 | PIPE1 on `0x2b` | ~13100 | full rate |
| one type (`0x2b`) | none | ~12000 | full rate |

`VC0CFGR1` reads `0x00000301` in the second and third rows - identical. So it is
neither the filter nor the word format: **the flag appears as soon as something
consumes a subset of what arrives.** The channel carries `0x2b`, `0x12` and
`0x2f`; PIPE1 is configured for `0x2b`; the other two reach the receiver and are
claimed by nobody, and that is what "unfiltered data type" reports.

Narrowing the channel filter to `0x2b` does not help - the fourth row is the
rejection column of the data type walk, which runs exactly that configuration.
The only quiet combination is accepting everything and consuming nothing.

It costs no frames. The preview runs at the full source rate throughout: 761
tiles in about 15 s. The frame count printed by `errors` does drop while a pipe
runs, but that is this tool's own counter losing the flag to the HAL's frame
interrupt handler, not a dropped frame - `errors` says so when it detects it.

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

- **The virtual-channel half of `dual` is untested.** `dual 0 0` proves the
  per-frame address swap - both tiles fill at the same rate - but with one
  channel the `P1FSCR.VC` write has nothing to switch between. The moment a VC1
  exists this is two commands: `vcs` to confirm it arrives, `dual 0 1` to show
  it.
- The **two DCMIPP errors at preview start** (`0x100` sync, then `0x900` sync
  plus data ID) are a real transient: the counters stay at two however long the
  preview runs. What is *not* a transient is the CSI data type error flag - see
  [What the data type error means](#what-the-data-type-error-means). It is
  explained and harmless, but it is permanently set while anything captures.
- What the `0x2f` RAW20 packets are **for** is unanswered. They are real - 264
  per frame, 168 bytes each, and the buffer comes back written rather than
  untouched - but almost entirely zero, and neither their count nor their length
  matches the picture. This side can only say what arrives; why the source sends
  it is a question for the transmitter.
- The rejection-count column of the walk does not discriminate **between
  candidates**: it reads roughly 1050-1300 for every one of them including the
  correct one, because it measures how fast the loop polls. It does discriminate
  between **channels**, which the hardware showed: the same column on VC1 was
  zero for all 28 candidates. Nothing was rejected there because nothing arrived.
- `refine` takes one sample per profile. Ranking two neighbouring profiles needs
  repeats, and each repeat costs a manual power-cycle.
- The scan's own error reporting was noisy on the first run: `HAL_DCMIPP_CSI_SetConfig()`
  latches SOT/control flags while it drives the D-PHY through reset, which showed
  up as `PHY` on a random-looking subset of bitrates. There is now a settle delay
  and a flag clear after every apply, but that has not been re-measured.
- **No exposure, white balance or colour matrix.** Colours are in the right
  place and the picture is flat; see [Why the picture looks washed
  out](#why-the-picture-looks-washed-out) for the three DCMIPP blocks that would
  fix it without needing a sensor.
- The byte counter is assumed to count within a line when the line counter is
  fixed to 1. That assumption held everywhere it was checked - a middle line of
  the frame is what the geometry search now measures - but it is still an
  assumption, and the raw value is printed next to the derived width so an
  implausible result is visible.
- No TraceX instrumentation events were added. A one-shot scan report belongs on
  the console, and `Tools/instrumentation/instrumentation.yaml` is deliberately
  left untouched so an existing `.trx` still decodes against the current schema.
- The greyscale-second-channel variant (PIPE2 with `PIPEDIFF = 1`) is described
  above but not implemented.

---

## 8. Picking this up again

Three commands from a cold start, in `Projects/STM32N6570-DK/Applications/VENC/VENC_SDCard_ThreadX`:

```powershell
./Tools/debug/stm32n6-gdb.ps1 -Build -FlashAppli -Preset CsiProbe
./Tools/debug/csi-console.ps1 -WaitFor "first frame after" -Sequence "link 280000"   # power-cycle the source
./Tools/debug/csi-console.ps1 -Sequence "probe 2500; single 0"
```

The second one is the only one that needs a human: the source has to be
power-cycled while it waits. Everything after that runs on a live link with no
further cycles - `vcs`, `dt`, `geom`, `grab`, `bayer`, `errors` all work without
touching the D-PHY.

**The one blocker is not on this side.** The receiver is characterised, the
preview runs at full rate, and the two-tile machinery is proven. VC0 and VC1 side
by side needs the CrossLink to emit a second virtual channel. When it does, `vcs`
confirms it in ten seconds and `dual 0 1` shows it.

**What the encoder path inherited from this work**, both in
`Appli/Core/Src/dcmipp_app.c`:

- the D-PHY bitrate, 1250 -> 2500, because 1250 measured marginal here;
- the Bayer pattern, RGGB, no longer inherited from the IMX335 but checked.

It will also raise the CSI data type error flag permanently, for the reason in
section 4. That is expected, not a fault.

### Things that cost time to learn, so they are written down

- **The FSBL is a RAM image** and the board boots into the ROM bootloader. GDB's
  `load` is not a convenience, it is what starts the system. Flashing the
  application alone leaves a board that says nothing.
- **The gdb server resets the target on connect** unless it is given `-g`.
  Attaching to a running board to read one register will reset it and cost a
  power-cycle. `stm32n6-gdb.ps1` passes `-g` whenever it is not loading.
- **The debugger cannot read PSRAM** at `0x90000000`. Trying it fails the detach
  and kills the session; print from the firmware instead.
- **The console drops characters** when a command arrives while it is printing -
  one byte per `HAL_UART_Receive()`, no FIFO. `csi-console.ps1` paces characters
  and retries a command when the board answers `unknown command`.
- **Status flags are sticky and `CSI_ERR1` never re-arms.** A `status` read
  reports the union of everything that ever happened. Use `errors` for a rate.
