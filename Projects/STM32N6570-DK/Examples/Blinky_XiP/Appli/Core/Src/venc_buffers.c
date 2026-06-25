#include "venc_buffers.h"

/* All four buffers are placed in the .psram output section (NOLOAD), which is
 * mapped to the XSPI1 memory-mapped window (APS256XX PSRAM @ 0x90000000).
 * The FSBL initialises XSPI1 in memory-mapped read/write mode before jumping
 * to the application, so these symbols are accessible as plain RAM. */

__attribute__((section(".psram"), used))
uint8_t frame_ping[VENC_FRAME_BUF_SIZE];

__attribute__((section(".psram"), used))
uint8_t frame_pong[VENC_FRAME_BUF_SIZE];

__attribute__((section(".psram"), used))
uint8_t bs_ping[VENC_BITSTREAM_BUF_SIZE];

__attribute__((section(".psram"), used))
uint8_t bs_pong[VENC_BITSTREAM_BUF_SIZE];
