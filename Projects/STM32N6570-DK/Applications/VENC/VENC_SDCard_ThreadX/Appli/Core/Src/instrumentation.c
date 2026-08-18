/**
  ******************************************************************************
  * @file    instrumentation.c
  * @brief   TraceX event filter configuration and schema marker.
  ******************************************************************************
  */

#include "instrumentation.h"

#if defined(TX_ENABLE_EVENT_TRACE) && !defined(INSTR_DISABLE)

#include "main.h"
#include "fx_api.h"

/*
 * Categories kept in the trace (everything not listed here is filtered out):
 *
 *   ThreadX TX_TRACE_INTERNAL_EVENTS  thread resume/suspend, ISR enter/exit,
 *                                     context switch - the scheduler timeline
 *                                     is reconstructed from these on the host.
 *   ThreadX TX_TRACE_QUEUE_EVENTS     enc_frame_queue send/receive, i.e. the
 *                                     VENC -> SD hand-off and its depth.
 *   ThreadX TX_TRACE_THREAD_EVENTS    thread create (names/priorities) plus
 *                                     sleep/terminate; low rate, high value.
 *   ThreadX TX_TRACE_USER_EVENTS      the application events.
 *   FileX   FX_TRACE_FILE_EVENTS      file open/write/close.
 *   FileX   FX_TRACE_MEDIA_EVENTS     media flush/close, useful for SD stalls.
 *
 * Filtered out and why:
 *
 *   BLOCK_POOL / BYTE_POOL   only used once at startup for thread stacks.
 *   EVENT_FLAGS              venc_app_flags fires twice per frame and carries
 *                            no information that FRAME_CAPTURED does not.
 *   INTERRUPT_CONTROL        very high rate, no analytical value here.
 *   MUTEX / SEMAPHORE        not used by this application's data path.
 *   TIME / TIMER             tick bookkeeping only.
 *   FX_TRACE_INTERNAL        per-sector driver read/write; would dominate the
 *                            ring buffer. Re-enable deliberately when
 *                            investigating the SD driver itself.
 *   FX_TRACE_DIRECTORY       only touched on file create/rotate.
 */
#ifndef INSTR_TRACE_FILTER_MASK
#define INSTR_TRACE_FILTER_MASK  ( TX_TRACE_BLOCK_POOL_EVENTS        \
                                 | TX_TRACE_BYTE_POOL_EVENTS         \
                                 | TX_TRACE_EVENT_FLAGS_EVENTS       \
                                 | TX_TRACE_INTERRUPT_CONTROL_EVENT  \
                                 | TX_TRACE_MUTEX_EVENTS             \
                                 | TX_TRACE_SEMAPHORE_EVENTS         \
                                 | TX_TRACE_TIME_EVENTS              \
                                 | TX_TRACE_TIMER_EVENTS             \
                                 | FX_TRACE_INTERNAL_EVENTS          \
                                 | FX_TRACE_DIRECTORY_EVENTS )
#endif

void INSTR_Init(void)
{
  (void)tx_trace_event_filter((ULONG)INSTR_TRACE_FILTER_MASK);

  /* TX_TRACE_TIME_SOURCE is DWT->CYCCNT, which counts at the CPU clock. */
  INSTR_EVENT(INSTR_ID_SCHEMA_INFO,
              INSTR_SCHEMA_VERSION,
              INSTR_SCHEMA_HASH,
              SystemCoreClock,
              TX_TIMER_TICKS_PER_SECOND);
}

#endif /* TX_ENABLE_EVENT_TRACE && !INSTR_DISABLE */
