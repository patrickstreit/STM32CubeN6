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
  * The composite is a column of segments: segment k holds its luma at
  * k * SEG_W * SEG_H and its chroma at the corresponding offset in the chroma
  * plane, so the whole thing is one plain NV12 frame of SEG_W x (SEG_H * n).
  *
  * Segment width
  * -------------
  * PLAN.md sizes a segment 440 wide. The pixel packer cannot do that: its pitch
  * register is a byte count that the hardware requires to be a multiple of 16
  * (the HAL asserts (PITCH & 0xF) == 0), and 440 is not. 448 is, it is also a
  * whole number of macroblocks, which removes the padding column the plan had
  * to account for, and it costs 1.8 % more macroblocks.
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
#define CSI_MUX_SEG_W        448U
/** Height of one composite segment, in lines. */
#define CSI_MUX_SEG_H        896U
/** Segments in the composite; one per virtual channel being captured. */
#define CSI_MUX_SEGMENTS       2U

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
