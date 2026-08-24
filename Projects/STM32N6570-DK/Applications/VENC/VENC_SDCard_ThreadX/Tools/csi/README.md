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

Status: **compiles and links; not yet run on hardware.** Everything below that
is a measurement rather than a datasheet fact is marked as such.

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
post-build steps as the normal build, so the existing flash task works after
pointing it at that directory.

Footprint for reference: 109 KB flash, 3.8 MB of the 16 MB PSRAM (3 MB of that
is the TraceX ring, 768 KB the framebuffer). The normal build is unchanged.

Console is COM1 at 115200 8N1, same as the main application.

---

## 2. What it does at start-up

```
BSP_CAMERA_Init()      power the camera connector, DCMIPP clocks, HAL_DCMIPP_Init
csi_probe_run()        sweep -> characterise -> leave the receiver configured
csi_probe_print_report()
preview_best_effort()  two channels side by side, or one centred, or nothing
```

The sweep is 18 bitrates x 2 lane counts x 2 lane mappings at 150 ms each, so
roughly 11 s, plus a few seconds per present virtual channel for data type and
geometry. Expect the whole start-up sequence to take 20-30 s before the preview
appears.

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
| `scan [ms]` | sweep lanes/mapping/bitrate, print every setting that shows any activity |
| `probe [mbps]` | characterise; without an argument it sweeps first |
| `phy <mbps> [lanes] [swap]` | apply one D-PHY setting directly, no probing |
| `dt <vc>` | identify the data types on one virtual channel |
| `geom <vc>` | measure lines and bytes per line of one channel |
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

**Data type** - `CSI_ERR1` reports the data type *and* the virtual channel of any
packet the receiver could not match to a configured filter. So the probe narrows
the channel's filter to data type `0x30` (CSI-2 reserved, nothing sends it), and
every arriving packet then names itself. The set collected over the window is the
set of data types the source actually transmits.

**Geometry** - `CSI_LB0CFGR` fires a status flag when a nominated
(line, byte) position is reached inside a frame. Whether a frame reaches line N
is monotonic in N, so a binary search over 0..65535 finds the exact line count in
16 steps, and the same over the byte counter with the line fixed to 1 gives the
payload bytes per line. Width follows from bytes per line and the bits per pixel
of the data type.

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

## 7. Not done

- Not run on hardware. Every number in the example report is illustrative.
- The byte counter is assumed to count within a line when the line counter is
  fixed to 1. If `bytes/line` comes back implausible, that assumption is where to
  look; the raw value is printed alongside the derived width for exactly that
  reason.
- No TraceX instrumentation events were added. A one-shot scan report belongs on
  the console, and `Tools/instrumentation/instrumentation.yaml` is deliberately
  left untouched so an existing `.trx` still decodes against the current schema.
- The greyscale-second-channel variant (PIPE2 with `PIPEDIFF = 1`) is described
  above but not implemented.
