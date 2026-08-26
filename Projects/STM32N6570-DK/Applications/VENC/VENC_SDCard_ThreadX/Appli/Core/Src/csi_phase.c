/**
  ******************************************************************************
  * @file    csi_phase.c
  * @brief   Link-level frame timing of the CSI-2 virtual channels - see csi_phase.h.
  *
  * The two HAL frame callbacks below are the only place in this firmware that
  * hooks the CSI frame delimiters, and they serve two measurements at once: the
  * phase measurement here, and the "what did the source actually send" side of
  * the capture measurement in csi_mux.c. They must stay as short as they are -
  * a timestamp and a counter - because they run at every frame boundary of
  * every channel, so any work done in them lands in the numbers they produce.
  ******************************************************************************
  */

#include "csi_phase.h"

#include <stdio.h>
#include <string.h>

#include "stm32n6570_discovery.h"
#include "stm32n6570_discovery_camera.h"

extern DCMIPP_HandleTypeDef hcamera_dcmipp;

#define PHASE_DEFAULT_FRAMES   120U
#define PHASE_EVENT_CAPACITY  1024U

#define EVT_SOF  0U
#define EVT_EOF  1U

/** One frame delimiter, as the receiver saw it. */
typedef struct
{
  uint32_t cyc;   /*!< DWT cycle count at the interrupt */
  uint8_t  vc;
  uint8_t  kind;  /*!< EVT_SOF or EVT_EOF */
} phase_event_t;

static volatile csi_phase_vc_t  g_counts[CSI_PHASE_VC_COUNT];
static volatile bool            g_observing;
static volatile bool            g_recording;
static volatile uint32_t        g_event_count;
static phase_event_t            g_events[PHASE_EVENT_CAPACITY];

static const uint32_t vc_start_bit[CSI_PHASE_VC_COUNT] =
{ CSI_CR_VC0START, CSI_CR_VC1START, CSI_CR_VC2START, CSI_CR_VC3START };

/* ------------------------------------------------------------------------- */
/* Cycle counter                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t g_cycles_per_us;

static void cycle_counter_init(void)
{
  if (g_cycles_per_us != 0U)
  {
    return;
  }

  if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U)
  {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  }

  /* CYCCNT is deliberately not reset: TraceX timestamps its whole ring from
     the same counter, so zeroing it would move every event recorded so far.
     Only differences are taken below, which a free-running counter gives just
     as well - including across its 5.4 s wrap at 800 MHz, because the
     subtraction is done in 32-bit arithmetic. */
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  g_cycles_per_us = HAL_RCC_GetCpuClockFreq() / 1000000U;
  if (g_cycles_per_us == 0U)
  {
    g_cycles_per_us = 1U;
  }
}

uint32_t csi_phase_cycles_per_us(void)
{
  cycle_counter_init();
  return g_cycles_per_us;
}

uint32_t csi_phase_now(void)
{
  cycle_counter_init();
  return DWT->CYCCNT;
}

/** @brief Cycles as microseconds in tenths, so a sub-microsecond gap still shows. */
static uint32_t cyc_to_us_x10(uint32_t cycles)
{
  return (uint32_t)(((uint64_t)cycles * 10U) / (uint64_t)csi_phase_cycles_per_us());
}

void csi_phase_print_us(const char *key, uint32_t cycles)
{
  uint32_t x10 = cyc_to_us_x10(cycles);
  printf("%s=%lu.%lu\n", key, (unsigned long)(x10 / 10U), (unsigned long)(x10 % 10U));
}

/* ------------------------------------------------------------------------- */
/* Statistics                                                                */
/* ------------------------------------------------------------------------- */

void csi_stat_reset(csi_stat_t *s)
{
  s->n   = 0U;
  s->min = 0xFFFFFFFFU;
  s->max = 0U;
  s->sum = 0U;
}

void csi_stat_add(csi_stat_t *s, uint32_t v)
{
  if (v < s->min) { s->min = v; }
  if (v > s->max) { s->max = v; }
  s->sum += (uint64_t)v;
  s->n++;
}

uint32_t csi_stat_avg(const csi_stat_t *s)
{
  return (s->n == 0U) ? 0U : (uint32_t)(s->sum / (uint64_t)s->n);
}

/** @brief Print n and min/avg/max of a cycle statistic under "<prefix><what>". */
void csi_stat_print(const char *prefix, const char *what, const csi_stat_t *s)
{
  char key[48];

  (void)snprintf(key, sizeof(key), "%s%s_n", prefix, what);
  printf("%s=%lu\n", key, (unsigned long)s->n);

  if (s->n == 0U)
  {
    return;
  }

  (void)snprintf(key, sizeof(key), "%s%s_us_min", prefix, what);
  csi_phase_print_us(key, s->min);
  (void)snprintf(key, sizeof(key), "%s%s_us_avg", prefix, what);
  csi_phase_print_us(key, csi_stat_avg(s));
  (void)snprintf(key, sizeof(key), "%s%s_us_max", prefix, what);
  csi_phase_print_us(key, s->max);
}

/* ------------------------------------------------------------------------- */
/* Interrupt hooks                                                           */
/* ------------------------------------------------------------------------- */

static void phase_record(uint32_t vc, uint8_t kind, uint32_t cyc)
{
  if (!g_recording || (g_event_count >= PHASE_EVENT_CAPACITY))
  {
    return;
  }

  g_events[g_event_count].cyc  = cyc;
  g_events[g_event_count].vc   = (uint8_t)vc;
  g_events[g_event_count].kind = kind;
  g_event_count++;
}

void HAL_DCMIPP_CSI_StartOfFrameEventCallback(DCMIPP_HandleTypeDef *hdcmipp, uint32_t VirtualChannel)
{
  uint32_t cyc = DWT->CYCCNT;

  (void)hdcmipp;

  if (!g_observing || (VirtualChannel >= CSI_PHASE_VC_COUNT))
  {
    return;
  }

  g_counts[VirtualChannel].sof++;
  g_counts[VirtualChannel].last_sof_cyc = cyc;
  phase_record(VirtualChannel, EVT_SOF, cyc);
}

void HAL_DCMIPP_CSI_EndOfFrameEventCallback(DCMIPP_HandleTypeDef *hdcmipp, uint32_t VirtualChannel)
{
  uint32_t cyc = DWT->CYCCNT;

  (void)hdcmipp;

  if (!g_observing || (VirtualChannel >= CSI_PHASE_VC_COUNT))
  {
    return;
  }

  g_counts[VirtualChannel].eof++;
  g_counts[VirtualChannel].last_eof_cyc = cyc;
  phase_record(VirtualChannel, EVT_EOF, cyc);
}

/* ------------------------------------------------------------------------- */
/* Observer                                                                  */
/* ------------------------------------------------------------------------- */

void csi_phase_observer_start(uint32_t vc_mask)
{
  cycle_counter_init();

  g_observing = false;
  memset((void *)g_counts, 0, sizeof(g_counts));

  for (uint32_t vc = 0U; vc < CSI_PHASE_VC_COUNT; vc++)
  {
    if ((vc_mask & (1UL << vc)) == 0U)
    {
      continue;
    }

    if ((CSI->SR0 & (CSI_SR0_VC0STATEF << vc)) == 0U)
    {
      uint32_t tickstart = HAL_GetTick();

      SET_BIT(CSI->CR, vc_start_bit[vc]);
      /* A channel reports the active state only once the receiver has seen a
         frame start on it, so this has to cover a frame period with room to
         spare rather than a register write. */
      while ((CSI->SR0 & (CSI_SR0_VC0STATEF << vc)) == 0U)
      {
        if ((HAL_GetTick() - tickstart) > 150U)
        {
          printf("PHASE: VC%lu never reported the active state; "
                 "it may not be transmitting\n", (unsigned long)vc);
          break;
        }
      }
    }

    __HAL_DCMIPP_CSI_CLEAR_FLAG(CSI, (DCMIPP_CSI_FLAG_SOF0 << vc) | (DCMIPP_CSI_FLAG_EOF0 << vc));
    __HAL_DCMIPP_CSI_ENABLE_IT(CSI, (DCMIPP_CSI_IT_SOF0 << vc) | (DCMIPP_CSI_IT_EOF0 << vc));
  }

  g_observing = true;
}

void csi_phase_observer_stop(void)
{
  g_observing = false;
  g_recording = false;

  __HAL_DCMIPP_CSI_DISABLE_IT(CSI, DCMIPP_CSI_IT_SOF0 | DCMIPP_CSI_IT_SOF1 |
                              DCMIPP_CSI_IT_SOF2 | DCMIPP_CSI_IT_SOF3 |
                              DCMIPP_CSI_IT_EOF0 | DCMIPP_CSI_IT_EOF1 |
                              DCMIPP_CSI_IT_EOF2 | DCMIPP_CSI_IT_EOF3);
}

uint32_t csi_phase_last_eof_cyc(uint32_t vc)
{
  return (vc < CSI_PHASE_VC_COUNT) ? g_counts[vc].last_eof_cyc : 0U;
}

void csi_phase_observer_snapshot(csi_phase_vc_t out[CSI_PHASE_VC_COUNT])
{
  for (uint32_t vc = 0U; vc < CSI_PHASE_VC_COUNT; vc++)
  {
    /* Every field is a single word that the interrupt only ever advances; a
       torn read would need the counter updated twice inside one copy, which at
       frame rate cannot happen. */
    out[vc].sof          = g_counts[vc].sof;
    out[vc].eof          = g_counts[vc].eof;
    out[vc].last_sof_cyc = g_counts[vc].last_sof_cyc;
    out[vc].last_eof_cyc = g_counts[vc].last_eof_cyc;
  }
}

/* ------------------------------------------------------------------------- */
/* M0                                                                        */
/* ------------------------------------------------------------------------- */

/** @brief Count the channels in @p mask and report the two lowest of them. */
static uint32_t mask_channels(uint32_t mask, uint32_t *first, uint32_t *second)
{
  uint32_t n = 0U;

  *first  = CSI_PHASE_VC_COUNT;
  *second = CSI_PHASE_VC_COUNT;

  for (uint32_t vc = 0U; vc < CSI_PHASE_VC_COUNT; vc++)
  {
    if ((mask & (1UL << vc)) != 0U)
    {
      if (n == 0U)      { *first  = vc; }
      else if (n == 1U) { *second = vc; }
      n++;
    }
  }
  return n;
}

int csi_phase_run(uint32_t vc_mask, uint32_t frames)
{
  static csi_stat_t xfer[CSI_PHASE_VC_COUNT];     /* SOF -> EOF: one frame on the wire */
  static csi_stat_t period[CSI_PHASE_VC_COUNT];   /* SOF -> SOF: the source frame rate */
  static csi_stat_t gap_dir[CSI_PHASE_VC_COUNT][CSI_PHASE_VC_COUNT];
  csi_stat_t   gap_inter;                         /* EOF -> SOF across channels        */
  csi_stat_t   gap_intra;                         /* EOF -> SOF within one channel     */
  uint32_t open_sof[CSI_PHASE_VC_COUNT];
  uint32_t prev_sof[CSI_PHASE_VC_COUNT];
  bool     is_open[CSI_PHASE_VC_COUNT];
  uint32_t vc_frames[CSI_PHASE_VC_COUNT];
  uint32_t overlaps      = 0U;
  uint32_t same_runs     = 0U;
  uint32_t last_frame_vc = CSI_PHASE_VC_COUNT;
  uint32_t first_vc;
  uint32_t second_vc;
  uint32_t channels;
  uint32_t deadline;
  uint32_t started;
  uint32_t window_ms;
  uint32_t events;
  char     pattern[64];
  uint32_t pattern_len = 0U;

  if (vc_mask == 0U)
  {
    vc_mask = 0x3U;
  }
  if (frames == 0U)
  {
    frames = PHASE_DEFAULT_FRAMES;
  }

  channels = mask_channels(vc_mask, &first_vc, &second_vc);
  if (channels == 0U)
  {
    printf("PHASE: no virtual channel selected\n");
    return -1;
  }

  cycle_counter_init();

  g_event_count = 0U;
  csi_phase_observer_start(vc_mask);
  g_recording = true;

  started  = HAL_GetTick();
  deadline = started + (frames * 200U) + 3000U;

  /* The wait ends on whichever comes first: enough frames on every selected
     channel, a full event buffer, or the timeout. A mark per second keeps a
     scripted console from reading the silence as a finished command. */
  while (HAL_GetTick() < deadline)
  {
    bool enough = true;

    for (uint32_t vc = 0U; vc < CSI_PHASE_VC_COUNT; vc++)
    {
      if (((vc_mask & (1UL << vc)) != 0U) && (g_counts[vc].sof < frames))
      {
        enough = false;
      }
    }
    if (enough || (g_event_count >= PHASE_EVENT_CAPACITY))
    {
      break;
    }

    HAL_Delay(1000U);
    printf(".");
  }
  printf("\n");

  g_recording = false;
  window_ms   = HAL_GetTick() - started;
  events      = g_event_count;
  csi_phase_observer_stop();

  if (events < 4U)
  {
    printf("PHASE: only %lu frame delimiter(s) arrived - is the link up? "
           "Run 'link' first\n", (unsigned long)events);
    return -1;
  }

  for (uint32_t vc = 0U; vc < CSI_PHASE_VC_COUNT; vc++)
  {
    csi_stat_reset(&xfer[vc]);
    csi_stat_reset(&period[vc]);
    is_open[vc]   = false;
    open_sof[vc]  = 0U;
    prev_sof[vc]  = 0U;
    vc_frames[vc] = 0U;
    for (uint32_t other = 0U; other < CSI_PHASE_VC_COUNT; other++)
    {
      csi_stat_reset(&gap_dir[vc][other]);
    }
  }
  csi_stat_reset(&gap_inter);
  csi_stat_reset(&gap_intra);

  /* One pass over the delimiters. Everything the report needs falls out of it:
     a frame that starts while another channel's frame has not ended is the
     interleaving proof, and the distance from one channel's frame end to the
     next channel's frame start is the room a per-frame channel switch has. */
  for (uint32_t i = 0U; i < events; i++)
  {
    uint32_t vc  = g_events[i].vc;
    uint32_t cyc = g_events[i].cyc;

    if (vc >= CSI_PHASE_VC_COUNT)
    {
      continue;
    }

    if (pattern_len < (sizeof(pattern) - 4U))
    {
      pattern[pattern_len++] = (g_events[i].kind == EVT_SOF) ? 'S' : 'E';
      pattern[pattern_len++] = (char)('0' + (int)vc);
      pattern[pattern_len++] = ' ';
    }

    if (g_events[i].kind == EVT_SOF)
    {
      for (uint32_t other = 0U; other < CSI_PHASE_VC_COUNT; other++)
      {
        if ((other != vc) && is_open[other])
        {
          overlaps++;
        }
      }

      if (prev_sof[vc] != 0U)
      {
        csi_stat_add(&period[vc], cyc - prev_sof[vc]);
      }
      prev_sof[vc] = cyc;
      open_sof[vc] = cyc;
      is_open[vc]  = true;
    }
    else
    {
      if (is_open[vc])
      {
        csi_stat_add(&xfer[vc], cyc - open_sof[vc]);
        is_open[vc] = false;
        vc_frames[vc]++;

        if (last_frame_vc == vc)
        {
          same_runs++;
        }
        last_frame_vc = vc;
      }
    }
  }
  pattern[pattern_len] = '\0';

  /* Gaps are taken between adjacent delimiters rather than per channel, so what
     is reported is the room actually left on the wire and not an average over
     two channels that may never have been adjacent. */
  for (uint32_t i = 1U; i < events; i++)
  {
    uint32_t from;
    uint32_t to;
    uint32_t d;

    if ((g_events[i - 1U].kind != EVT_EOF) || (g_events[i].kind != EVT_SOF))
    {
      continue;
    }

    from = g_events[i - 1U].vc;
    to   = g_events[i].vc;
    d    = g_events[i].cyc - g_events[i - 1U].cyc;

    if ((from >= CSI_PHASE_VC_COUNT) || (to >= CSI_PHASE_VC_COUNT))
    {
      continue;
    }

    if (from == to)
    {
      csi_stat_add(&gap_intra, d);
    }
    else
    {
      csi_stat_add(&gap_inter, d);
      csi_stat_add(&gap_dir[from][to], d);
    }
  }

  printf("=== PHASE RESULT ===\n");
  printf("cpu_hz=%lu\n", (unsigned long)HAL_RCC_GetCpuClockFreq());
  printf("vc_mask=0x%lx\n", (unsigned long)vc_mask);
  printf("events=%lu\n", (unsigned long)events);
  printf("events_capped=%lu\n", (unsigned long)((events >= PHASE_EVENT_CAPACITY) ? 1U : 0U));
  printf("window_ms=%lu\n", (unsigned long)window_ms);
  printf("channels=%lu\n", (unsigned long)channels);
  printf("order=%s\n", (overlaps > 0U) ? "interleaved" : "serial");
  printf("overlaps=%lu\n", (unsigned long)overlaps);
  printf("same_vc_runs=%lu\n", (unsigned long)same_runs);
  printf("pattern=%s\n", pattern);

  for (uint32_t vc = 0U; vc < CSI_PHASE_VC_COUNT; vc++)
  {
    char prefix[12];
    char key[32];

    if ((vc_mask & (1UL << vc)) == 0U)
    {
      continue;
    }

    (void)snprintf(prefix, sizeof(prefix), "vc%lu.", (unsigned long)vc);
    printf("%sframes=%lu\n", prefix, (unsigned long)vc_frames[vc]);
    printf("%ssof=%lu\n", prefix, (unsigned long)g_counts[vc].sof);
    printf("%seof=%lu\n", prefix, (unsigned long)g_counts[vc].eof);
    if ((window_ms > 0U) && (vc_frames[vc] > 0U))
    {
      printf("%sfps_x100=%lu\n", prefix,
             (unsigned long)(((uint64_t)vc_frames[vc] * 100000U) / (uint64_t)window_ms));
    }
    csi_stat_print(prefix, "xfer", &xfer[vc]);
    csi_stat_print(prefix, "period", &period[vc]);
    if (period[vc].n > 0U)
    {
      (void)snprintf(key, sizeof(key), "%speriod_jitter", prefix);
      csi_phase_print_us(key, period[vc].max - period[vc].min);
    }
  }

  csi_stat_print("", "gap_inter", &gap_inter);
  csi_stat_print("", "gap_intra", &gap_intra);

  if (channels >= 2U)
  {
    char key[32];

    if (gap_dir[first_vc][second_vc].n > 0U)
    {
      (void)snprintf(key, sizeof(key), "gap_%luto%lu_us_avg",
                     (unsigned long)first_vc, (unsigned long)second_vc);
      csi_phase_print_us(key, csi_stat_avg(&gap_dir[first_vc][second_vc]));
    }
    if (gap_dir[second_vc][first_vc].n > 0U)
    {
      (void)snprintf(key, sizeof(key), "gap_%luto%lu_us_avg",
                     (unsigned long)second_vc, (unsigned long)first_vc);
      csi_phase_print_us(key, csi_stat_avg(&gap_dir[second_vc][first_vc]));
    }
  }
  printf("=== END ===\n");

  if (channels < 2U)
  {
    printf("PHASE: one channel only - nothing can be said about their order\n");
  }
  else if (overlaps > 0U)
  {
    printf("PHASE: frames overlap on the wire (%lu start(s) arrived while another\n"
           "       channel's frame was still open). One time-shared pipe can then\n"
           "       capture at most every second frame per channel.\n",
           (unsigned long)overlaps);
  }
  else
  {
    printf("PHASE: the channels are serialised - every frame ends before the next\n"
           "       one starts. A per-frame channel switch has the inter-channel gap\n"
           "       above to complete in, so capturing both channels in full is\n"
           "       possible as long as the switch fits into that gap.\n");
  }

  return 0;
}
