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
#include "stm32n6570_discovery_camera.h"

extern DCMIPP_HandleTypeDef hcamera_dcmipp;

/* Time for the D-PHY to settle after HAL_DCMIPP_CSI_SetConfig() took it through
   reset. Flags latched during that transition say nothing about the link, so
   they are cleared afterwards rather than reported. */
#define CSI_PHY_SETTLE_MS   5U

/* How long to wait for the source to start transmitting. These are timeouts, not
   delays: the wait ends as soon as the receiver reports a high-speed clock.
   Guessing a fixed delay is not an option - too short and every measurement
   afterwards silently characterises a dead link, which is exactly what a 600 ms
   guess did on the first hardware run.
   Driven from the board, the FPGA source measured 6 to 8 seconds to reload its
   configuration. Power-cycled by hand it takes as long as it takes, and the
   operator needs a few seconds on the supply before that even begins. */
#define CSI_SOURCE_AUTO_TIMEOUT_MS    15000U
#define CSI_SOURCE_MANUAL_TIMEOUT_MS  90000U

/* After the clock appears, the lanes still have to synchronise and the first
   frame has to start. */
#define CSI_SOURCE_SYNC_GRACE_MS        300U

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

/* Coarse sweep grid. A D-PHY profile covers a frequency band rather than a
   point, so this ladder finds the region and the caller can refine around the
   winner. 1250 is in the list because it is the per-lane rate of a 2500 Mbit/s
   two-lane link, which is what this board is wired to. */
static const uint16_t csi_scan_mbps[] =
{
  200, 400, 600, 800, 1000, 1100, 1200, 1250, 1300, 1400, 1500, 1600,
  1700, 1800, 1900, 2000, 2100, 2200, 2300, 2400, 2500
};
#define CSI_SCAN_MBPS_COUNT (sizeof(csi_scan_mbps) / sizeof(csi_scan_mbps[0]))

/* Known-good starting point: the setting the encoder application uses. */
const csi_probe_phy_t csi_probe_default_phy = { .mbps = 1250U, .lanes = 2U, .swapped = 0U };

/* Manual by default: the source may well be on a bench supply, with no line back
   to the board for BSP_CAMERA_HwReset() to pull. */
static csi_source_mode_t g_source_mode = CSI_SOURCE_MANUAL;

/* What the D-PHY is currently programmed with, and whether the last apply
   actually reprogrammed it. Together they keep the probe from asking for a
   power-cycle it does not need. mbps 0 means "nothing applied yet". */
static csi_probe_phy_t g_applied_phy;
static bool            g_phy_reapplied;

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

  /* Stopping a channel part way through a frame raises sync and SOT errors of
     the probe's own making. Every caller stops channels only after it has
     finished accumulating, so dropping those flags here loses nothing - and it
     stops the HAL error callback from reporting them the moment the CSI
     interrupt is re-enabled, which read as a link fault in the middle of an
     otherwise error-free measurement. */
  CSI->FCR0 = 0xFFFFFFFFU;
  CSI->FCR1 = 0xFFFFFFFFU;
  hcamera_dcmipp.ErrorCode = HAL_DCMIPP_ERROR_NONE;
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

  /* Applying a setting resets the D-PHY and therefore drops the link, which then
     costs a source restart - a manual one, in the default mode. Re-applying the
     setting that is already programmed would do that for nothing. */
  if ((g_applied_phy.mbps == phy->mbps) &&
      (g_applied_phy.lanes == phy->lanes) &&
      (g_applied_phy.swapped == phy->swapped))
  {
    g_phy_reapplied = false;
    return HAL_OK;
  }
  g_phy_reapplied = true;

  csi_stop_all_vc();

  csiconf.NumberOfLanes   = (phy->lanes == 1U) ? DCMIPP_CSI_ONE_DATA_LANE
                                               : DCMIPP_CSI_TWO_DATA_LANES;
  csiconf.DataLaneMapping = (phy->swapped != 0U) ? DCMIPP_CSI_INVERTED_DATA_LANES
                                                 : DCMIPP_CSI_PHYSICAL_DATA_LANES;
  csiconf.PHYBitrate      = csi_probe_bitrate_profile(phy->mbps, NULL);

  if (HAL_DCMIPP_CSI_SetConfig(&hcamera_dcmipp, &csiconf) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* SetConfig() drives the D-PHY through reset and toggles its test interface.
     The flags that come out of that are an artefact of the reconfiguration, not
     a property of the link, so let it settle and drop them. Without this the
     sweep reports SOT errors on a random-looking subset of bitrates. */
  HAL_Delay(CSI_PHY_SETTLE_MS);
  CSI->FCR0 = 0xFFFFFFFFU;
  CSI->FCR1 = 0xFFFFFFFFU;

  g_applied_phy = *phy;
  return HAL_OK;
}

void csi_probe_source_restart(uint32_t settle_ms)
{
  /* Cycles the camera connector's power and reset lines. This has to happen
     *after* the receiver is configured: a D-PHY transmitter that starts while
     the receiver is still in reset is never picked up, and every bitrate change
     puts the receiver through reset again. */
  (void)BSP_CAMERA_HwReset(0);
  HAL_Delay(settle_ms);
}

void csi_probe_set_source_mode(csi_source_mode_t mode)
{
  g_source_mode = mode;
}

csi_source_mode_t csi_probe_get_source_mode(void)
{
  return g_source_mode;
}

uint32_t csi_probe_source_cycle_wait(uint32_t timeout_ms)
{
  uint32_t tickstart;
  uint32_t elapsed  = 0U;
  uint32_t next_dot = 1000U;

  if (timeout_ms == 0U)
  {
    timeout_ms = (g_source_mode == CSI_SOURCE_AUTO) ? CSI_SOURCE_AUTO_TIMEOUT_MS
                                                    : CSI_SOURCE_MANUAL_TIMEOUT_MS;
  }

  if (g_source_mode == CSI_SOURCE_AUTO)
  {
    (void)BSP_CAMERA_HwReset(0);
  }
  else
  {
    printf("CSI: power-cycle the source now (waiting up to %lu s for its clock)\n",
           (unsigned long)(timeout_ms / 1000U));
  }

  tickstart = HAL_GetTick();
  while ((CSI->SR1 & CSI_SR1_ACTCLF) == 0U)
  {
    elapsed = HAL_GetTick() - tickstart;
    if (elapsed >= timeout_ms)
    {
      printf("\nCSI: no clock after %lu s\n", (unsigned long)(timeout_ms / 1000U));
      return 0U;
    }

    /* One dot per second, so a long manual wait does not look like a hang. */
    if ((g_source_mode == CSI_SOURCE_MANUAL) && (elapsed >= next_dot))
    {
      printf(".");
      fflush(stdout);
      next_dot += 1000U;
    }
  }

  if (g_source_mode == CSI_SOURCE_MANUAL)
  {
    printf("\n");
  }

  /* Clock is there; give the lanes time to synchronise and a frame to start. */
  HAL_Delay(CSI_SOURCE_SYNC_GRACE_MS);
  return elapsed + 1U;
}

/* ------------------------------------------------------------------------- */
/* Observation                                                               */
/* ------------------------------------------------------------------------- */

bool csi_probe_observe(const csi_probe_phy_t *phy, uint32_t window_ms,
                       bool restart_source, csi_probe_result_t *out)
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

  /* Receiver is fully up now; only then is it worth restarting the source - and
     only if the link is not already running. Asking a human to power-cycle
     something that is already transmitting is how a diagnostic tool gets
     abandoned. */
  if (restart_source)
  {
    if (g_phy_reapplied || ((CSI->SR1 & CSI_SR1_ACTCLF) == 0U))
    {
      out->source_boot_ms = csi_probe_source_cycle_wait(0U);
    }
    else
    {
      out->source_boot_ms = 1U;   /* already transmitting */
    }
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

    /* Counting errors rather than only flagging them is what makes neighbouring
       bitrate profiles comparable: a link can be up and still be marginal, and
       the difference only shows as a rate. */
    if ((sr0 & CSI_SR0_ECCERRF)  != 0U) { out->err_ecc++; }
    if ((sr0 & CSI_SR0_CECCERRF) != 0U) { out->err_ecc_corrected++; }
    if ((sr0 & CSI_SR0_CRCERRF)  != 0U) { out->err_crc++; }
    if ((sr1 & CSI_SR1_PHY_ERRORS) != 0U) { out->err_phy++; }

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

/**
  * @brief  Render one channel's frame count into @p buf.
  * @note   "-" nothing, "12" twelve complete frames, "12!" twelve frames started
  *         but none finished - the difference between "no data" and "data that
  *         does not survive the link".
  */
static void csi_format_vc(char *buf, size_t len, uint32_t sof, uint32_t eof)
{
  if (eof != 0U)
  {
    (void)snprintf(buf, len, "%lu", (unsigned long)eof);
  }
  else if (sof != 0U)
  {
    (void)snprintf(buf, len, "%lu!", (unsigned long)sof);
  }
  else
  {
    (void)snprintf(buf, len, "-");
  }
}

static void csi_print_scan_row(const csi_probe_result_t *r)
{
  char vc_text[CSI_PROBE_VC_COUNT][12];

  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    csi_format_vc(vc_text[vc], sizeof(vc_text[vc]), r->sof[vc], r->eof[vc]);
  }

  /* The virtual channel state flag is deliberately not shown: it reports that a
     channel was started, not that anything arrived on it, so it reads "all four
     present" even on a dead link. The frame counts are the honest signal. */
  printf("  %4u  %u %s | clk %c  l0 %c%c  l1 %c%c | %5s %5s %5s %5s |",
         (unsigned)r->phy.mbps,
         (unsigned)r->phy.lanes,
         (r->phy.swapped != 0U) ? "swap" : "norm",
         ((r->sr1 & CSI_SR1_ACTCLF)   != 0U) ? 'A' : '-',
         ((r->sr1 & CSI_SR1_ACTDL0F)  != 0U) ? 'A' : '-',
         ((r->sr1 & CSI_SR1_SYNCDL0F) != 0U) ? 'S' : '-',
         ((r->sr1 & CSI_SR1_ACTDL1F)  != 0U) ? 'A' : '-',
         ((r->sr1 & CSI_SR1_SYNCDL1F) != 0U) ? 'S' : '-',
         vc_text[0], vc_text[1], vc_text[2], vc_text[3]);

  /* Counts, not just flags: a link can be up and still be marginal, and picking
     between neighbouring bitrate profiles is a comparison of rates. */
  printf(" %3lu %3lu %3lu", (unsigned long)r->err_ecc, (unsigned long)r->err_crc,
         (unsigned long)r->err_phy);

  if ((r->sr0 & CSI_SR0_SYNCERRF) != 0U) { printf(" SYNC"); }
  if ((r->sr0 & CSI_SR0_WDERRF)   != 0U) { printf(" WDG"); }
  if ((r->sr0 & CSI_SR0_SPKTERRF) != 0U) { printf(" SPKT"); }
  printf("\n");
}

static uint32_t csi_result_frames(const csi_probe_result_t *r)
{
  return r->eof[0] + r->eof[1] + r->eof[2] + r->eof[3];
}

bool csi_probe_scan(uint32_t window_ms, bool restart_source, csi_probe_phy_t *best)
{
  csi_probe_result_t result;
  csi_probe_phy_t    winner       = {0};
  uint32_t           best_frames  = 0U;
  uint32_t           best_errors  = 0xFFFFFFFFU;
  bool               best_clean   = false;
  bool               found        = false;
  bool               clock_seen   = false;
  uint32_t           combinations = CSI_SCAN_MBPS_COUNT * 4U;

  if (restart_source && (g_source_mode == CSI_SOURCE_MANUAL))
  {
    /* One prompt per combination is not a workflow. Fall back to a single
       restart up front and say so. */
    printf("CSI: source mode is manual, so a restart per combination would mean %lu\n"
           "     power-cycles by hand. Doing one up front instead; 'refine' is the\n"
           "     command for comparing a handful of profiles properly.\n",
           (unsigned long)combinations);
    restart_source = false;
  }

  printf("CSI: scanning %u bitrates x 2 lane counts x 2 mappings, %lu ms each%s\n",
         (unsigned)CSI_SCAN_MBPS_COUNT, (unsigned long)window_ms,
         restart_source ? ", restarting the source each time" : "");

  if (restart_source)
  {
    /* Each iteration pays for the reset sequence plus the source's boot time,
       and that boot time was measured in seconds, not milliseconds. */
    printf("CSI: this restarts the source %lu times; expect several minutes\n",
           (unsigned long)combinations);
  }
  else
  {
    /* One restart up front, then sweep against a transmitter that keeps running.
       Whether it survives the receiver resets in between is exactly what the
       table will show: if only the first rows carry frames, it does not, and the
       sweep is not a usable instrument for this source. */
    uint32_t boot_ms;

    printf("CSI: starting the source once, then sweeping without restarting it\n");
    HAL_NVIC_DisableIRQ(CSI_IRQn);
    boot_ms = csi_probe_source_cycle_wait(0U);
    HAL_NVIC_EnableIRQ(CSI_IRQn);

    if (boot_ms == 0U)
    {
      printf("CSI: the source never produced a clock; nothing to sweep against.\n");
      return false;
    }
    printf("CSI: source transmitting after %lu ms\n", (unsigned long)(boot_ms - 1U));
  }

  printf("  mbps  ln map  | dphy activity      | complete frames per vc (N! = none finished) | ecc crc phy\n");

  for (uint32_t lanes = 2U; lanes >= 1U; lanes--)
  {
    for (uint32_t swapped = 0U; swapped <= 1U; swapped++)
    {
      for (uint32_t i = 0U; i < CSI_SCAN_MBPS_COUNT; i++)
      {
        csi_probe_phy_t phy;
        uint32_t        frames;
        uint32_t        errors;
        bool            clean;

        phy.mbps    = csi_scan_mbps[i];
        phy.lanes   = (uint8_t)lanes;
        phy.swapped = (uint8_t)swapped;

        (void)csi_probe_observe(&phy, window_ms, restart_source, &result);

        if ((result.sr1 & CSI_SR1_ACTCLF) != 0U)
        {
          clock_seen = true;
        }

        frames = csi_result_frames(&result);
        errors = result.err_ecc + result.err_crc + result.err_phy;
        clean  = (errors == 0U);

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

        /* Rank by frames first, then by how quiet the link was. Frames are the
           thing being asked for; error count is what separates two settings that
           both deliver them, which is the case that matters when the source sits
           between two D-PHY profiles. */
        if ((frames > best_frames) ||
            ((frames == best_frames) && (errors < best_errors)))
        {
          best_clean  = clean;
          best_frames = frames;
          best_errors = errors;
          winner      = phy;
        }
      }
    }
  }

  if (!found)
  {
    printf("CSI: no virtual channel produced a complete frame at any setting.\n");
    if (!clock_seen)
    {
      /* Without a clock the bitrate is irrelevant: nothing is being transmitted,
         or the transmitter came up before the receiver did. */
      printf("     The D-PHY clock lane was never active, at any bitrate. That is not a\n");
      printf("     bitrate mismatch - no high-speed clock reached the receiver at all.\n");
      printf("     Try 'power cycle' followed by 'link', and check that the source is\n");
      printf("     actually transmitting and that its power rail comes up.\n");
    }
    else
    {
      printf("     A clock was seen but no frame completed. Refine the bitrate around the\n");
      printf("     rows that showed lane sync, and check the lane mapping.\n");
    }
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
/* Link watch                                                                */
/* ------------------------------------------------------------------------- */

bool csi_probe_wait_for_link(const csi_probe_phy_t *phy, uint32_t timeout_ms)
{
  uint32_t tickstart;
  uint32_t seen_sr0 = 0U;
  uint32_t seen_sr1 = 0U;
  uint32_t t_clock  = 0U;
  uint32_t t_sync   = 0U;
  uint32_t t_frame  = 0U;

  if (phy == NULL)
  {
    return false;
  }

  if (timeout_ms == 0U)
  {
    timeout_ms = (g_source_mode == CSI_SOURCE_AUTO) ? CSI_SOURCE_AUTO_TIMEOUT_MS
                                                    : CSI_SOURCE_MANUAL_TIMEOUT_MS;
  }

  HAL_NVIC_DisableIRQ(CSI_IRQn);

  if (csi_probe_apply_phy(phy) != HAL_OK)
  {
    HAL_NVIC_EnableIRQ(CSI_IRQn);
    printf("CSI: applying %u Mbit/s failed\n", (unsigned)phy->mbps);
    return false;
  }

  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc, DCMIPP_CSI_DT_BPP8);
    SET_BIT(CSI->CR, vc_start_bit[vc]);
  }

  printf("CSI: receiver up at %u Mbit/s, %u lane(s), %s mapping. Watching for %lu ms.\n",
         (unsigned)phy->mbps, (unsigned)phy->lanes,
         (phy->swapped != 0U) ? "inverted" : "physical", (unsigned long)timeout_ms);

  if (g_source_mode == CSI_SOURCE_AUTO)
  {
    printf("CSI: cycling the source's power and reset lines\n");
    /* No settle delay here: the point of this function is to watch the link come
       up, so the wait belongs inside the loop where it can be timed. */
    (void)BSP_CAMERA_HwReset(0);
  }
  else
  {
    printf("CSI: >>> power-cycle the source now <<<\n");
  }

  CSI->FCR0 = 0xFFFFFFFFU;
  CSI->FCR1 = 0xFFFFFFFFU;

  tickstart = HAL_GetTick();
  while ((HAL_GetTick() - tickstart) < timeout_ms)
  {
    uint32_t sr0     = CSI->SR0;
    uint32_t sr1     = CSI->SR1;
    uint32_t elapsed = HAL_GetTick() - tickstart;

    if (((sr1 & CSI_SR1_ACTCLF) != 0U) && (t_clock == 0U))
    {
      t_clock = elapsed + 1U;
    }
    if (((sr1 & (CSI_SR1_SYNCDL0F | CSI_SR1_SYNCDL1F)) != 0U) && (t_sync == 0U))
    {
      t_sync = elapsed + 1U;
    }
    if (((sr0 & (CSI_SR0_EOF0F | CSI_SR0_EOF1F | CSI_SR0_EOF2F | CSI_SR0_EOF3F)) != 0U) &&
        (t_frame == 0U))
    {
      t_frame = elapsed + 1U;
    }

    seen_sr0 |= sr0;
    seen_sr1 |= sr1;

    /* Keep the frame flags moving so a stalled link is distinguishable from one
       that delivered a single frame and stopped. */
    CSI->FCR0 = sr0 & (CSI_SR0_SOF0F | CSI_SR0_SOF1F | CSI_SR0_SOF2F | CSI_SR0_SOF3F |
                       CSI_SR0_EOF0F | CSI_SR0_EOF1F | CSI_SR0_EOF2F | CSI_SR0_EOF3F);

    if ((t_frame != 0U) && (elapsed > (t_frame + 200U)))
    {
      break;   /* frames are flowing, no need to sit out the whole timeout */
    }
  }

  csi_stop_all_vc();
  HAL_NVIC_EnableIRQ(CSI_IRQn);

  printf("CSI: clock %s", (t_clock != 0U) ? "" : "never active");
  if (t_clock != 0U) { printf("active after %lu ms", (unsigned long)(t_clock - 1U)); }
  printf(", lane sync %s", (t_sync != 0U) ? "" : "never");
  if (t_sync != 0U) { printf("after %lu ms", (unsigned long)(t_sync - 1U)); }
  printf(", first frame %s", (t_frame != 0U) ? "" : "never");
  if (t_frame != 0U) { printf("after %lu ms", (unsigned long)(t_frame - 1U)); }
  printf("\n");

  if (t_clock == 0U)
  {
    printf("CSI: no high-speed clock. The bitrate setting is irrelevant until this\n");
    printf("     changes - the source is not transmitting, or its rail never came up.\n");
  }
  else if (t_sync == 0U)
  {
    printf("CSI: clock but no lane sync - this is where the bitrate setting matters.\n");
  }
  else if (t_frame == 0U)
  {
    printf("CSI: lanes in sync but no end-of-frame - check the lane mapping and\n");
    printf("     whether the source sends frame start/end short packets.\n");
  }

  return t_frame != 0U;
}

/* ------------------------------------------------------------------------- */
/* Refine                                                                    */
/* ------------------------------------------------------------------------- */

bool csi_probe_refine(csi_probe_phy_t *phy, uint32_t neighbours, uint32_t window_ms)
{
  csi_probe_result_t result;
  csi_probe_phy_t    winner      = {0};
  uint32_t           centre_idx;
  uint32_t           first_idx;
  uint32_t           last_idx;
  uint32_t           best_frames = 0U;
  uint32_t           best_errors = 0xFFFFFFFFU;
  bool               found       = false;

  if (phy == NULL)
  {
    return false;
  }

  centre_idx = csi_probe_bitrate_profile(phy->mbps, NULL);
  first_idx  = (centre_idx > neighbours) ? (centre_idx - neighbours) : 0U;
  last_idx   = centre_idx + neighbours;
  if (last_idx >= CSI_PHY_PROFILE_COUNT)
  {
    last_idx = CSI_PHY_PROFILE_COUNT - 1U;
  }

  printf("CSI: comparing profiles %u..%u Mbit/s around %u, %u lane(s), %s mapping\n",
         (unsigned)csi_phy_profile_mbps[first_idx],
         (unsigned)csi_phy_profile_mbps[last_idx],
         (unsigned)phy->mbps, (unsigned)phy->lanes,
         (phy->swapped != 0U) ? "inverted" : "physical");
  printf("CSI: %lu profiles, each needs the source restarted\n",
         (unsigned long)((last_idx - first_idx) + 1U));
  printf("  mbps  | complete frames per vc (N! = N started, none finished) | ecc crc phy\n");

  for (uint32_t idx = first_idx; idx <= last_idx; idx++)
  {
    csi_probe_phy_t candidate = *phy;
    char            vc_text[CSI_PROBE_VC_COUNT][12];
    uint32_t        frames;
    uint32_t        errors;

    candidate.mbps = csi_phy_profile_mbps[idx];

    (void)csi_probe_observe(&candidate, window_ms, true, &result);

    if (result.source_boot_ms == 0U)
    {
      printf("  %4u  | source did not start\n", (unsigned)candidate.mbps);
      continue;
    }

    for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
    {
      csi_format_vc(vc_text[vc], sizeof(vc_text[vc]), result.sof[vc], result.eof[vc]);
    }

    frames = csi_result_frames(&result);
    errors = result.err_ecc + result.err_crc + result.err_phy;

    printf("  %4u  | %5s %5s %5s %5s | %3lu %3lu %3lu\n",
           (unsigned)candidate.mbps,
           vc_text[0], vc_text[1], vc_text[2], vc_text[3],
           (unsigned long)result.err_ecc, (unsigned long)result.err_crc,
           (unsigned long)result.err_phy);

    if (frames == 0U)
    {
      continue;
    }
    found = true;

    /* Frames first, then quietness - the same ranking the sweep uses, which is
       what makes the two comparable. */
    if ((frames > best_frames) || ((frames == best_frames) && (errors < best_errors)))
    {
      best_frames = frames;
      best_errors = errors;
      winner      = candidate;
    }
  }

  if (!found)
  {
    printf("CSI: no profile in that range delivered a frame\n");
    return false;
  }

  /* "frames" here means frames that finished. A row showing 20! delivered no
     complete frame at all and so ranks below a row with a single real one, which
     looks wrong at a glance unless the units are spelled out. */
  printf("CSI: quietest is %u Mbit/s (%lu complete frames, %lu errors)\n",
         (unsigned)winner.mbps, (unsigned long)best_frames, (unsigned long)best_errors);
  printf("CSI: one window per profile and one restart each, so a small difference\n");
  printf("     between neighbours is noise rather than a ranking. Repeat to confirm.\n");
  *phy = winner;
  return true;
}

/* ------------------------------------------------------------------------- */
/* Data type identification                                                  */
/* ------------------------------------------------------------------------- */

void csi_probe_datatypes(uint32_t vc, uint32_t window_ms, csi_probe_vc_info_t *info)
{
  DCMIPP_CSI_VCFilteringConfTypeDef filter = {0};
  uint32_t tickstart;
  static uint32_t dt_count[CSI_PROBE_DT_COUNT];   /* static: 256 B off the stack */

  if ((info == NULL) || (vc >= CSI_PROBE_VC_COUNT))
  {
    return;
  }

  info->dt_mask_lo = 0U;
  info->dt_mask_hi = 0U;
  info->image_dt   = 0U;
  memset(dt_count, 0, sizeof(dt_count));

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
        if (dt_count[err_dt] < 0xFFFFFFFFU)
        {
          dt_count[err_dt]++;
        }
      }
    }
  }

  csi_stop_all_vc();

  /* Put the channel back to "accept everything" so nothing downstream trips
     over a filter the probe left behind. */
  (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc, DCMIPP_CSI_DT_BPP8);
  HAL_NVIC_EnableIRQ(CSI_IRQn);

  /* The image data type is the long-packet type that arrived most often.
     Picking the lowest-numbered one instead would be wrong on a marginal link:
     an ECC error the receiver cannot correct turns a data type into a
     neighbouring value, and RAW10 (0x2b) corrupted into 0x2f is indistinguishable
     from a genuine data type on a single observation. The real one dominates by
     orders of magnitude, a corrupted one appears a handful of times. */
  {
    uint32_t best_count = 0U;

    for (uint32_t dt = 0U; dt < CSI_PROBE_DT_COUNT; dt++)
    {
      if (dt_is_image(dt) && (dt_count[dt] > best_count))
      {
        best_count     = dt_count[dt];
        info->image_dt = dt;
      }
    }
    info->image_dt_count = best_count;

    /* Anything else that looked like an image type is header corruption, and
       saying so is more useful than silently dropping it. */
    info->dt_corrupt = 0U;
    for (uint32_t dt = 0U; dt < CSI_PROBE_DT_COUNT; dt++)
    {
      if (dt_is_image(dt) && (dt != info->image_dt) && (dt_count[dt] != 0U))
      {
        info->dt_corrupt += dt_count[dt];
      }
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

void csi_probe_report_invalidate(csi_probe_report_t *report)
{
  if (report != NULL)
  {
    memset(report, 0, sizeof(*report));
  }
}

void csi_probe_run(csi_probe_phy_t *phy, csi_probe_report_t *report)
{
  csi_probe_result_t   result;
  csi_probe_vc_info_t *info;
  const uint32_t       window_ms = 500U;

  if ((phy == NULL) || (report == NULL))
  {
    return;
  }

  /* Cleared up front: a run that bails out half way must not leave the previous
     one's numbers behind to be reprinted later. */
  csi_probe_report_invalidate(report);
  info = report->vc;

  if (phy->mbps == 0U)
  {
    /* Try the setting the encoder application uses before sweeping: a sweep
       costs a source restart per combination, and this is very often the answer.
       The restart is what makes it work at all - the transmitter has to come up
       after the receiver, and every bitrate change resets the receiver. */
    csi_probe_phy_t candidate = csi_probe_default_phy;

    printf("CSI: trying the known-good setting first (%u Mbit/s, %u lanes)\n",
           (unsigned)candidate.mbps, (unsigned)candidate.lanes);

    if (csi_probe_wait_for_link(&candidate, 0U))
    {
      *phy = candidate;
    }
    else if (!csi_probe_scan(150U, true, phy))
    {
      return;
    }
  }

  /* Re-measure the chosen setting over a longer window so the per-channel frame
     rate is worth printing. */
  (void)csi_probe_observe(phy, window_ms, true, &result);

  if (result.source_boot_ms == 0U)
  {
    printf("CSI: the source never started; nothing to characterise\n");
    return;
  }
  printf("CSI: source transmitting %lu ms after its restart\n",
         (unsigned long)(result.source_boot_ms - 1U));

  report->phy           = *phy;
  report->link          = result;
  report->link_measured = true;
  report->valid         = true;

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

  /* Leave each present channel configured for the format it actually carries.
     The D-PHY is deliberately not re-applied here: it already holds this setting
     and the link is up, so touching it would reset the receiver and cost another
     power-cycle for no gain. Only the per-channel format registers are written,
     which the transmitter never sees. */
  for (uint32_t vc = 0U; vc < CSI_PROBE_VC_COUNT; vc++)
  {
    if (info[vc].present)
    {
      (void)HAL_DCMIPP_CSI_SetVCConfig(&hcamera_dcmipp, vc,
                                       csi_probe_dt_bpp_code(info[vc].image_dt));
    }
  }
}

void csi_probe_print_report(const csi_probe_report_t *report, const csi_probe_phy_t *current)
{
  const csi_probe_vc_info_t *info;
  const csi_probe_phy_t     *phy;

  if (report == NULL)
  {
    return;
  }

  if (!report->valid)
  {
    printf("CSI: nothing measured yet - run 'probe'\n");
    return;
  }

  info = report->vc;
  phy  = &report->phy;

  printf("\n=== CSI-2 probe result ===\n");
  printf("D-PHY : %u Mbit/s per lane, %u lane(s), %s mapping\n",
         (unsigned)phy->mbps, (unsigned)phy->lanes,
         (phy->swapped != 0U) ? "inverted" : "physical");

  /* The setting printed above is the one these numbers were taken at, not
     whatever the receiver holds now. Say so when they have drifted apart. */
  if ((current != NULL) &&
      ((current->mbps != phy->mbps) || (current->lanes != phy->lanes) ||
       (current->swapped != phy->swapped)))
  {
    printf("        (the receiver now holds %u Mbit/s, %u lane(s), %s mapping;\n"
           "         these numbers are from the setting above - rerun 'probe')\n",
           (unsigned)current->mbps, (unsigned)current->lanes,
           (current->swapped != 0U) ? "inverted" : "physical");
  }

  if (!report->link_measured)
  {
    printf("Link  : not measured in this run\n");
  }
  else if ((report->link.err_ecc | report->link.err_crc | report->link.err_phy) != 0U)
  {
    printf("Link  : MARGINAL over %lu ms - ecc %lu uncorrectable / %lu corrected, "
           "crc %lu, dphy %lu\n",
           (unsigned long)report->link.window_ms,
           (unsigned long)report->link.err_ecc,
           (unsigned long)report->link.err_ecc_corrected,
           (unsigned long)report->link.err_crc,
           (unsigned long)report->link.err_phy);
    printf("        'refine' compares the neighbouring bitrate profiles;\n");
    printf("        an uncorrectable header ECC error changes the data type value.\n");
  }
  else
  {
    printf("Link  : clean over %lu ms, no ecc/crc/dphy errors\n",
           (unsigned long)report->link.window_ms);
  }

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

    if (info[vc].dt_corrupt != 0U)
    {
      /* Other image data types were seen. On a clean link that would mean the
         source really sends two of them; here it almost always means corrupted
         headers, so give the ratio rather than a verdict. */
      printf("        data type seen %lu times, %lu other image types "
             "(header corruption unless the source really sends both)\n",
             (unsigned long)info[vc].image_dt_count, (unsigned long)info[vc].dt_corrupt);
    }

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
