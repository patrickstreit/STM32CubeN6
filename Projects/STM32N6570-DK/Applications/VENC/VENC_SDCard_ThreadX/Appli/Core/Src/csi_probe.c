/**
  ******************************************************************************
  * @file    csi_probe.c
  * @brief   MIPI CSI-2 receiver probe - see csi_probe.h for the rationale.
  *
  * Everything here is polled and CSI-only. The CSI interrupt is masked while a
  * measurement runs: with a wrong bitrate the receiver raises one error per
  * packet, and with the HAL handler attached that is an interrupt storm which
  * starves the console before the probe can report anything.
  ******************************************************************************
  */

#include "csi_probe.h"

#include <stdio.h>
#include <string.h>

#include "stm32n6570_discovery.h"

extern DCMIPP_HandleTypeDef hcamera_dcmipp;

/* Bitrate of every HAL DCMIPP_CSI_PHY_BT_xxx profile, in profile order. */
static const uint16_t csi_phy_profile_mbps[] =
{
    80,   90,  100,  110,  120,  130,  140,  150,  160,  170,  180,  190,
   205,  220,  235,  250,  275,  300,  325,  350,  400,  450,  500,  550,
   600,  650,  700,  750,  800,  850,  900,  950, 1000, 1050, 1100, 1150,
  1200, 1250, 1300, 1350, 1400, 1450, 1500, 1550, 1600, 1650, 1700, 1750,
  1800, 1850, 1900, 1950, 2000, 2050, 2100, 2150, 2200, 2250, 2300, 2350,
  2400, 2450, 2500
};
#define CSI_PHY_PROFILE_COUNT (sizeof(csi_phy_profile_mbps) / sizeof(csi_phy_profile_mbps[0]))

/* Coarse sweep grid. A D-PHY profile tolerates a fair amount of mismatch, so
   walking all 63 profiles would only cost time; this ladder finds the region
   and the caller can refine around the winner with a single observation. */
static const uint16_t csi_scan_mbps[] =
{
  200, 400, 600, 800, 1000, 1200, 1400, 1500, 1600, 1700, 1800, 1900,
  2000, 2100, 2200, 2300, 2400, 2500
};
#define CSI_SCAN_MBPS_COUNT (sizeof(csi_scan_mbps) / sizeof(csi_scan_mbps[0]))

static const uint32_t vc_start_bit[CSI_PROBE_VC_COUNT] =
{ CSI_CR_VC0START, CSI_CR_VC1START, CSI_CR_VC2START, CSI_CR_VC3START };

static const uint32_t vc_stop_bit[CSI_PROBE_VC_COUNT] =
{ CSI_CR_VC0STOP, CSI_CR_VC1STOP, CSI_CR_VC2STOP, CSI_CR_VC3STOP };

/* CSI_SR0 error bits that mean "the link is not decoding cleanly". */
#define CSI_SR0_LINK_ERRORS  (CSI_SR0_CRCERRF | CSI_SR0_ECCERRF | CSI_SR0_SYNCERRF | \
                              CSI_SR0_SPKTERRF | CSI_SR0_WDERRF)

/* CSI_SR1 error bits that mean "the D-PHY is not locked at this bitrate". */
#define CSI_SR1_PHY_ERRORS   (CSI_SR1_ESOTDL0F | CSI_SR1_ESOTSYNCDL0F | CSI_SR1_EESCDL0F | \
                              CSI_SR1_ESYNCESCDL0F | CSI_SR1_ECTRLDL0F | \
                              CSI_SR1_ESOTDL1F | CSI_SR1_ESOTSYNCDL1F | CSI_SR1_EESCDL1F | \
                              CSI_SR1_ESYNCESCDL1F | CSI_SR1_ECTRLDL1F)

/* Data type the probe asks the receiver to accept while identifying what the
   source really sends. 0x30 is in the CSI-2 reserved range, so no sane
   transmitter uses it and every incoming packet is reported as an ID error. */
#define CSI_PROBE_IMPOSSIBLE_DT  0x30U

/* ------------------------------------------------------------------------- */
/* Data type helpers                                                         */
/* ------------------------------------------------------------------------- */

const char *csi_probe_dt_name(uint32_t dt)
{
  switch (dt)
  {
    case 0x00U: return "FRAME_START";
    case 0x01U: return "FRAME_END";
    case 0x02U: return "LINE_START";
    case 0x03U: return "LINE_END";
    case 0x08U: return "GENERIC_SHORT";
    case 0x10U: return "NULL";
    case 0x11U: return "BLANKING";
    case 0x12U: return "EMBEDDED";
    case DCMIPP_DT_YUV420_8:  return "YUV420_8";
    case DCMIPP_DT_YUV420_10: return "YUV420_10";
    case DCMIPP_DT_YUV422_8:  return "YUV422_8";
    case DCMIPP_DT_YUV422_10: return "YUV422_10";
    case DCMIPP_DT_RGB444:    return "RGB444";
    case DCMIPP_DT_RGB555:    return "RGB555";
    case DCMIPP_DT_RGB565:    return "RGB565";
    case DCMIPP_DT_RGB666:    return "RGB666";
    case DCMIPP_DT_RGB888:    return "RGB888";
    case DCMIPP_DT_RAW8:      return "RAW8";
    case DCMIPP_DT_RAW10:     return "RAW10";
    case DCMIPP_DT_RAW12:     return "RAW12";
    case DCMIPP_DT_RAW14:     return "RAW14";
    default:                  return "?";
  }
}

uint32_t csi_probe_dt_bpp(uint32_t dt)
{
  switch (dt)
  {
    case DCMIPP_DT_RAW8:      return 8U;
    case DCMIPP_DT_RAW10:     return 10U;
    case DCMIPP_DT_RAW12:     return 12U;
    case DCMIPP_DT_RAW14:     return 14U;
    case DCMIPP_DT_YUV420_8:  return 12U;
    case DCMIPP_DT_YUV422_8:  return 16U;
    case DCMIPP_DT_YUV422_10: return 20U;
    case DCMIPP_DT_RGB565:    return 16U;
    case DCMIPP_DT_RGB888:    return 24U;
    default:                  return 0U;
  }
}

uint32_t csi_probe_dt_bpp_code(uint32_t dt)
{
  switch (csi_probe_dt_bpp(dt))
  {
    case 10U: return DCMIPP_CSI_DT_BPP10;
    case 12U: return DCMIPP_CSI_DT_BPP12;
    case 14U: return DCMIPP_CSI_DT_BPP14;
    case 16U: return DCMIPP_CSI_DT_BPP16;
    default:  return DCMIPP_CSI_DT_BPP8;
  }
}

/** @brief Is this data type a long packet carrying image lines? */
static bool dt_is_image(uint32_t dt)
{
  return (dt >= 0x18U) && (dt <= 0x37U);
}

/** @brief Was data type @p dt observed on this virtual channel? */
static bool dt_seen(const csi_probe_vc_info_t *info, uint32_t dt)
{
  return (dt < 32U) ? ((info->dt_mask_lo & (1UL << dt)) != 0U)
                    : ((info->dt_mask_hi & (1UL << (dt - 32U))) != 0U);
}

uint32_t csi_probe_bitrate_profile(uint32_t mbps, uint32_t *applied_mbps)
{
  uint32_t best_idx  = 0U;
  uint32_t best_diff = 0xFFFFFFFFU;

  for (uint32_t i = 0U; i < CSI_PHY_PROFILE_COUNT; i++)
  {
    uint32_t profile = csi_phy_profile_mbps[i];
    uint32_t diff    = (profile > mbps) ? (profile - mbps) : (mbps - profile);
    if (diff < best_diff)
    {
      best_diff = diff;
      best_idx  = i;
    }
  }

  if (applied_mbps != NULL)
  {
    *applied_mbps = csi_phy_profile_mbps[best_idx];
  }
  return best_idx;
}

/* ------------------------------------------------------------------------- */
/* Virtual channel start/stop                                                */
/* ------------------------------------------------------------------------- */

/** @brief Stop every virtual channel and wait until none is active any more. */
static void csi_stop_all_vc(void)
{
  uint32_t tickstart;
  uint32_t active_mask = CSI_SR0_VC0STATEF | (CSI_SR0_VC0STATEF << 1) |
                         (CSI_SR0_VC0STATEF << 2) | (CSI_SR0_VC0STATEF << 3);

  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    SET_BIT(CSI->CR, vc_stop_bit[vc]);
  }

  tickstart = HAL_GetTick();
  while ((CSI->SR0 & active_mask) != 0U)
  {
    if ((HAL_GetTick() - tickstart) > 50U)
    {
      break;
    }
  }

  /* The HAL pipe-start path arms SOF/EOF interrupts per channel; make sure none
     is left armed behind the probe's back. */
  __HAL_DCMIPP_CSI_DISABLE_IT(CSI, DCMIPP_CSI_IT_SOF0 | DCMIPP_CSI_IT_SOF1 |
                              DCMIPP_CSI_IT_SOF2 | DCMIPP_CSI_IT_SOF3 |
                              DCMIPP_CSI_IT_EOF0 | DCMIPP_CSI_IT_EOF1 |
                              DCMIPP_CSI_IT_EOF2 | DCMIPP_CSI_IT_EOF3);
}

/**
  * @brief  Start one virtual channel and wait for it to report the active state.
  * @note   The channel only becomes active once the receiver has seen a frame
  *         start on it, so the timeout has to cover at least one frame period.
  * @retval false on timeout; the caller may still continue, the measurement
  *         will then simply find nothing.
  */
static bool csi_start_vc(uint32_t vc, uint32_t timeout_ms)
{
  uint32_t tickstart;

  SET_BIT(CSI->CR, vc_start_bit[vc]);

  tickstart = HAL_GetTick();
  while ((CSI->SR0 & (CSI_SR0_VC0STATEF << vc)) == 0U)
  {
    if ((HAL_GetTick() - tickstart) > timeout_ms)
    {
      return false;
    }
  }
  return true;
}

HAL_StatusTypeDef csi_probe_apply_phy(const csi_probe_phy_t *phy)
{
  DCMIPP_CSI_ConfTypeDef csiconf = {0};

  if (phy == NULL)
  {
    return HAL_ERROR;
  }

  csi_stop_all_vc();

  csiconf.NumberOfLanes   = (phy->lanes == 1U) ? DCMIPP_CSI_ONE_DATA_LANE
                                               : DCMIPP_CSI_TWO_DATA_LANES;
  csiconf.DataLaneMapping = (phy->swapped != 0U) ? DCMIPP_CSI_INVERTED_DATA_LANES
                                                 : DCMIPP_CSI_PHYSICAL_DATA_LANES;
  csiconf.PHYBitrate      = csi_probe_bitrate_profile(phy->mbps, NULL);

  return HAL_DCMIPP_CSI_SetConfig(&hcamera_dcmipp, &csiconf);
}

/* ------------------------------------------------------------------------- */
/* Observation                                                               */
/* ------------------------------------------------------------------------- */

bool csi_probe_observe(const csi_probe_phy_t *phy, uint32_t window_ms, csi_probe_result_t *out)
{
  uint32_t tickstart;
  bool     any_frame = false;

  if ((phy == NULL) || (out == NULL))
  {
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->phy       = *phy;
  out->window_ms = window_ms;

  /* Poll everything: a mismatched bitrate produces one error per packet and the
     HAL handler would spend the whole window inside the ISR. */
  HAL_NVIC_DisableIRQ(CSI_IRQn);

  if (csi_probe_apply_phy(phy) != HAL_OK)
  {
    HAL_NVIC_EnableIRQ(CSI_IRQn);
    return false;
  }

  /* Accept every data type on every channel: the format is unknown at this
     point and filtering would hide the very packets we are looking for. */
  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc, DCMIPP_CSI_DT_BPP8);
  }

  /* Arm all four channels at once and watch the state flags inside the polling
     loop: waiting for each one in turn would cost a full timeout per absent
     channel, which dominates a sweep over dozens of settings. */
  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    SET_BIT(CSI->CR, vc_start_bit[vc]);
  }

  /* Discard whatever accumulated while the channels were coming up. */
  CSI->FCR0 = 0xFFFFFFFFU;
  CSI->FCR1 = 0xFFFFFFFFU;

  tickstart = HAL_GetTick();
  while ((HAL_GetTick() - tickstart) < window_ms)
  {
    uint32_t sr0    = CSI->SR0;
    uint32_t sr1    = CSI->SR1;
    uint32_t clear0 = 0U;

    out->sr0 |= sr0;
    out->sr1 |= sr1;

    for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
    {
      if ((sr0 & (CSI_SR0_VC0STATEF << vc)) != 0U)
      {
        out->vc_state_mask |= (1UL << vc);
      }
      if ((sr0 & (CSI_SR0_SOF0F << vc)) != 0U)
      {
        out->sof[vc]++;
        clear0 |= (CSI_SR0_SOF0F << vc);
      }
      if ((sr0 & (CSI_SR0_EOF0F << vc)) != 0U)
      {
        out->eof[vc]++;
        clear0 |= (CSI_SR0_EOF0F << vc);
        any_frame = true;
      }
    }

    /* Error flags are cleared as well, so the sticky OR reflects errors that
       kept coming rather than one latched at start-up. */
    CSI->FCR0 = clear0 | (sr0 & (CSI_SR0_LINK_ERRORS | CSI_SR0_CECCERRF | CSI_SR0_IDERRF));
    CSI->FCR1 = sr1 & CSI_SR1_PHY_ERRORS;
  }

  csi_stop_all_vc();
  HAL_NVIC_EnableIRQ(CSI_IRQn);

  return any_frame;
}

/* ------------------------------------------------------------------------- */
/* Sweep                                                                     */
/* ------------------------------------------------------------------------- */

static void csi_print_scan_row(const csi_probe_result_t *r)
{
  printf("  %4u  %u %s | clk %c l0 %c%c l1 %c%c | vc %c%c%c%c | eof %3lu %3lu %3lu %3lu |",
         (unsigned)r->phy.mbps,
         (unsigned)r->phy.lanes,
         (r->phy.swapped != 0U) ? "swap" : "norm",
         ((r->sr1 & CSI_SR1_ACTCLF)   != 0U) ? 'A' : '-',
         ((r->sr1 & CSI_SR1_ACTDL0F)  != 0U) ? 'A' : '-',
         ((r->sr1 & CSI_SR1_SYNCDL0F) != 0U) ? 'S' : '-',
         ((r->sr1 & CSI_SR1_ACTDL1F)  != 0U) ? 'A' : '-',
         ((r->sr1 & CSI_SR1_SYNCDL1F) != 0U) ? 'S' : '-',
         ((r->vc_state_mask & 1U) != 0U) ? '0' : '-',
         ((r->vc_state_mask & 2U) != 0U) ? '1' : '-',
         ((r->vc_state_mask & 4U) != 0U) ? '2' : '-',
         ((r->vc_state_mask & 8U) != 0U) ? '3' : '-',
         (unsigned long)r->eof[0], (unsigned long)r->eof[1],
         (unsigned long)r->eof[2], (unsigned long)r->eof[3]);

  if ((r->sr1 & CSI_SR1_PHY_ERRORS) != 0U) { printf(" PHY"); }
  if ((r->sr0 & CSI_SR0_ECCERRF)    != 0U) { printf(" ECC"); }
  if ((r->sr0 & CSI_SR0_CRCERRF)    != 0U) { printf(" CRC"); }
  if ((r->sr0 & CSI_SR0_SYNCERRF)   != 0U) { printf(" SYNC"); }
  if ((r->sr0 & CSI_SR0_WDERRF)     != 0U) { printf(" WDG"); }
  if ((r->sr0 & CSI_SR0_SPKTERRF)   != 0U) { printf(" SPKT"); }
  if ((r->sr0 & CSI_SR0_CECCERRF)   != 0U) { printf(" ecc-corrected"); }
  printf("\n");
}

static uint32_t csi_result_frames(const csi_probe_result_t *r)
{
  return r->eof[0] + r->eof[1] + r->eof[2] + r->eof[3];
}

bool csi_probe_scan(uint32_t window_ms, csi_probe_phy_t *best)
{
  csi_probe_result_t result;
  csi_probe_phy_t    winner      = {0};
  uint32_t           best_frames = 0U;
  bool               best_clean  = false;
  bool               found       = false;

  printf("CSI: scanning %u bitrates x 2 lane counts x 2 mappings, %lu ms each\n",
         (unsigned)CSI_SCAN_MBPS_COUNT, (unsigned long)window_ms);
  printf("  mbps  ln map  | dphy activity     | vc active | frames per vc     | errors\n");

  for (uint32_t lanes = 2U; lanes >= 1U; lanes--)
  {
    for (uint32_t swapped = 0U; swapped <= 1U; swapped++)
    {
      for (uint32_t i = 0U; i < CSI_SCAN_MBPS_COUNT; i++)
      {
        csi_probe_phy_t phy;
        uint32_t        frames;
        bool            clean;

        phy.mbps    = csi_scan_mbps[i];
        phy.lanes   = (uint8_t)lanes;
        phy.swapped = (uint8_t)swapped;

        (void)csi_probe_observe(&phy, window_ms, &result);

        frames = csi_result_frames(&result);
        clean  = ((result.sr1 & CSI_SR1_PHY_ERRORS) == 0U) &&
                 ((result.sr0 & CSI_SR0_LINK_ERRORS) == 0U);

        /* Only rows that carry information; a full sweep is otherwise four
           screens of "nothing here". */
        if ((frames != 0U) || (result.sr0 != 0U) || (result.sr1 != 0U))
        {
          csi_print_scan_row(&result);
        }

        if (frames == 0U)
        {
          continue;
        }
        found = true;

        /* An error-free lock always beats a noisy one; within the same class,
           more frames means a more stable link. */
        if ((clean && !best_clean) || ((clean == best_clean) && (frames > best_frames)))
        {
          best_clean  = clean;
          best_frames = frames;
          winner      = phy;
        }
      }
    }
  }

  if (!found)
  {
    printf("CSI: no virtual channel produced a complete frame at any setting.\n");
    printf("     Check that the source is streaming and that EN_CAM/NRST_CAM are asserted;\n");
    printf("     if the clock lane never shows 'A' the D-PHY sees no clock at all.\n");
    return false;
  }

  printf("CSI: best setting %u Mbit/s, %u lane(s), %s mapping (%lu frames, %s)\n",
         (unsigned)winner.mbps, (unsigned)winner.lanes,
         (winner.swapped != 0U) ? "inverted" : "physical",
         (unsigned long)best_frames, best_clean ? "no errors" : "with errors");

  if (best != NULL)
  {
    *best = winner;
  }
  return true;
}

/* ------------------------------------------------------------------------- */
/* Data type identification                                                  */
/* ------------------------------------------------------------------------- */

void csi_probe_datatypes(uint32_t vc, uint32_t window_ms, csi_probe_vc_info_t *info)
{
  DCMIPP_CSI_VCFilteringConfTypeDef filter = {0};
  uint32_t tickstart;

  if ((info == NULL) || (vc >= CSI_PROBE_VC_COUNT))
  {
    return;
  }

  info->dt_mask_lo = 0U;
  info->dt_mask_hi = 0U;
  info->image_dt   = 0U;

  HAL_NVIC_DisableIRQ(CSI_IRQn);
  csi_stop_all_vc();

  /* Narrow the filter to a data type nothing can be sending. Every real packet
     then raises an ID error and CSI_ERR1 names the data type that arrived. */
  filter.DataTypeNB        = 1U;
  filter.DataTypeClass[0]  = CSI_PROBE_IMPOSSIBLE_DT;
  filter.DataTypeFormat[0] = DCMIPP_CSI_DT_BPP8;
  (void)HAL_DCMIPP_CSI_SetVCFilteringConfig(&hcamera_dcmipp, vc, &filter);

  if (!csi_start_vc(vc, 150U))
  {
    printf("CSI: VC%lu did not report the active state; identifying anyway\n",
           (unsigned long)vc);
  }

  CSI->FCR0 = 0xFFFFFFFFU;

  tickstart = HAL_GetTick();
  while ((HAL_GetTick() - tickstart) < window_ms)
  {
    if ((CSI->SR0 & CSI_SR0_IDERRF) != 0U)
    {
      uint32_t err1   = CSI->ERR1;
      uint32_t err_dt = (err1 & CSI_ERR1_IDDTERR) >> CSI_ERR1_IDDTERR_Pos;
      uint32_t err_vc = (err1 & CSI_ERR1_IDVCERR) >> CSI_ERR1_IDVCERR_Pos;

      /* ERR1 only holds the most recent offender, so the loop samples it as
         fast as it can and accumulates the set of data types it ever saw. */
      CSI->FCR0 = CSI_SR0_IDERRF;

      if (err_vc == vc)
      {
        if (err_dt < 32U)
        {
          info->dt_mask_lo |= (1UL << err_dt);
        }
        else
        {
          info->dt_mask_hi |= (1UL << (err_dt - 32U));
        }
      }
    }
  }

  csi_stop_all_vc();

  /* Put the channel back to "accept everything" so nothing downstream trips
     over a filter the probe left behind. */
  (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc, DCMIPP_CSI_DT_BPP8);
  HAL_NVIC_EnableIRQ(CSI_IRQn);

  /* The image data type is the long-packet type in the observed set. */
  for (uint32_t dt = 0U; dt < CSI_PROBE_DT_COUNT; dt++)
  {
    if (dt_seen(info, dt) && dt_is_image(dt))
    {
      info->image_dt = dt;
      break;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

/**
  * @brief  Does virtual channel @p vc reach (@p line, @p byte) inside one frame?
  * @note   Uses CSI line/byte counter 0, which only raises a status flag. No
  *         DCMIPP pipe is involved, so an unknown resolution cannot overrun any
  *         buffer - that is the whole reason the geometry is measured this way.
  */
static bool csi_reaches(uint32_t vc, uint32_t line, uint32_t byte, uint32_t timeout_ms)
{
  DCMIPP_CSI_LineByteCounterConfTypeDef cnt = {0};
  uint32_t tickstart;
  bool     reached = false;

  cnt.VirtualChannel = vc;
  cnt.LineCounter    = line;
  cnt.ByteCounter    = byte;
  (void)HAL_DCMIPP_CSI_SetLineByteCounterConfig(&hcamera_dcmipp, DCMIPP_CSI_COUNTER0, &cnt);

  /* Enable the counter but not its interrupt: the flag is polled. */
  SET_BIT(CSI->PRGITR, CSI_PRGITR_LB0EN);
  CSI->FCR0 = CSI_SR0_LB0F;

  tickstart = HAL_GetTick();
  while ((HAL_GetTick() - tickstart) < timeout_ms)
  {
    if ((CSI->SR0 & CSI_SR0_LB0F) != 0U)
    {
      reached = true;
      break;
    }
  }

  CLEAR_BIT(CSI->PRGITR, CSI_PRGITR_LB0EN);
  CSI->FCR0 = CSI_SR0_LB0F;

  return reached;
}

static bool probe_line(uint32_t vc, uint32_t value, uint32_t timeout_ms)
{
  return csi_reaches(vc, value, 0U, timeout_ms);
}

static bool probe_byte(uint32_t vc, uint32_t value, uint32_t timeout_ms)
{
  /* Line 1, so the byte counter refers to bytes inside the first line. */
  return csi_reaches(vc, 1U, value, timeout_ms);
}

/**
  * @brief  Largest value in [1, @p hi] for which @p reached still fires.
  * @note   Monotonic by construction: if a frame reaches byte/line N it also
  *         reached every smaller one, so a binary search is exact.
  */
static uint32_t csi_search_max(uint32_t vc, uint32_t hi, uint32_t timeout_ms,
                               bool (*reached)(uint32_t vc, uint32_t value, uint32_t timeout_ms))
{
  uint32_t lo   = 1U;
  uint32_t best = 0U;

  while (lo <= hi)
  {
    uint32_t mid = lo + ((hi - lo) / 2U);

    if (reached(vc, mid, timeout_ms))
    {
      best = mid;
      lo   = mid + 1U;
    }
    else
    {
      if (mid == 0U)
      {
        break;
      }
      hi = mid - 1U;
    }
  }
  return best;
}

void csi_probe_geometry(uint32_t vc, csi_probe_vc_info_t *info)
{
  /* Roughly three frame periods at 30 fps, so a miss is a real miss. */
  const uint32_t timeout_ms = 120U;
  uint32_t bpp;

  if ((info == NULL) || (vc >= CSI_PROBE_VC_COUNT))
  {
    return;
  }

  info->lines          = 0U;
  info->bytes_per_line = 0U;
  info->width          = 0U;

  HAL_NVIC_DisableIRQ(CSI_IRQn);
  csi_stop_all_vc();
  (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc, csi_probe_dt_bpp_code(info->image_dt));

  if (!csi_start_vc(vc, 150U))
  {
    printf("CSI: VC%lu did not report the active state; measuring anyway\n", (unsigned long)vc);
  }

  info->lines          = csi_search_max(vc, 0xFFFFU, timeout_ms, probe_line);
  info->bytes_per_line = csi_search_max(vc, 0xFFFFU, timeout_ms, probe_byte);

  csi_stop_all_vc();
  HAL_NVIC_EnableIRQ(CSI_IRQn);

  bpp = csi_probe_dt_bpp(info->image_dt);
  if ((bpp != 0U) && (info->bytes_per_line != 0U))
  {
    info->width = (info->bytes_per_line * 8U) / bpp;
  }
}

/* ------------------------------------------------------------------------- */
/* Full run and report                                                       */
/* ------------------------------------------------------------------------- */

void csi_probe_run(csi_probe_phy_t *phy, csi_probe_vc_info_t info[CSI_PROBE_VC_COUNT])
{
  csi_probe_result_t result;
  const uint32_t     window_ms = 500U;

  if ((phy == NULL) || (info == NULL))
  {
    return;
  }

  memset(info, 0, sizeof(csi_probe_vc_info_t) * CSI_PROBE_VC_COUNT);

  if (phy->mbps == 0U)
  {
    if (!csi_probe_scan(150U, phy))
    {
      return;
    }
  }

  /* Re-measure the chosen setting over a longer window so the per-channel frame
     rate is worth printing. */
  (void)csi_probe_observe(phy, window_ms, &result);

  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    info[vc].frames  = result.eof[vc];
    info[vc].present = (result.eof[vc] != 0U);
    info[vc].fps_x10 = (result.eof[vc] * 10000U) / window_ms;

    if (!info[vc].present)
    {
      continue;
    }

    csi_probe_datatypes(vc, 150U, &info[vc]);
    csi_probe_geometry(vc, &info[vc]);
  }

  /* Leave the receiver on the setting that was characterised, with each present
     channel configured for the format it actually carries. */
  (void)csi_probe_apply_phy(phy);
  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    if (info[vc].present)
    {
      (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc,
                                       csi_probe_dt_bpp_code(info[vc].image_dt));
    }
  }
}

void csi_probe_print_report(const csi_probe_phy_t *phy,
                            const csi_probe_vc_info_t info[CSI_PROBE_VC_COUNT])
{
  if ((phy == NULL) || (info == NULL))
  {
    return;
  }

  printf("\n=== CSI-2 probe result ===\n");
  printf("D-PHY : %u Mbit/s per lane, %u lane(s), %s mapping\n",
         (unsigned)phy->mbps, (unsigned)phy->lanes,
         (phy->swapped != 0U) ? "inverted" : "physical");

  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    if (!info[vc].present)
    {
      continue;
    }

    printf("VC%lu   : %lu.%lu fps, DT 0x%02lx %s",
           (unsigned long)vc,
           (unsigned long)(info[vc].fps_x10 / 10U),
           (unsigned long)(info[vc].fps_x10 % 10U),
           (unsigned long)info[vc].image_dt,
           csi_probe_dt_name(info[vc].image_dt));

    if (info[vc].lines != 0U)
    {
      printf(", %lu lines, %lu bytes/line",
             (unsigned long)info[vc].lines, (unsigned long)info[vc].bytes_per_line);
    }
    if (info[vc].width != 0U)
    {
      printf(" -> %lux%lu", (unsigned long)info[vc].width, (unsigned long)info[vc].lines);
    }
    printf("\n");

    printf("        other packets:");
    for (uint32_t dt = 0U; dt < CSI_PROBE_DT_COUNT; dt++)
    {
      if (dt_seen(&info[vc], dt) && !dt_is_image(dt))
      {
        printf(" 0x%02lx(%s)", (unsigned long)dt, csi_probe_dt_name(dt));
      }
    }
    printf("\n");
  }

  printf("==========================\n\n");
}

void csi_probe_dump_status(void)
{
  uint32_t sr0  = CSI->SR0;
  uint32_t sr1  = CSI->SR1;
  uint32_t err1 = CSI->ERR1;
  uint32_t err2 = CSI->ERR2;

  printf("CSI SR0=0x%08lx SR1=0x%08lx ERR1=0x%08lx ERR2=0x%08lx\n",
         (unsigned long)sr0, (unsigned long)sr1,
         (unsigned long)err1, (unsigned long)err2);

  printf("  dphy:");
  if ((sr1 & CSI_SR1_ACTCLF)     != 0U) { printf(" clk-active"); }
  if ((sr1 & CSI_SR1_ACTDL0F)    != 0U) { printf(" l0-active"); }
  if ((sr1 & CSI_SR1_SYNCDL0F)   != 0U) { printf(" l0-sync"); }
  if ((sr1 & CSI_SR1_ACTDL1F)    != 0U) { printf(" l1-active"); }
  if ((sr1 & CSI_SR1_SYNCDL1F)   != 0U) { printf(" l1-sync"); }
  if ((sr1 & CSI_SR1_PHY_ERRORS) != 0U) { printf(" ERRORS(bitrate/skew?)"); }
  if (sr1 == 0U)                        { printf(" nothing on the lanes"); }
  printf("\n");

  printf("  vc  :");
  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    if ((sr0 & (CSI_SR0_VC0STATEF << vc)) != 0U) { printf(" vc%lu-active", (unsigned long)vc); }
    if ((sr0 & (CSI_SR0_SOF0F << vc))     != 0U) { printf(" sof%lu", (unsigned long)vc); }
    if ((sr0 & (CSI_SR0_EOF0F << vc))     != 0U) { printf(" eof%lu", (unsigned long)vc); }
  }
  printf("\n");

  printf("  err :");
  if ((sr0 & CSI_SR0_SYNCERRF) != 0U)
  {
    printf(" sync(vc%lu)", (unsigned long)((err2 & CSI_ERR2_SYNCVCERR) >> CSI_ERR2_SYNCVCERR_Pos));
  }
  if ((sr0 & CSI_SR0_WDERRF) != 0U)
  {
    printf(" watchdog(vc%lu,no-data)", (unsigned long)((err2 & CSI_ERR2_WDVCERR) >> CSI_ERR2_WDVCERR_Pos));
  }
  if ((sr0 & CSI_SR0_SPKTERRF) != 0U)
  {
    printf(" short-packet(dt0x%02lx)", (unsigned long)((err2 & CSI_ERR2_SPKTDTERR) >> CSI_ERR2_SPKTDTERR_Pos));
  }
  if ((sr0 & CSI_SR0_IDERRF) != 0U)
  {
    printf(" unfiltered-dt(vc%lu,dt0x%02lx)",
           (unsigned long)((err1 & CSI_ERR1_IDVCERR) >> CSI_ERR1_IDVCERR_Pos),
           (unsigned long)((err1 & CSI_ERR1_IDDTERR) >> CSI_ERR1_IDDTERR_Pos));
  }
  if ((sr0 & CSI_SR0_CRCERRF) != 0U)
  {
    printf(" crc(dt0x%02lx)", (unsigned long)((err1 & CSI_ERR1_CRCDTERR) >> CSI_ERR1_CRCDTERR_Pos));
  }
  if ((sr0 & CSI_SR0_ECCERRF) != 0U)
  {
    printf(" ecc-uncorrectable");
  }
  if ((sr0 & CSI_SR0_CECCERRF) != 0U)
  {
    printf(" ecc-corrected(dt0x%02lx)", (unsigned long)((err1 & CSI_ERR1_CECCDTERR) >> CSI_ERR1_CECCDTERR_Pos));
  }
  if ((sr0 & (CSI_SR0_LINK_ERRORS | CSI_SR0_CECCERRF | CSI_SR0_IDERRF)) == 0U)
  {
    printf(" none");
  }
  printf("\n");

  /* Flag-clear registers mirror the status bit positions. */
  CSI->FCR0 = sr0;
  CSI->FCR1 = sr1;
}
