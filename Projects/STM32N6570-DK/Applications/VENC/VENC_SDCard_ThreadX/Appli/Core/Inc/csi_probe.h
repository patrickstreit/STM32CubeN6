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
  uint32_t source_boot_ms;           /*!< time from source restart to clock, 0 = never */
  /* Polling samples in which the flag was set, not packet counts. Only useful as
     a relative measure between settings observed for the same window length -
     which is exactly what picking a bitrate profile needs. */
  uint32_t err_ecc;                  /*!< uncorrectable header ECC errors */
  uint32_t err_ecc_corrected;        /*!< corrected header ECC errors     */
  uint32_t err_crc;                  /*!< payload CRC errors              */
  uint32_t err_phy;                  /*!< D-PHY SOT/escape/control errors */
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
  uint32_t image_dt_count;           /*!< how often it was observed                 */
  uint32_t dt_corrupt;               /*!< observations of other image data types,
                                          i.e. headers the ECC could not repair    */
  uint32_t lines;                    /*!< lines per frame, 0 if not measured        */
  uint32_t bytes_per_line;           /*!< payload bytes per line, 0 if not measured */
  uint32_t width;                    /*!< derived from bytes_per_line and image_dt  */
} csi_probe_vc_info_t;

/** The setting the encoder application uses: 2500 Mbit/s over two lanes. */
extern const csi_probe_phy_t csi_probe_default_phy;

/**
  * How the CSI-2 source gets restarted.
  *
  * Order matters either way: a D-PHY transmitter that starts while the receiver
  * is still in reset is never picked up, and every bitrate change puts the
  * receiver through reset. So the source has to come up *after* the receiver.
  */
typedef enum
{
  /** The operator power-cycles it. The probe prints a prompt and then waits for
      the clock to appear. This is the default, because a source on a bench
      supply has no line back to the board for the firmware to pull. */
  CSI_SOURCE_MANUAL = 0,
  /** The source hangs off the camera connector, so EN_CAM / NRST_CAM restart it. */
  CSI_SOURCE_AUTO
} csi_source_mode_t;

void              csi_probe_set_source_mode(csi_source_mode_t mode);
csi_source_mode_t csi_probe_get_source_mode(void);

/**
  * @brief  Restart the source - or ask for it to be restarted - and wait for its
  *         high-speed clock to reach the receiver.
  * @param  timeout_ms  0 for the default, which depends on the source mode
  * @retval milliseconds from the request to the clock appearing, plus one;
  *         0 means the clock never appeared.
  *
  * The wait is a timeout, not a delay: it ends as soon as there is a clock. That
  * matters because the transmitter's start-up time is not knowable in advance -
  * an FPGA reloads its configuration first, and a human takes even longer.
  */
uint32_t csi_probe_source_cycle_wait(uint32_t timeout_ms);

/** @brief Drive EN_CAM / NRST_CAM regardless of the source mode, then settle. */
void csi_probe_source_restart(uint32_t settle_ms);

/**
  * @brief  Apply @p phy, get the source restarted, and watch the link come up.
  * @param  timeout_ms  how long to watch, 0 for the source mode's default
  * @retval true if a complete frame arrived
  *
  * Reports when the high-speed clock appeared, when the lanes synchronised and
  * when the first frame ended. Those three tell apart "nothing is transmitting",
  * "wrong bitrate" and "wrong lane mapping or no frame delimiters".
  */
bool csi_probe_wait_for_link(const csi_probe_phy_t *phy, uint32_t timeout_ms);

/**
  * @brief  Compare the bitrate profiles around @p phy and keep the quietest.
  * @param  phy        in/out: centre of the search, replaced by the winner
  * @param  neighbours how many profiles to try either side
  * @param  window_ms  observation window per profile
  * @retval true if any profile delivered frames
  *
  * This is the tool for "the link is up but marginal": it needs one source
  * restart per profile, which is a handful rather than the dozens a full sweep
  * costs, and it reports error counts so two working settings can be ranked.
  */
bool csi_probe_refine(csi_probe_phy_t *phy, uint32_t neighbours, uint32_t window_ms);

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
