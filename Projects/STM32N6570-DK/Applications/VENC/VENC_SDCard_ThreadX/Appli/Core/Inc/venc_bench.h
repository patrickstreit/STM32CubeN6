/**
  ******************************************************************************
  * @file    venc_bench.h
  * @brief   Encode-time model of the H.264 encoder (PLAN.md M2).
  *
  * What this measures, and why that way
  * ------------------------------------
  * The only number the composite plan needs from the encoder is how long
  * H264EncStrmEncode() takes per macroblock, because that is what decides which
  * frame rate a 448x1792 composite can be sustained at. So that call, and
  * nothing around it, is what gets timed.
  *
  * The frames come from the live camera rather than from a still picture held
  * in memory. Encoding the same picture over and over would make every inter
  * frame nearly free - no residual to code - and would flatter the encoder by a
  * wide margin. Real motion is the only input that gives a usable number.
  *
  * Intra and inter frames are kept apart. They differ by a factor that matters
  * here, and one of the variants under test (enableCabac = 2) changes precisely
  * the split between them: intra coded with CAVLC, inter with CABAC.
  *
  * The output is discarded while a measurement runs (see venc_bench_discard).
  * Otherwise a slow SD card would back the ring buffer up and the encoder would
  * be measured waiting for a block rather than encoding.
  ******************************************************************************
  */

#ifndef VENC_BENCH_H
#define VENC_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where the encoder reads the picture it encodes. */
typedef enum
{
  /** Straight out of the DCMIPP capture buffer, which lives in PSRAM. */
  VENC_INPUT_FROM_CAPTURE = 0,
  /** Copied into AXISRAM first, so the encoder reads its input from internal
      memory. The copy happens outside the timed region and is reported
      separately - what is under test is the read bandwidth the encoder sees,
      not the cost of getting the frame there. */
  VENC_INPUT_FROM_AXISRAM
} venc_input_src_t;

/** @brief Free-running cycle counter, enabled on first use. */
uint32_t venc_bench_now(void);

/** @brief CPU cycles per microsecond. */
uint32_t venc_bench_cycles_per_us(void);

/** @brief Drop every sample collected so far and start a new measurement. */
void venc_bench_reset(void);

/**
  * @brief  Record one encode.
  * @param  intra        true for an IDR/intra picture
  * @param  cycles       duration of H264EncStrmEncode()
  * @param  stream_bytes size of the produced bitstream
  * @param  copy_cycles  time spent staging the input, 0 when it was not staged
  */
void venc_bench_sample(bool intra, uint32_t cycles, uint32_t stream_bytes, uint32_t copy_cycles);

/** @brief Frames recorded since the last reset. */
uint32_t venc_bench_frames(void);

/** @brief Print the measurement as a machine-readable report. */
void venc_bench_report(const char *label);

/** @brief Whether encoded output is being thrown away instead of recorded. */
bool venc_bench_discard(void);

/** @brief Throw encoded output away, so the SD path cannot stall a measurement. */
void venc_bench_set_discard(bool discard);

/* ------------------------------------------------------------------------- */
/* Variants under test                                                       */
/*                                                                           */
/* Each of these changes the configuration and re-initialises the encoder, so */
/* they only work while the pipeline is stopped. All of them return 0 on      */
/* success and print why they refused otherwise.                             */
/* ------------------------------------------------------------------------- */

/** @brief NV12 semi-planar input instead of interleaved YUYV (variant a). */
int venc_bench_set_nv12(bool nv12);

/** @brief Where the encoder reads its input from (variant b). */
int venc_bench_set_input_src(venc_input_src_t src);

/** @brief 0 = CAVLC, 1 = CABAC, 2 = CAVLC intra / CABAC inter (variant c). */
int venc_bench_set_cabac(uint32_t mode);

/** @brief 0 = off, 1 = adaptive, 2 = always (variant d). */
int venc_bench_set_transform8x8(uint32_t mode);

/** @brief Target bitrate in bits per second (variant e). */
int venc_bench_set_bitrate(uint32_t bits_per_second);

/** @brief Where the encoder is currently reading its input from. */
venc_input_src_t venc_bench_input_src(void);

/** @brief Print the configuration the next measurement would run with. */
void venc_bench_print_config(void);

/** @brief The staging buffer, or NULL when this build has none. */
uint8_t *venc_bench_stage_buffer(uint32_t *size);

#ifdef __cplusplus
}
#endif

#endif /* VENC_BENCH_H */
