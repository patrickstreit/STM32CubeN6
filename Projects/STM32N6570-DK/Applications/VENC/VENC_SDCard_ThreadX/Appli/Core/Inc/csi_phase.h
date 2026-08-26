/**
  ******************************************************************************
  * @file    csi_phase.h
  * @brief   Link-level frame timing of the CSI-2 virtual channels (PLAN.md M0).
  *
  * What this answers
  * -----------------
  * The aggregator in front of this board sends two cameras as two virtual
  * channels and its documentation says only that data is sent "alternately".
  * Whether that alternation is per frame or per packet decides everything
  * downstream: with whole frames arriving one after the other, one time-shared
  * pipe can capture both channels completely, while packet-level interleaving
  * caps each channel at half the source rate no matter how fast the switch is.
  *
  * The question is answered without touching a pixel pipe. The CSI receiver
  * raises a start- and an end-of-frame interrupt per virtual channel from the
  * short packets alone, so timestamping those four events is enough to see
  * whether VC1's frame starts before VC0's has ended, and how much room the
  * gap between them leaves for a channel switch.
  *
  * The same interrupts double as the reference for a capture measurement: they
  * count what the *source* sent, which is the only honest denominator for the
  * frames a pipe managed to keep (csi_mux.h).
  ******************************************************************************
  */

#ifndef CSI_PHASE_H
#define CSI_PHASE_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32n6xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_PHASE_VC_COUNT   4U

/** What the link-level observer has seen on one virtual channel. */
typedef struct
{
  uint32_t sof;           /*!< frame-start short packets since the observer started */
  uint32_t eof;           /*!< frame-end short packets since then                   */
  uint32_t last_sof_cyc;  /*!< DWT cycle count of the most recent frame start       */
  uint32_t last_eof_cyc;  /*!< DWT cycle count of the most recent frame end         */
} csi_phase_vc_t;

/**
  * A min/average/maximum accumulator over cycle counts.
  *
  * It lives here rather than in each measurement because both M0 and M1 report
  * the same kind of number and should print it the same way: one key per line,
  * microseconds to one decimal, so a report parses without knowing which
  * measurement produced it.
  */
typedef struct
{
  uint32_t n;
  uint32_t min;
  uint32_t max;
  uint64_t sum;
} csi_stat_t;

void     csi_stat_reset(csi_stat_t *s);
void     csi_stat_add(csi_stat_t *s, uint32_t cycles);
uint32_t csi_stat_avg(const csi_stat_t *s);

/** @brief Print "<prefix><what>_n" and, when non-empty, its min/avg/max in us. */
void csi_stat_print(const char *prefix, const char *what, const csi_stat_t *s);

/** @brief Print one cycle count as "key=<microseconds>.<tenth>". */
void csi_phase_print_us(const char *key, uint32_t cycles);

/**
  * @brief  Cycle count of the most recent frame end on @p vc, 0 if none yet.
  * @note   Readable from an interrupt: it is one word, only ever written by the
  *         frame-end interrupt.
  */
uint32_t csi_phase_last_eof_cyc(uint32_t vc);

/**
  * @brief  Start every virtual channel in @p vc_mask and count its frames.
  * @note   Idempotent per channel: a channel the pipe already started is left
  *         alone. Zeroes the counters.
  */
void csi_phase_observer_start(uint32_t vc_mask);

/** @brief Disarm the frame interrupts. Channels are left running. */
void csi_phase_observer_stop(void);

/** @brief Copy the counters out; safe against the interrupt updating them. */
void csi_phase_observer_snapshot(csi_phase_vc_t out[CSI_PHASE_VC_COUNT]);

/** @brief CPU cycles per microsecond, for turning cycle deltas into times. */
uint32_t csi_phase_cycles_per_us(void);

/** @brief Free-running cycle counter, enabled on first use. */
uint32_t csi_phase_now(void);

/**
  * @brief  M0: timestamp the frame delimiters of @p vc_mask over @p frames
  *         frames and print a machine-readable report.
  * @param  frames  frames to observe on the busiest channel, 0 for the default
  * @retval 0 when a report could be produced
  *
  * Prints whether the channels are serialised or interleaved, the gap between
  * one channel's frame end and the next channel's frame start, the transfer
  * duration of a frame and the jitter of the frame period.
  */
int csi_phase_run(uint32_t vc_mask, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* CSI_PHASE_H */
