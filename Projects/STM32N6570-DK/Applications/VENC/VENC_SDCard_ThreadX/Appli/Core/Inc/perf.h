/* perf.h - lightweight profiling helpers for VENC pipeline */
#ifndef PERF_H
#define PERF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t count;
  uint64_t total_us;
  uint32_t min_us;
  uint32_t max_us;
} PerfStat_t;

/* Init (safe to call multiple times) */
void perf_init(void);

/* time source */
uint64_t perf_get_time_us(void);
/* return raw 32-bit DWT cycle count */
uint32_t perf_get_cycle_count(void);
/* compute delta in microseconds between two 32-bit cycle counts (handles wrap) */
uint32_t perf_delta_us(uint32_t start_cycles, uint32_t end_cycles);
/* monotone 64-bit cycle counter and delta helpers */
uint64_t perf_get_u64_cycles(void);
uint64_t perf_delta_us64(uint64_t start_cycles, uint64_t end_cycles);

/* add samples for named stages */
void perf_add_encode(uint32_t us);
void perf_add_h264(uint32_t us);
void perf_add_queue_send(uint32_t us);
void perf_add_queue_recv(uint32_t us);
void perf_add_sd_write(uint32_t us);
void perf_add_file_close(uint32_t us);
void perf_add_media_flush(uint32_t us);
void perf_add_sd_ll_write_blocks(uint32_t blocks);
void perf_add_sd_ll_dma(uint32_t us);
void perf_add_blockpool_wait(uint32_t us);
void perf_add_frame_size(uint32_t size, uint32_t capacity, bool is_intra);
void perf_add_sd_buffer_flush(uint32_t buffered_size, uint32_t write_size, uint32_t remain_size, bool forced);
void perf_add_sd_direct_write(uint32_t size);

/* event counters */
void perf_note_capture_frame(void);
void perf_note_encoded_frame(void);
void perf_note_written_frame(void);
void perf_note_frame_skip(void);
void perf_note_zero_size_frame(void);
void perf_note_fuse_error(void);
void perf_note_encode_error(void);
void perf_note_queue_send_fail(void);

/* report summary (prints on stdout) and resets stats */
void perf_report_and_reset(uint32_t frames, uint64_t bytes, uint32_t elapsed_ms);

/* Block-pool and queue occupancy instrumentation */
void perf_set_queue_capacity(uint32_t capacity);
void perf_set_blockpool_capacity(uint32_t capacity);
void perf_inc_blockpool(void);
void perf_dec_blockpool(void);
void perf_inc_queue(void);
void perf_dec_queue(void);

#endif /* PERF_H */
