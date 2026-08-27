/**
  ******************************************************************************
  * @file    csi_mux.h
  * @brief   Alternating capture of two virtual channels into one composite
  *          frame, without the encoder (PLAN.md M1).
  *
  * What this measures
  * ------------------
  * Whether PIPE1 - the only pipe with a demosaicing block, and therefore the
  * only colour path for two RAW Bayer channels - can be switched between two
  * virtual channels fast enough to keep every frame of both. The switch itself
  * is the mechanism csi_preview.c already proves: virtual channel and
  * destination address are written in the frame-complete interrupt and the
  * DCMIPP latches them at the next frame start.
  *
  * What is different here is the destination. Instead of two tiles of a
  * framebuffer, each channel is written as a segment of one composite NV12
  * frame - the layout the encoder will be handed later - so the measurement
  * runs against the buffer geometry that has to work in the end, not a
  * convenient stand-in.
  *
  * The composite is a row of segments: segment k occupies the columns
  * [k * SEG_W, (k+1) * SEG_W) of one NV12 frame of (SEG_W * n) x SEG_H.
  *
  * That row, rather than the column this used to be, is what makes the pixel
  * packer pitch load-bearing. Stacked segments are contiguous - each one is a
  * plain run of memory and the pitch equals the segment width - so switching
  * channels was nothing but an address flip. Side by side, every segment is a
  * window into a wider frame: the pitch has to be the *composite* width while
  * the downsizer still emits SEG_W pixels per line, and the write pointer
  * jumps by pitch - SEG_W bytes at the end of each one. The hardware does that
  * without help (P1PPM0PR/P1PPM1PR are byte counts independent of the captured
  * width, and the HAL programs the same value into both for NV12), but it is
  * the one part of the mechanism the stacked layout never exercised, which is
  * why M1 is taken again in this geometry.
  *
  * Segment size
  * ------------
  * 896 x 448 per channel, composite 1792 x 448. Two channels here stand in for
  * the four of 448 x 448 the final design has: same composite, same pitch, same
  * macroblock count, and the switching mechanism does not care how many
  * segments it cycles through.
  *
  * The pixel packer pitch is a byte count the hardware requires to be a
  * multiple of 16 (the HAL asserts (PITCH & 0xF) == 0, and the asserts are off
  * in this build, so a bad value lands in the register silently). It is now the
  * composite width rather than the segment width, so it is 1792 that has to
  * satisfy it - which it does, as do the segment offsets 0 and 896 that the
  * destination addresses are stepped by.
  ******************************************************************************
  */

#ifndef CSI_MUX_H
#define CSI_MUX_H

#include <stdbool.h>
#include <stdint.h>

#include "csi_preview.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Width of one composite segment, in pixels. A multiple of 16 - see above. */
#define CSI_MUX_SEG_W        896U
/** Height of one composite segment, in lines. */
#define CSI_MUX_SEG_H        448U
/** Segments in the composite; one per virtual channel being captured. */
#define CSI_MUX_SEGMENTS       2U

/** Width of the whole composite, in pixels - and its NV12 pitch, in bytes. */
#define CSI_MUX_COMPOSITE_W  (CSI_MUX_SEG_W * CSI_MUX_SEGMENTS)

/**
  * @brief  Capture @p a and @p b alternately into the composite for @p seconds,
  *         then print a machine-readable report.
  * @param  seconds  measurement window, 0 for the default
  * @retval 0 when a report could be produced
  *
  * Both sources must have the same geometry and data type: one PIPE1
  * configuration is shared and only the channel and the destination addresses
  * change per frame.
  *
  * The report puts the frames the pipe kept next to the frames the source sent
  * over the same window - the latter counted from the CSI frame delimiters
  * (csi_phase.h), which is the only reference that is independent of the pipe.
  */
int csi_mux_run(const csi_preview_source_t *a, const csi_preview_source_t *b, uint32_t seconds);

/** @brief Per-frame hook; call from BSP_CAMERA_FrameEventCallback() for PIPE1. */
void csi_mux_on_pipe1_frame(void);

/** @brief Whether a measurement is running and owns PIPE1. */
bool csi_mux_active(void);

#ifdef __cplusplus
}
#endif

#endif /* CSI_MUX_H */
