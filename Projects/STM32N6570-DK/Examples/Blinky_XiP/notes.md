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
| DMA copy  P->P (LLI)     | 4 MB  |   99 ms |  42.5 MB/s |
| DMA copy  P->int (LLI)   | 64 KB | 0.83 ms |  79.1 MB/s |
| DMA copy  int->P (LLI)   | 64 KB | 0.81 ms |  80.8 MB/s |

### DMA P->P lever study (isolated impact of one change per row)

Baseline = chunked DMA, burst 16, split ports (src=AHB, dst=AXI), refresh 400.
Each row toggles exactly one lever; the factor is the delta vs. that baseline.

| Lever                        |    Time | Throughput | vs. base |
| ---------------------------- | ------: | ---------: | -------: |
| 0 · base (chunk burst16)     |  138 ms |  30.4 MB/s |        — |
| A · hardware linked-list     |   99 ms |  42.5 MB/s |   x1.40  |
| B · LLI + refresh 400->700   |   99 ms |  42.5 MB/s |   x1.40  |

- **Lever A (linked-list): +40 %.** A single HW-chained node list removes the
  CPU restart gap between chunks -> the whole 4 MB streams without CPU turnaround.
- **Lever B (refresh 700): no effect.** At burst 16 the refresh CS turnaround is
  not the bottleneck, so relaxing DCR4 refresh does not move the needle.

> **Engine note:** the linked-list path runs on **GPDMA1_Channel12**, not HPDMA1.
> On this part HPDMA1 raises a USE (user-setting) error (`CxSR` USEF) at channel
> *enable* in linked-list mode, in every configuration (any channel, burst,
> security, node location) - the error fires before the first node is fetched.
> GPDMA1 (the engine ST's `DMA_LinkedList` example uses) runs the identical
> descriptor list without error. Channels 12..15 are required for AXI external
> memory (PSRAM) per RM0486 Table 84. Plain single-block chunked copies still
> use HPDMA1_Channel0.

Interpretation:
- CPU word loop is `volatile` (uncached, single-beat) on purpose: every access
  is an isolated memory-mapped XSPI transaction (~1330 CPU cycles / ~2.5 us per
  word). Dominated by command/latency phase and the DCR4 refresh (CS released
  ~every 2 us). This is the worst case, not representative of cached bulk access.
- None of these numbers are limited by raw bus bandwidth. The x16 DDR bus peaks
  at ~800 MB/s; a P->P copy (each byte crosses the bus twice) has a ceiling of
  ~400 MB/s. Both CPU memcpy (4 MB/s) and DMA (30 MB/s) are far below that, so
  they are *transaction-overhead* bound (command/latency phase + refresh
  turnaround), not bandwidth bound.
- The difference is purely how well the fixed per-transaction overhead is
  amortized:
    - CPU memcpy stalls on every memory-mapped access until the XSPI
      transaction completes (no overlap, short accesses) => overhead dominates.
    - DMA (HPDMA1) issues back-to-back 16-beat bursts (64 B) and its FIFO
      decouples read from write => same overhead spread over far more payload
      => ~7.5x faster, over the *same* shared bus.
- The shared octal bus only explains the *relative* penalty of P->P vs P<->int
  (read + write + turnaround on one bus), not the absolute figure: DMA P->P at
  30 MB/s is still nowhere near the ~400 MB/s ceiling.
- Direction matters: with one side in internal SRAM, DMA reaches ~80 MB/s
  because only one side is the slow PSRAM and the bus is not simultaneously
  reading+writing PSRAM.

Takeaways for the VENC pipeline:
- Move bulk data with DMA (bursts), never word-wise CPU/`volatile` access.
- Use a hardware linked-list (GPDMA1) for large streams: +40 % over CPU-restarted
  chunks (42 vs. 30 MB/s) with zero CPU involvement during the transfer.
- Prefer staging through internal SRAM over PSRAM->PSRAM copies (~2x faster,
  ~80 vs. 42 MB/s).
- Reported numbers are effective end-to-end (incl. cache maintenance), well
  below the raw bus peak (~800 MB/s x16 DDR), which is expected for isolated
  block transfers with refresh turnaround.
