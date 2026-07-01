#include "psram_bench.h"

#if defined(PSRAM_BENCH_ENABLE)

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * PSRAM throughput benchmark (APS256XX @ XSPI1, memory-mapped @ 0x90000000).
 *
 * Besides raw CPU vs DMA figures this build runs a "lever study" that isolates
 * the impact of each optimization knob on the heavy 4 MB PSRAM->PSRAM copy, so
 * the effect of every lever is visible on the UART (with a factor vs baseline):
 *
 *   1. DMA chunked, burst 16, refresh 400   (baseline)
 *   2. DMA chunked, burst 64, refresh 400   (+ longer bursts)
 *   3. DMA LLI,     burst 64, refresh 400   (+ hardware linked-list, no CPU gap)
 *   4. DMA LLI,     burst 64, refresh 700   (+ fewer refresh CS turnarounds)
 *
 * DCR4 (refresh) is changed at runtime between runs and restored afterwards.
 * 700 cycles @ 200 MHz kernel = 3.5 us, still below the ~4 us tCEM limit.
 * ---------------------------------------------------------------------------*/

#define BENCH_PSRAM_SIZE   (4U * 1024U * 1024U)   /* 4 MB per PSRAM buffer      */
#define BENCH_INT_SIZE     (64U * 1024U)          /* 64 KB internal SRAM block  */
#define BENCH_DMA_CHUNK    (0xFF00U)              /* 65280 B, <=65535 & 256-aln */
#define BENCH_MAX_NODES    (66U)                  /* 4 MB / 65280 + margin      */
#define BENCH_MB_DIV       (1000000ULL)           /* report decimal MB/s        */

#define BENCH_REFRESH_BASE (400U)                 /* ~2.0 us CS release          */
#define BENCH_REFRESH_TUNE (700U)                 /* ~3.5 us, still < tCEM       */

#define BENCH_LLI_DEBUG    0                       /* 1 = dump LLI regs (pollutes timing) */

/* ---------------------------------------------------------------------------
 * HPDMA1 addressing / security (STM32N6, secure build):
 *   This Appli is compiled with -mcmse, so CPU_IN_SECURE_STATE is defined and
 *   HPDMA1_Channel0 is the SECURE instance. The DMA must therefore issue SECURE
 *   transactions for secure resources: AXISRAM is a secure internal RAM and is
 *   addressed through its native secure alias 0x34000000 with SSEC/DSEC=1 and a
 *   secure channel (like ST's DMA_LinkedList example). PSRAM (0x90000000) is an
 *   external memory that is reached non-secure (as the chunked path proves), so
 *   its endpoints keep SSEC/DSEC=0.
 *   Bus reachability: the HPDMA AHB port (port 1) cannot reach AXISRAM; only the
 *   AXI port (port 0) can, so any AXISRAM endpoint is routed to port 0.
 * ------------------------------------------------------------------------- */
#define AXISRAM_S_BASE     (0x34000000UL)
#define AXISRAM_SPAN       (0x00400000UL)         /* 4 MB window is plenty       */
#define IS_AXISRAM(p)      (((uint32_t)(p) >= AXISRAM_S_BASE) && \
                            ((uint32_t)(p) <  AXISRAM_S_BASE + AXISRAM_SPAN))

/* Two large scratch buffers in PSRAM (.psram = XSPI1 memory-mapped window). */
__attribute__((section(".psram"), used)) static uint8_t bench_a[BENCH_PSRAM_SIZE];
__attribute__((section(".psram"), used)) static uint8_t bench_b[BENCH_PSRAM_SIZE];

/* Internal AXISRAM block for PSRAM<->internal copy tests. */
static uint8_t bench_int[BENCH_INT_SIZE];

/* Linked-list node storage in internal (secure) AXISRAM. The HPDMA engine
 * fetches these descriptors itself over its AXI port with a secure channel. */
static DMA_NodeTypeDef  bench_nodes[BENCH_MAX_NODES];
static DMA_QListTypeDef bench_qlist;

static UART_HandleTypeDef *s_uart;
static uint32_t            s_cpu_hz;

/* --------------------------------------------------------------------------- */
/* UART / reporting helpers                                                     */
/* --------------------------------------------------------------------------- */

static void bench_puts(const char *s)
{
  HAL_UART_Transmit(s_uart, (uint8_t *)s, (uint16_t)strlen(s), 1000U);
}

/* Prints one result line and returns the computed MB/s. If base_mbps != 0 a
 * "xN.NN" factor relative to that baseline is appended (impact of the lever). */
static uint32_t bench_report(const char *name, uint32_t bytes, uint32_t cycles,
                             uint32_t base_mbps)
{
  char line[96];

  if (cycles == 0U)
  {
    snprintf(line, sizeof(line), "%-26s   n/a\r\n", name);
    bench_puts(line);
    return 0U;
  }

  uint32_t us   = (uint32_t)(((uint64_t)cycles * 1000000ULL) / s_cpu_hz);
  uint32_t mbps = (uint32_t)(((uint64_t)bytes * s_cpu_hz) / ((uint64_t)cycles * BENCH_MB_DIV));

  if (base_mbps != 0U)
  {
    uint32_t ratio = (mbps * 100U) / base_mbps;   /* x100 fixed point */
    snprintf(line, sizeof(line), "%-26s %8lu us  %6lu MB/s  x%lu.%02lu\r\n",
             name, (unsigned long)us, (unsigned long)mbps,
             (unsigned long)(ratio / 100U), (unsigned long)(ratio % 100U));
  }
  else
  {
    snprintf(line, sizeof(line), "%-26s %8lu us  %6lu MB/s\r\n",
             name, (unsigned long)us, (unsigned long)mbps);
  }
  bench_puts(line);
  return mbps;
}

/* --------------------------------------------------------------------------- */
/* DWT cycle counter                                                            */
/* --------------------------------------------------------------------------- */

static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT       = 0U;
  DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline void dwt_reset(void)
{
  __DSB();
  DWT->CYCCNT = 0U;
  __DSB();
}

static inline uint32_t dwt_read(void)
{
  __DSB();
  return DWT->CYCCNT;
}

/* --------------------------------------------------------------------------- */
/* CPU access patterns                                                          */
/* --------------------------------------------------------------------------- */

/* Word write loop (write-back cache => mostly cache speed, then flushed). */
static uint32_t cpu_write_words(void *dst, uint32_t bytes)
{
  volatile uint32_t *p = (volatile uint32_t *)dst;
  uint32_t n = bytes / 4U;

  SCB_CleanInvalidateDCache();
  dwt_reset();
  for (uint32_t i = 0U; i < n; i++)
  {
    p[i] = i;
  }
  __DSB();
  uint32_t c = dwt_read();

  /* Push the written data out to PSRAM for the subsequent read/DMA tests. */
  SCB_CleanDCache_by_Addr((uint32_t *)dst, (int32_t)bytes);
  return c;
}

/* Word read loop with prior invalidate => real PSRAM line-fill bandwidth. */
static uint32_t cpu_read_words(const void *src, uint32_t bytes, uint32_t *sum_out)
{
  const volatile uint32_t *p = (const volatile uint32_t *)src;
  uint32_t n = bytes / 4U;
  uint32_t sum = 0U;

  SCB_InvalidateDCache_by_Addr((uint32_t *)src, (int32_t)bytes);
  dwt_reset();
  for (uint32_t i = 0U; i < n; i++)
  {
    sum += p[i];
  }
  __DSB();
  uint32_t c = dwt_read();

  *sum_out = sum;
  return c;
}

static uint32_t cpu_memcpy(void *dst, const void *src, uint32_t bytes)
{
  SCB_CleanInvalidateDCache();
  dwt_reset();
  memcpy(dst, src, bytes);
  __DSB();
  uint32_t c = dwt_read();
  SCB_CleanDCache_by_Addr((uint32_t *)dst, (int32_t)bytes);
  return c;
}

/* Fast source preparation: cached writes + a single clean to PSRAM. */
static void psram_prep_src(void *dst, uint32_t bytes)
{
  uint32_t *p = (uint32_t *)dst;
  uint32_t n = bytes / 4U;
  for (uint32_t i = 0U; i < n; i++)
  {
    p[i] = 0xA5A50000U + i;
  }
  SCB_CleanDCache_by_Addr((uint32_t *)dst, (int32_t)bytes);
}

/* --------------------------------------------------------------------------- */
/* XSPI1 refresh (DCR4) runtime tuning                                          */
/* --------------------------------------------------------------------------- */

static void set_refresh(uint32_t cycles)
{
  XSPI1->DCR4 = cycles;
  __DSB();
  (void)XSPI1->DCR4;   /* read back to ensure the write took effect */
}

/* --------------------------------------------------------------------------- */
/* Common DMA Init fill (single-block and linked-list share the same config)    */
/* --------------------------------------------------------------------------- */

static void dma_fill_init(DMA_InitTypeDef *init, uint32_t burst)
{
  init->Request             = DMA_REQUEST_SW;
  init->BlkHWRequest        = DMA_BREQ_SINGLE_BURST;
  init->Direction           = DMA_MEMORY_TO_MEMORY;
  init->SrcInc              = DMA_SINC_INCREMENTED;
  init->DestInc             = DMA_DINC_INCREMENTED;
  init->SrcDataWidth        = DMA_SRC_DATAWIDTH_WORD;
  init->DestDataWidth       = DMA_DEST_DATAWIDTH_WORD;
  init->Priority            = DMA_HIGH_PRIORITY;
  init->SrcBurstLength      = burst;
  init->DestBurstLength     = burst;
  /* Port map (RM0486 CxTR1 SAP/DAP): port 0 = AXI, port 1 = AHB.
   * The AXI port (0) reaches AXISRAM and the external XSPI/PSRAM window; the
   * AHB port (1) reaches PSRAM (via the AHB2AXI bridge) but NOT AXISRAM.
   * For the PSRAM<->PSRAM baseline we split src=AXI / dst=AHB so the read and
   * write phases overlap on the two master ports. Both ports cap AXI/AHB
   * bursts at 16 beats, so burst > 16 brings no benefit here. */
  init->TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
  init->TransferEventMode   = DMA_TCEM_BLOCK_TRANSFER;
  init->Mode                = DMA_NORMAL;
}

/* --------------------------------------------------------------------------- */
/* DMA variant 1/2: chunked single-block transfers (BNDT is only 16-bit)        */
/* --------------------------------------------------------------------------- */

static uint32_t dma_copy_chunked(void *dst, const void *src, uint32_t bytes,
                                 uint32_t burst, uint32_t *ok)
{
  DMA_HandleTypeDef hdma;

  __HAL_RCC_HPDMA1_CLK_ENABLE();

  memset(&hdma, 0, sizeof(hdma));
  hdma.Instance = HPDMA1_Channel0;
  dma_fill_init(&hdma.Init, burst);

  if (HAL_DMA_Init(&hdma) != HAL_OK)
  {
    *ok = 0U;
    return 0U;
  }

  uint32_t remaining = bytes;
  uint32_t s_addr    = (uint32_t)src;
  uint32_t d_addr    = (uint32_t)dst;

  dwt_reset();
  while (remaining > 0U)
  {
    uint32_t chunk = (remaining > BENCH_DMA_CHUNK) ? BENCH_DMA_CHUNK : remaining;

    if (HAL_DMA_Start(&hdma, s_addr, d_addr, chunk) != HAL_OK ||
        HAL_DMA_PollForTransfer(&hdma, HAL_DMA_FULL_TRANSFER, 1000U) != HAL_OK)
    {
      *ok = 0U;
      HAL_DMA_DeInit(&hdma);
      return 0U;
    }

    s_addr    += chunk;
    d_addr    += chunk;
    remaining -= chunk;
  }
  uint32_t c = dwt_read();

  HAL_DMA_DeInit(&hdma);
  *ok = 1U;
  return c;
}

/* --------------------------------------------------------------------------- */
/* DMA variant 3: hardware linked-list (one chained transfer, no CPU restart)   */
/* --------------------------------------------------------------------------- */

static uint32_t dma_copy_lli(void *dst, const void *src, uint32_t bytes,
                             uint32_t burst, uint32_t *ok)
{
  DMA_HandleTypeDef   hdma;
  DMA_NodeConfTypeDef ncfg;

  __HAL_RCC_GPDMA1_CLK_ENABLE();   /* DIAG: use GPDMA1 (ST's proven LLI engine) */

  /* Build the queue of linear nodes covering the whole transfer. */
  memset(&bench_qlist, 0, sizeof(bench_qlist));
  memset(&ncfg, 0, sizeof(ncfg));
  ncfg.NodeType = DMA_GPDMA_LINEAR_NODE;
  dma_fill_init(&ncfg.Init, burst);

  /* AXISRAM is only reachable through the AXI port (port 0). Keep the source
   * on AXI and move the destination to AXI whenever it targets AXISRAM,
   * otherwise keep it on AHB (port 1) to overlap read/write for PSRAM->PSRAM. */
  {
    uint32_t dport = IS_AXISRAM(dst) ? DMA_DEST_ALLOCATED_PORT0
                                     : DMA_DEST_ALLOCATED_PORT1;
    ncfg.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | dport;
  }

  /* Per-endpoint security: secure for AXISRAM, non-secure for external PSRAM.
   * BuildNode writes SSEC/DSEC into each node's CTR1 from these fields. */
  ncfg.SrcSecure  = IS_AXISRAM(src) ? DMA_CHANNEL_SRC_SEC  : DMA_CHANNEL_SRC_NSEC;
  ncfg.DestSecure = IS_AXISRAM(dst) ? DMA_CHANNEL_DEST_SEC : DMA_CHANNEL_DEST_NSEC;

  uint32_t remaining = bytes;
  uint32_t s_addr    = (uint32_t)src;
  uint32_t d_addr    = (uint32_t)dst;
  uint32_t idx       = 0U;

  while (remaining > 0U && idx < BENCH_MAX_NODES)
  {
    uint32_t chunk = (remaining > BENCH_DMA_CHUNK) ? BENCH_DMA_CHUNK : remaining;

    ncfg.SrcAddress = s_addr;
    ncfg.DstAddress = d_addr;
    ncfg.DataSize   = chunk;

    DMA_NodeTypeDef *node = &bench_nodes[idx];

    if (HAL_DMAEx_List_BuildNode(&ncfg, node) != HAL_OK)
    {
      bench_puts("   [lli fail: BuildNode]\r\n");
      *ok = 0U;
      return 0U;
    }
    if (HAL_DMAEx_List_InsertNode_Tail(&bench_qlist, node) != HAL_OK)
    {
      bench_puts("   [lli fail: InsertNode]\r\n");
      *ok = 0U;
      return 0U;
    }

    s_addr    += chunk;
    d_addr    += chunk;
    remaining -= chunk;
    idx++;
  }

  if (remaining != 0U)   /* ran out of nodes */
  {
    bench_puts("   [lli fail: node budget]\r\n");
    *ok = 0U;
    return 0U;
  }

  memset(&hdma, 0, sizeof(hdma));
  /* GPDMA1_Channel12 drives the linked list. HPDMA1 raises a USE (user-setting)
   * error at channel enable in linked-list mode on this part, while GPDMA1 (the
   * engine used by ST's DMA_LinkedList example) works. Channels 12..15 carry the
   * 64-byte FIFO + 2D engine required to reach AXI external memory (PSRAM),
   * RM0486 Table 84; channels 0..11 cannot address external AXI memory. */
  hdma.Instance                         = GPDMA1_Channel12;
  hdma.InitLinkedList.Priority          = DMA_HIGH_PRIORITY;
  hdma.InitLinkedList.LinkStepMode      = DMA_LSM_FULL_EXECUTION;
  hdma.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0;
  hdma.InitLinkedList.TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER;
  hdma.InitLinkedList.LinkedListMode    = DMA_LINKEDLIST_NORMAL;

  if (HAL_DMAEx_List_Init(&hdma) != HAL_OK)
  {
    bench_puts("   [lli fail: List_Init]\r\n");
    *ok = 0U;
    return 0U;
  }

  /* Secure + privileged channel so the engine may fetch the descriptors from
   * secure AXISRAM (matches ST's DMA_LinkedList reference example). */
  if (HAL_DMA_ConfigChannelAttributes(&hdma,
        DMA_CHANNEL_SEC | DMA_CHANNEL_PRIV |
        DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC) != HAL_OK)
  {
    bench_puts("   [lli fail: ChAttr]\r\n");
    *ok = 0U;
    return 0U;
  }

  if (HAL_DMAEx_List_LinkQ(&hdma, &bench_qlist) != HAL_OK)
  {
    bench_puts("   [lli fail: LinkQ]\r\n");
    *ok = 0U;
    return 0U;
  }

  /* Flush the freshly built/linked descriptors from cache to RAM AFTER all
   * node + queue modifications, immediately before the engine fetches them. */
  SCB_CleanDCache();

  dwt_reset();
  if (HAL_DMAEx_List_Start(&hdma) != HAL_OK)
  {
    bench_puts("   [lli fail: Start]\r\n");
    *ok = 0U;
    HAL_DMAEx_List_UnLinkQ(&hdma);
    HAL_DMA_DeInit(&hdma);
    return 0U;
  }

#if BENCH_LLI_DEBUG
  {
    /* Registers are loaded from the head node at channel enable -> dump them
     * together with the head node descriptor to diagnose a USE setting error. */
    DMA_Channel_TypeDef *ch = hdma.Instance;
    char d[128];
    snprintf(d, sizeof(d),
             "Inst=%08lx CCR=%08lx CTR1=%08lx CTR2=%08lx BR1=%08lx\r\n",
             (unsigned long)ch,
             (unsigned long)ch->CCR, (unsigned long)ch->CTR1,
             (unsigned long)ch->CTR2, (unsigned long)ch->CBR1);
    bench_puts(d);
    snprintf(d, sizeof(d),
             "   SAR=%08lx DAR=%08lx LLR=%08lx CLBAR=%08lx SR=%08lx\r\n",
             (unsigned long)ch->CSAR, (unsigned long)ch->CDAR,
             (unsigned long)ch->CLLR, (unsigned long)ch->CLBAR,
             (unsigned long)ch->CSR);
    bench_puts(d);
    snprintf(d, sizeof(d),
             "   &node0=%08lx Head=%08lx\r\n",
             (unsigned long)&bench_nodes[0],
             (unsigned long)bench_qlist.Head);
    bench_puts(d);
    DMA_NodeTypeDef *n0 = &bench_nodes[0];
    snprintf(d, sizeof(d),
             "   node0: T1=%08lx T2=%08lx B1=%08lx SA=%08lx DA=%08lx LL=%08lx\r\n",
             (unsigned long)n0->LinkRegisters[0],
             (unsigned long)n0->LinkRegisters[1],
             (unsigned long)n0->LinkRegisters[2],
             (unsigned long)n0->LinkRegisters[3],
             (unsigned long)n0->LinkRegisters[4],
             (unsigned long)n0->LinkRegisters[5]);
    bench_puts(d);
  }
#endif

  if (HAL_DMA_PollForTransfer(&hdma, HAL_DMA_FULL_TRANSFER, 2000U) != HAL_OK)
  {
    char m[48];
    snprintf(m, sizeof(m), "   [lli fail: Poll ec=0x%08lx]\r\n",
             (unsigned long)hdma.ErrorCode);
    bench_puts(m);
    *ok = 0U;
    HAL_DMAEx_List_UnLinkQ(&hdma);
    HAL_DMA_DeInit(&hdma);
    return 0U;
  }
  uint32_t c = dwt_read();

  HAL_DMAEx_List_UnLinkQ(&hdma);
  HAL_DMA_DeInit(&hdma);
  *ok = 1U;
  return c;
}

/* --------------------------------------------------------------------------- */
/* Public entry point                                                           */
/* --------------------------------------------------------------------------- */

void PSRAM_Bench_Run(UART_HandleTypeDef *huart)
{
  s_uart   = huart;
  s_cpu_hz = SystemCoreClock;

  dwt_init();

  uint32_t refresh0 = XSPI1->DCR4;

  char hdr[112];
  snprintf(hdr, sizeof(hdr),
           "\r\n=== PSRAM benchmark v1 (CPU %lu MHz, %lu MB blocks, refresh=%lu) ===\r\n",
           (unsigned long)(s_cpu_hz / 1000000U),
           (unsigned long)(BENCH_PSRAM_SIZE / (1024U * 1024U)),
           (unsigned long)refresh0);
  bench_puts(hdr);

  uint32_t cyc;
  uint32_t sum;
  uint32_t ok;
  uint32_t base;

  /* ---- CPU reference, full 4 MB PSRAM buffer ---- */
  cyc = cpu_write_words(bench_a, BENCH_PSRAM_SIZE);
  (void)bench_report("CPU write  (word)", BENCH_PSRAM_SIZE, cyc, 0U);

  cyc = cpu_read_words(bench_a, BENCH_PSRAM_SIZE, &sum);
  (void)bench_report("CPU read   (word)", BENCH_PSRAM_SIZE, cyc, 0U);

  cyc = cpu_memcpy(bench_b, bench_a, BENCH_PSRAM_SIZE);
  (void)bench_report("CPU memcpy P->P", BENCH_PSRAM_SIZE, cyc, 0U);

  /* Fill the source once so every DMA run copies real data. */
  psram_prep_src(bench_a, BENCH_PSRAM_SIZE);

  /* ---- DMA P->P lever study (each row toggles exactly one lever) ----
   * Common fast baseline = chunked, burst 16, split ports (src=AHB, dst=AXI),
   * refresh 400. Every following row changes ONE thing so its delta vs the
   * baseline (the "xN.NN" factor) is the isolated impact of that lever. */
  bench_puts("-- DMA P->P lever study (4 MB, base=chunk burst16 split rfr400) --\r\n");

  set_refresh(BENCH_REFRESH_BASE);
  cyc  = dma_copy_chunked(bench_b, bench_a, BENCH_PSRAM_SIZE, 16U, &ok);
  base = bench_report("0 base chunk16 rfr400", BENCH_PSRAM_SIZE, ok ? cyc : 0U, 0U);

  /* Lever A1: hardware linked-list instead of CPU-restarted chunks. */
  cyc = dma_copy_lli(bench_b, bench_a, BENCH_PSRAM_SIZE, 16U, &ok);
  (void)bench_report("A LLI  (vs base)", BENCH_PSRAM_SIZE, ok ? cyc : 0U, base);

  /* Lever A2: longer bursts (64). Needs single AXI port -> loses the split. */
  cyc = dma_copy_chunked(bench_b, bench_a, BENCH_PSRAM_SIZE, 64U, &ok);
  (void)bench_report("A burst64 (vs base)", BENCH_PSRAM_SIZE, ok ? cyc : 0U, base);

  /* Lever B: refresh 400 -> 700 (fewer CS turnarounds), on the LLI variant. */
  set_refresh(BENCH_REFRESH_TUNE);
  cyc = dma_copy_lli(bench_b, bench_a, BENCH_PSRAM_SIZE, 16U, &ok);
  (void)bench_report("B LLI+rfr700 (vs base)", BENCH_PSRAM_SIZE, ok ? cyc : 0U, base);

  set_refresh(refresh0);   /* restore the safe FSBL value */

  /* ---- Same levers on PSRAM <-> internal SRAM (pipeline relevant) ---- */
  bench_puts("-- LLI burst16 P<->int (64 KB) --\r\n");

  set_refresh(BENCH_REFRESH_TUNE);
  cyc = dma_copy_lli(bench_int, bench_a, BENCH_INT_SIZE, 16U, &ok);
  (void)bench_report("DMA P->int", BENCH_INT_SIZE, ok ? cyc : 0U, 0U);

  SCB_CleanDCache_by_Addr((uint32_t *)bench_int, (int32_t)BENCH_INT_SIZE);
  cyc = dma_copy_lli(bench_a, bench_int, BENCH_INT_SIZE, 16U, &ok);
  (void)bench_report("DMA int->P", BENCH_INT_SIZE, ok ? cyc : 0U, 0U);

  set_refresh(refresh0);

  bench_puts("=== PSRAM benchmark done ===\r\n");
}

#endif /* PSRAM_BENCH_ENABLE */
