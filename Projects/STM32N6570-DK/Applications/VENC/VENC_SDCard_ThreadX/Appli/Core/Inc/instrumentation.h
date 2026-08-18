/**
  ******************************************************************************
  * @file    instrumentation.h
  * @brief   Thin application event API on top of TraceX user events.
  *
  * There is deliberately no second ring buffer and no second time base: every
  * application event goes into the same TraceX buffer, with the same TraceX
  * timestamp, as the native ThreadX and FileX events.
  ******************************************************************************
  */

#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "generated/instr_ids.h"

#if defined(TX_ENABLE_EVENT_TRACE) && !defined(INSTR_DISABLE)

#include "tx_api.h"

/**
  * @brief  Record one application event with its four TraceX information fields.
  *
  * Must stay allocation-free and printf-free: this runs in the hot path and
  * from ISR context.
  */
#define INSTR_EVENT(event_id, arg0, arg1, arg2, arg3)                 \
    ((void)tx_trace_user_event_insert((ULONG)(event_id),              \
                                      (ULONG)(arg0),                  \
                                      (ULONG)(arg1),                  \
                                      (ULONG)(arg2),                  \
                                      (ULONG)(arg3)))

/**
  * @brief  Apply the trace event filter and emit the schema marker.
  * @note   Call once, immediately after tx_trace_enable().
  */
void INSTR_Init(void);

#else /* tracing disabled */

#define INSTR_EVENT(event_id, arg0, arg1, arg2, arg3) ((void)0)

static inline void INSTR_Init(void) { }

#endif

#ifdef __cplusplus
}
#endif

#endif /* INSTRUMENTATION_H */
