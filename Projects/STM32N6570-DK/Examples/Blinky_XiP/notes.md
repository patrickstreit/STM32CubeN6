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

| Transfer          | Block | Time    | Throughput |
| ----------------- | ----- | ------- | ---------- |
| CPU write (word)  | 4 MB  | 2628 ms |  1.6 MB/s  |
| CPU read  (word)  | 4 MB  | 2479 ms |  1.7 MB/s  |
| CPU memcpy P->P   | 4 MB  | 1038 ms |  4.0 MB/s  |
| DMA copy  P->P    | 4 MB  |  138 ms | 30.4 MB/s  |
| CPU memcpy P->int | 64 KB | 3.98 ms | 16.5 MB/s  |
| CPU memcpy int->P | 64 KB | 5.02 ms | 13.1 MB/s  |
| DMA copy  P->int  | 64 KB | 1.35 ms | 48.6 MB/s  |
| DMA copy  int->P  | 64 KB | 1.47 ms | 44.6 MB/s  |

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
- Direction matters: with one side in internal SRAM, DMA reaches 44-48 MB/s
  because only one side is the slow PSRAM and the bus is not simultaneously
  reading+writing PSRAM.

Takeaways for the VENC pipeline:
- Move bulk data with DMA (bursts), never word-wise CPU/`volatile` access.
- Prefer staging through internal SRAM over PSRAM->PSRAM copies (~1.5x faster).
- Reported numbers are effective end-to-end (incl. cache maintenance), well
  below the raw bus peak (~800 MB/s x16 DDR), which is expected for isolated
  block transfers with refresh turnaround.
