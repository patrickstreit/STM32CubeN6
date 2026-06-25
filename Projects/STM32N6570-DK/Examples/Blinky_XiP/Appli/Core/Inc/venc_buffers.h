#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * VENC pipeline ping-pong buffers (placed in PSRAM via XSPI1 @ 0x90000000)
 *
 * Sized for the maximum supported resolution: 1920 x 1088 (H.264 MB-aligned).
 * This covers 800x800 target and limit-testing up to 1920x1080.
 *
 * Buffer layout:
 *   frame_ping / frame_pong   DCMIPP -> VENC  (raw RGB565 frames, 4 MB each)
 *   bs_ping    / bs_pong      VENC   -> SD    (H.264 bitstream,  ~1 MB each)
 * ---------------------------------------------------------------------------*/

#define VENC_FRAME_MAX_WIDTH    1920U
#define VENC_FRAME_MAX_HEIGHT   1088U   /* nearest multiple of 16 above 1080 */
#define VENC_FRAME_BPP          2U      /* RGB565: 2 bytes per pixel          */

/* 1920 * 1088 * 2 = 4,177,920 bytes (~4 MB) */
#define VENC_FRAME_BUF_SIZE     (VENC_FRAME_MAX_WIDTH * VENC_FRAME_MAX_HEIGHT * VENC_FRAME_BPP)

/* Bitstream buffer: uint32_t[W*H/8] per VENC_SDCard example = W*H/2 bytes (~1 MB) */
#define VENC_BITSTREAM_BUF_SIZE (VENC_FRAME_MAX_WIDTH * VENC_FRAME_MAX_HEIGHT / 2U)

extern uint8_t frame_ping[VENC_FRAME_BUF_SIZE];
extern uint8_t frame_pong[VENC_FRAME_BUF_SIZE];
extern uint8_t bs_ping   [VENC_BITSTREAM_BUF_SIZE];
extern uint8_t bs_pong   [VENC_BITSTREAM_BUF_SIZE];
