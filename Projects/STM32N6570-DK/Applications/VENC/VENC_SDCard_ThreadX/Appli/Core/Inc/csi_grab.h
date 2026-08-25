/**
  ******************************************************************************
  * @file    csi_grab.h
  * @brief   Dump the raw payload of one CSI-2 data type to memory and print it.
  *
  * The counters in csi_probe answer how much arrives and in what shape. They
  * cannot answer what is in it. This does: PIPE0 of the DCMIPP is the dump pipe -
  * no ISP, no pixel packing, the received bytes as they are - so pointing it at
  * one data type and reading the first bytes back says what those packets carry.
  *
  * It also gives a second, independent data type filter. The CSI virtual channel
  * filter and the pipe's own DTIDA comparison are separate pieces of hardware;
  * when the first one cannot tell two data types apart, the second one still may.
  ******************************************************************************
  */

#ifndef CSI_GRAB_H
#define CSI_GRAB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Capture one frame of @p dt on @p vc and hexdump the start of it.
  * @param  vc          virtual channel to capture from
  * @param  dt          data type the pipe accepts; everything else is dropped
  * @param  show_bytes  how many bytes to print, capped at the buffer size
  *
  * The dump is bounded by the hardware, not by trust: the pipe's dump limit is
  * set to the buffer size, so an unknown source cannot write past it whatever it
  * sends. Safe to call with a data type that turns out not to exist - then the
  * counter reads zero and the buffer stays as it was.
  */
void csi_grab(uint32_t vc, uint32_t dt, uint32_t show_bytes);

#ifdef __cplusplus
}
#endif

#endif /* CSI_GRAB_H */
