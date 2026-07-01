# Notes
Guide Followed:
https://community.st.com/stm32-mcus-60/how-to-debug-the-stm32n6-using-vs-code-156102

3. Create an STM32N6 CMake project (done)
4. Code editing and post build commands (done)
5. Build the project (done)
6. Create and modify the launch.json file (done)
    - see firmware.code-workspace in SensorPlatform
7. Create and modify tasks.json (done)
    - see firmware.code-workspace in SensorPlatform
8. Start debugging (done)
    - correction of missing xspi ncs override config in MX
    - jump to application --> ok
    - extmem read from debug session works as intended

## PSRAM throughput benchmark (APS256XX @ XSPI1, 200 MHz)

Optional module `Appli/Core/Src/psram_bench.c`, enabled via CMake option
`PSRAM_BENCH=ON` (preset "Debug + Bench"). Streams results over LPUART1.
Measured with CPU @ 533 MHz, XSPI1 kernel @ 200 MHz, DCR4 refresh = 400.

| Transfer                 | Block |    Time | Throughput |
| ------------------------ | ----- | ------: | ---------: |
| CPU write (word)         | 4 MB  | 2695 ms |   1.6 MB/s |
| CPU read  (word)         | 4 MB  | 2504 ms |   1.7 MB/s |
| CPU memcpy P->P          | 4 MB  | 1040 ms |   4.0 MB/s |
| DMA copy  P->P (chunked) | 4 MB  |  138 ms |  30.4 MB/s |
| DMA copy  P->P (LLI)     | 4 MB  |   40 ms | 104.0 MB/s |
| DMA copy  P->int (LLI)   | 64 KB | 0.23 ms | 284.0 MB/s |
| DMA copy  int->P (LLI)   | 64 KB | 0.34 ms | 191.0 MB/s |

### DMA P->P lever study (isolated impact of one change per row)

Baseline = chunked DMA, word beats, burst 16, split ports (src=AHB, dst=AXI),
refresh 400. Each row toggles exactly one lever; the factor is the delta vs. the
baseline.

| Lever                        |    Time | Throughput | vs. base |
| ---------------------------- | ------: | ---------: | -------: |
| 0 · base (chunk word b16)    |  138 ms |  30.4 MB/s |        — |
| A · hardware linked-list     |   40 ms | 104.0 MB/s |   x3.46  |
| A · LLI doubleword (b8)      |   40 ms | 104.0 MB/s |   x3.46  |
| B · LLI + refresh 400->700   |   40 ms | 104.0 MB/s |   x3.46  |

- **Lever A (linked-list): +246 %.** A single HW-chained node list on
  HPDMA1_Channel12 removes the CPU restart gap between chunks -> the whole 4 MB
  streams without CPU turnaround (30 -> 104 MB/s).
- **Doubleword (64-bit beats): no effect.** word/burst16 already fills the 64-byte
  channel FIFO, so wider beats do not help; the limit is the PSRAM transaction
  rate, not the AXI beat width. (64-bit width is forbidden on the AHB port,
  RM0486 Table 91, so the doubleword variant forces both endpoints onto AXI.)
- **Lever B (refresh 700): no effect.** At burst 16 the refresh CS turnaround is
  not the bottleneck, so relaxing DCR4 refresh does not move the needle.

> **Engine note:** the linked-list path runs on **HPDMA1_Channel12** and requires
> **per-channel CID isolation**. HPDMA gates the descriptor fetch (and data
> accesses) through `CCIDCFGR`; without aligning the channel to the CPU's CID the
> RISAF rejects the fetch and the channel raises a USE (user-setting) error at
> *enable* before the first node is fetched. The fix is one call after
> `HAL_DMA_ConfigChannelAttributes`:
> `HAL_DMA_SetIsolationAttributes(&hdma, {DMA_ISOLATION_ON, DMA_CHANNEL_STATIC_CID_1})`
> (see NUCLEO-N657X0-Q/Examples/DMA/DMA_RAMToRAM). No extra RISAF code is needed
> here: the CPU (CID_1) already owns the PSRAM + AXISRAM whitelists. Channels
> 12..15 are required for AXI external memory (PSRAM) per RM0486 Table 84.
> GPDMA1_Channel12 also runs the same list (42 MB/s) without CID setup because it
> has no `CCIDCFGR` gate, but HPDMA1 is ~2.5x faster and is the preferred engine.
> Plain single-block chunked copies still use HPDMA1_Channel0.

Interpretation:
- CPU word loop is `volatile` (uncached, single-beat) on purpose: every access
  is an isolated memory-mapped XSPI transaction (~1330 CPU cycles / ~2.5 us per
  word). Dominated by command/latency phase and the DCR4 refresh (CS released
  ~every 2 us). This is the worst case, not representative of cached bulk access.
- None of these numbers are limited by raw bus bandwidth. The PSRAM is an
  octal **x8 DDR** device (`PHY_LINK_RAM8`); at the 200 MHz XSPI kernel the raw
  peak is ~400 MB/s, and a P->P copy (each byte crosses the bus twice) has a
  ceiling of ~200 MB/s. The CPU memcpy (4 MB/s) and chunked DMA (30 MB/s) are far
  below that, so they are *transaction-overhead* bound (command/latency phase +
  refresh turnaround), not bandwidth bound.
- The difference is purely how well the fixed per-transaction overhead is
  amortized:
    - CPU memcpy stalls on every memory-mapped access until the XSPI
      transaction completes (no overlap, short accesses) => overhead dominates.
    - DMA (HPDMA1_Ch12 LLI) issues back-to-back 16-beat bursts (64 B) and its
      FIFO decouples read from write => same overhead spread over far more
      payload => ~3.5x faster than chunked, over the *same* shared bus.
- The shared octal bus explains the *relative* penalty of P->P vs P<->int
  (read + write + turnaround on one bus): P->P (104 MB/s) is essentially at the
  serial read+write limit 1/(1/284 + 1/191) ~= 114 MB/s of the single interface.
- Direction matters: with one side in internal SRAM, DMA reaches 284 MB/s (read,
  71 % of the ~400 MB/s peak) and 191 MB/s (write) because only one side is the
  slow PSRAM and the bus is not simultaneously reading+writing PSRAM.
- Doubleword (64-bit) DMA beats do NOT help: the XSPI controller serialises any
  AXI beat width into the same x8 DDR pin stream, so the physical transfer is
  unchanged (measured identical to word). The only real bandwidth lever left is a
  higher XSPI kernel clock (if the PSRAM rating + signal integrity allow it).

Takeaways for the VENC pipeline:
- Move bulk data with DMA (bursts), never word-wise CPU/`volatile` access.
- Use a hardware linked-list on HPDMA1_Channel12 (+ CID isolation) for large
  streams: 104 MB/s, ~3.5x over CPU-restarted chunks, zero CPU involvement.
- Prefer staging through internal SRAM over PSRAM->PSRAM copies: a one-directional
  PSRAM read hits 284 MB/s vs. 104 MB/s for a P->P copy on the single octal bus.
- Doubleword DMA beats and DCR4 refresh tuning give no gain here; the DMA path is
  tuned. The remaining bandwidth lever is the XSPI kernel clock, not the DMA.
- Reported numbers are effective end-to-end (incl. cache maintenance), below the
  ~400 MB/s raw x8 DDR peak, which is expected for isolated block transfers with
  per-burst command/latency + refresh turnaround.
