/**
  ******************************************************************************
  * @file    csi_probe.h
  * @brief   Self-contained MIPI CSI-2 receiver probe for an unknown source.
  *
  * The probe answers three questions about a CSI-2 stream that is generated
  * externally (no I2C sensor is driven from the STM32):
  *
  *   1. which D-PHY setting locks         -> lanes, lane mapping, bitrate profile
  *   2. which virtual channels are present -> per-VC start/end-of-frame counts
  *   3. what each VC carries              -> data type and frame geometry
  *
  * It touches only the CSI block, never a DCMIPP pixel pipe, so it works before
  * anything is known about the pixel format and cannot corrupt memory.
  * Everything is polled; the CSI interrupt is masked for the duration of a
  * measurement so an error storm cannot flood the CPU.
  ******************************************************************************
  */

#ifndef CSI_PROBE_H
#define CSI_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32n6xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSI_PROBE_VC_COUNT     4U
#define CSI_PROBE_DT_COUNT    64U   /* CSI-2 data type is 6 bit */

/** D-PHY receiver setting under test. */
typedef struct
{
  uint16_t mbps;     /*!< per-lane bitrate, snapped to a HAL DCMIPP_CSI_PHY_BT_xxx profile */
  uint8_t  lanes;    /*!< 1 or 2; the STM32N6 D-PHY receiver has no 4-lane mode            */
  uint8_t  swapped;  /*!< 0 = physical lane mapping, 1 = inverted (lane 0 <-> lane 1)      */
} csi_probe_phy_t;

/** Result of one observation window. */
typedef struct
{
  csi_probe_phy_t phy;
  uint32_t window_ms;
  uint32_t sof[CSI_PROBE_VC_COUNT];  /*!< start-of-frame short packets seen per VC */
  uint32_t eof[CSI_PROBE_VC_COUNT];  /*!< end-of-frame short packets seen per VC   */
  uint32_t vc_state_mask;            /*!< bit n: VCn reached the active state       */
  uint32_t sr0;                      /*!< sticky OR of CSI_SR0 over the window      */
  uint32_t sr1;                      /*!< sticky OR of CSI_SR1 over the window      */
} csi_probe_result_t;

/** What one virtual channel turned out to carry. */
typedef struct
{
  bool     present;                  /*!< at least one complete frame was observed */
  uint32_t frames;                   /*!< frames counted during the window         */
  uint32_t fps_x10;                  /*!< frame rate * 10, derived from the window */
  uint32_t dt_mask_lo;               /*!< observed data types 0..31,  bit = DT      */
  uint32_t dt_mask_hi;               /*!< observed data types 32..63, bit = DT - 32 */
  uint32_t image_dt;                 /*!< the long-packet data type, 0 if unknown   */
  uint32_t lines;                    /*!< lines per frame, 0 if not measured        */
  uint32_t bytes_per_line;           /*!< payload bytes per line, 0 if not measured */
  uint32_t width;                    /*!< derived from bytes_per_line and image_dt  */
} csi_probe_vc_info_t;

/** The setting the encoder application uses: 2500 Mbit/s over two lanes. */
extern const csi_probe_phy_t csi_probe_default_phy;

/**
  * @brief  Cycle the camera connector's power and reset lines.
  * @param  settle_ms  time to wait afterwards for the source to boot
  * @note   Order matters. A D-PHY transmitter that starts while the receiver is
  *         still in reset is never picked up, and every bitrate change puts the
  *         receiver through reset, so the source has to be restarted after the
  *         receiver is configured - not before.
  */
void csi_probe_source_restart(uint32_t settle_ms);

/**
  * @brief  Apply @p phy, optionally restart the source, and watch the link come up.
  * @param  timeout_ms      how long to watch
  * @param  restart_source  cycle the source's power lines after the receiver is up
  * @retval true if a complete frame arrived
  *
  * Reports when the high-speed clock appeared, when the lanes synchronised and
  * when the first frame ended. Those three tell apart "nothing is transmitting",
  * "wrong bitrate" and "wrong lane mapping or no frame delimiters".
  */
bool csi_probe_wait_for_link(const csi_probe_phy_t *phy, uint32_t timeout_ms, bool restart_source);

/**
  * @brief  Snap a requested per-lane bitrate to the nearest HAL D-PHY profile.
  * @param  mbps          requested bitrate in Mbit/s per lane
  * @param  applied_mbps  optional out: the bitrate of the profile that was picked
  * @retval the DCMIPP_CSI_PHY_BT_xxx profile index
  */
uint32_t csi_probe_bitrate_profile(uint32_t mbps, uint32_t *applied_mbps);

/**
  * @brief  Program the D-PHY receiver for @p phy and leave it enabled.
  * @note   All virtual channels are stopped first; none is started.
  */
HAL_StatusTypeDef csi_probe_apply_phy(const csi_probe_phy_t *phy);

/**
  * @brief  Observe all four virtual channels for @p window_ms with @p phy applied.
  */
bool csi_probe_observe(const csi_probe_phy_t *phy, uint32_t window_ms,
                       bool restart_source, csi_probe_result_t *out);

/**
  * @brief  Sweep lane count, lane mapping and bitrate; print a table of what locks.
  * @param  window_ms       observation window per combination (>= 100 ms recommended)
  * @param  restart_source   cycle the source's power lines for every combination.
  *                          Needed whenever the transmitter only synchronises if
  *                          it starts after the receiver, which costs a reset
  *                          sequence plus its boot time per combination.
  * @param  best             optional out: the setting with the most frames and no
  *                          D-PHY error
  * @retval true if at least one combination produced complete frames
  */
bool csi_probe_scan(uint32_t window_ms, bool restart_source, csi_probe_phy_t *best);

/**
  * @brief  Identify the data types carried by @p vc.
  * @note   Works by narrowing the virtual channel's data type filter to a value
  *         the source cannot be sending, so every packet raises an ID error and
  *         CSI_ERR1 reports the data type that actually arrived.
  */
void csi_probe_datatypes(uint32_t vc, uint32_t window_ms, csi_probe_vc_info_t *info);

/**
  * @brief  Measure lines per frame and payload bytes per line of @p vc.
  * @note   Binary search on the CSI line/byte counter; no memory is written by
  *         the DCMIPP, so this is safe with an unknown resolution.
  */
void csi_probe_geometry(uint32_t vc, csi_probe_vc_info_t *info);

/**
  * @brief  Full run: scan, then characterise every virtual channel that is present.
  * @param  phy   in/out: if phy->mbps is 0 a full sweep is run and the winner is
  *               written back; otherwise only that setting is characterised
  * @param  info  out: one entry per virtual channel
  */
void csi_probe_run(csi_probe_phy_t *phy, csi_probe_vc_info_t info[CSI_PROBE_VC_COUNT]);

/** @brief Print a human readable summary of a completed run. */
void csi_probe_print_report(const csi_probe_phy_t *phy,
                            const csi_probe_vc_info_t info[CSI_PROBE_VC_COUNT]);

/** @brief Print CSI_SR0/CSI_SR1/CSI_ERR1/CSI_ERR2 decoded, without changing anything. */
void csi_probe_dump_status(void);

/** @brief Name of a CSI-2 data type, or "?" when unknown. */
const char *csi_probe_dt_name(uint32_t dt);

/** @brief Bits per pixel of a CSI-2 data type, 0 when unknown or not a pixel type. */
uint32_t csi_probe_dt_bpp(uint32_t dt);

/** @brief HAL DCMIPP_CSI_DT_BPPxx code for a data type, DCMIPP_CSI_DT_BPP8 as fallback. */
uint32_t csi_probe_dt_bpp_code(uint32_t dt);

#ifdef __cplusplus
}
#endif

#endif /* CSI_PROBE_H */
