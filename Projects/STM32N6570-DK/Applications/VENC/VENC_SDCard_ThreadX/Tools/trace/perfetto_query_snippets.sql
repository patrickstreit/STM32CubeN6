-- Perfetto SQL snippets for the traces produced by trace_convert.py.
-- Paste into the "Query (SQL)" page of https://ui.perfetto.dev
--
-- Conventions produced by pftrace/exporter.py:
--   slice.name       'encode', 'fx_file_write', 'running', 'ISR <n>', or the
--                    event name for instants (FRAME_CAPTURED, ...)
--   track.name       'VENC', 'Storage', 'Capture', thread names, counter names
--   args key prefix  'debug.' + the argument name from instrumentation.yaml
--   slice.dur        nanoseconds (-1 for instants)


-- ---------------------------------------------------------------------------
-- 1. Slowest SD writes, with frame and payload size attached
-- ---------------------------------------------------------------------------
SELECT
  s.ts,
  s.dur / 1e6                                                      AS dur_ms,
  MAX(CASE WHEN a.key = 'debug.frame_id'    THEN a.int_value END)  AS frame_id,
  MAX(CASE WHEN a.key = 'debug.bytes'       THEN a.int_value END)  AS bytes,
  MAX(CASE WHEN a.key = 'debug.file_number' THEN a.int_value END)  AS file_number
FROM slice s
LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
WHERE s.name = 'fx_file_write'
GROUP BY s.id
ORDER BY s.dur DESC
LIMIT 50;


-- ---------------------------------------------------------------------------
-- 2. Write-duration distribution in 50 ms buckets (how heavy is the tail?)
-- ---------------------------------------------------------------------------
SELECT
  CAST(dur / 50e6 AS INT) * 50    AS bucket_ms,
  COUNT(*)                        AS n
FROM slice
WHERE name = 'fx_file_write'
GROUP BY bucket_ms
ORDER BY bucket_ms;


-- ---------------------------------------------------------------------------
-- 3. H4: do stalls coincide with file rotation?
-- ---------------------------------------------------------------------------
WITH slow AS (
  SELECT id, ts, dur FROM slice WHERE name = 'fx_file_write' AND dur > 100e6
),
rot AS (
  SELECT ts FROM slice WHERE name = 'FILE_ROTATED'
)
SELECT
  slow.ts,
  slow.dur / 1e6                                             AS dur_ms,
  (SELECT MIN(ABS(slow.ts - rot.ts)) FROM rot) / 1e6         AS ms_to_nearest_rotation
FROM slow
ORDER BY slow.dur DESC;


-- ---------------------------------------------------------------------------
-- 4. Encoder back-pressure: queue depth sampled just before each slow write
-- ---------------------------------------------------------------------------
WITH slow AS (
  SELECT ts, dur FROM slice WHERE name = 'fx_file_write' AND dur > 100e6
)
SELECT
  slow.ts,
  slow.dur / 1e6                  AS dur_ms,
  (SELECT c.value
     FROM counter c
     JOIN counter_track ct ON ct.id = c.track_id
    WHERE ct.name = 'venc_queue_level' AND c.ts <= slow.ts
    ORDER BY c.ts DESC LIMIT 1)   AS queue_level_before,
  (SELECT c.value
     FROM counter c
     JOIN counter_track ct ON ct.id = c.track_id
    WHERE ct.name = 'bitstream_fill' AND c.ts <= slow.ts
    ORDER BY c.ts DESC LIMIT 1)   AS bitstream_fill_before
FROM slow
ORDER BY slow.dur DESC;


-- ---------------------------------------------------------------------------
-- 5. Encodes longer than 100 ms
-- ---------------------------------------------------------------------------
SELECT
  s.ts,
  s.dur / 1e6                                                        AS dur_ms,
  MAX(CASE WHEN a.key = 'debug.frame_id'      THEN a.int_value END)  AS frame_id,
  MAX(CASE WHEN a.key = 'debug.encoded_bytes' THEN a.int_value END)  AS encoded_bytes
FROM slice s
JOIN track t ON t.id = s.track_id
LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
WHERE s.name = 'encode' AND t.name = 'VENC'
GROUP BY s.id
HAVING s.dur > 100e6
ORDER BY s.dur DESC;


-- ---------------------------------------------------------------------------
-- 6. Dropped frames with their running skip count
-- ---------------------------------------------------------------------------
SELECT
  s.ts,
  MAX(CASE WHEN a.key = 'debug.frame_id'   THEN a.int_value END)  AS frame_id,
  MAX(CASE WHEN a.key = 'debug.skip_count' THEN a.int_value END)  AS skip_count
FROM slice s
LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
WHERE s.name = 'FRAME_DROPPED'
GROUP BY s.id
ORDER BY s.ts;


-- ---------------------------------------------------------------------------
-- 7. Capture cadence: worst frame periods
-- ---------------------------------------------------------------------------
WITH cap AS (
  SELECT ts, LAG(ts) OVER (ORDER BY ts) AS prev_ts
  FROM slice WHERE name = 'FRAME_CAPTURED'
)
SELECT ts, (ts - prev_ts) / 1e6 AS period_ms
FROM cap
WHERE prev_ts IS NOT NULL
ORDER BY period_ms DESC
LIMIT 50;


-- ---------------------------------------------------------------------------
-- 8. Where does the SD writer thread spend its time?
-- ---------------------------------------------------------------------------
SELECT
  s.name,
  COUNT(*)          AS n,
  SUM(s.dur) / 1e6  AS total_ms,
  AVG(s.dur) / 1e6  AS avg_ms,
  MAX(s.dur) / 1e6  AS max_ms
FROM slice s
JOIN thread_track tt ON tt.id = s.track_id
JOIN thread th ON th.utid = tt.utid
WHERE th.name LIKE '%SDCard%' AND s.dur > 0
GROUP BY s.name
ORDER BY total_ms DESC;


-- ---------------------------------------------------------------------------
-- 9. Queue occupancy over time (TX_QUEUE_SEND / TX_QUEUE_RECEIVE, enqueued)
-- ---------------------------------------------------------------------------
SELECT
  s.ts,
  s.name,
  MAX(CASE WHEN a.key = 'debug.enqueued' THEN a.int_value END) AS enqueued
FROM slice s
LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
WHERE s.name LIKE 'TX_QUEUE_%'
GROUP BY s.id
ORDER BY s.ts;


-- ---------------------------------------------------------------------------
-- 10. AFTER enabling TX_TRACE_SEMAPHORE_EVENTS (see README section 8, step 1).
--     The sd_tx_semaphore GET -> PUT gap is the hardware/card wait; everything
--     else in the write path is software overhead.
-- ---------------------------------------------------------------------------
SELECT
  s.ts,
  s.name,
  MAX(CASE WHEN a.key = 'debug.semaphore_ptr' THEN a.int_value END) AS sem_ptr,
  MAX(CASE WHEN a.key = 'debug.current_count' THEN a.int_value END) AS sem_count,
  MAX(CASE WHEN a.key = 'debug.wait_option'   THEN a.int_value END) AS wait_option
FROM slice s
LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
WHERE s.name IN ('TX_SEMAPHORE_GET', 'TX_SEMAPHORE_PUT')
GROUP BY s.id
ORDER BY s.ts;


WITH sem AS (
  SELECT
    s.id,
    s.ts,
    s.name,
    MAX(CASE WHEN a.key = 'debug.semaphore_ptr' THEN a.int_value END) AS sem_ptr,
    MAX(CASE WHEN a.key = 'debug.semaphore_ptr_name' THEN a.string_value END) AS sem_name,
    MAX(CASE WHEN a.key = 'debug.current_count' THEN a.int_value END) AS sem_count,
    MAX(CASE WHEN a.key = 'debug.wait_option' THEN a.int_value END) AS wait_option
  FROM slice s
  LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name IN ('TX_SEMAPHORE_GET', 'TX_SEMAPHORE_PUT')
  GROUP BY s.id
),
ordered AS (
  SELECT
    *,
    LAG(ts) OVER (PARTITION BY sem_ptr ORDER BY ts) AS prev_ts,
    LAG(name) OVER (PARTITION BY sem_ptr ORDER BY ts) AS prev_name,
    CASE
      WHEN LOWER(COALESCE(sem_name, '')) LIKE '%rx%' THEN 'RX'
      WHEN LOWER(COALESCE(sem_name, '')) LIKE '%tx%' THEN 'TX'
      ELSE 'OTHER'
    END AS dir
  FROM sem
),
paired AS (
  SELECT
    sem_ptr,
    sem_name,
    dir,
    prev_ts AS get_ts,
    ts AS put_ts,
    (ts - prev_ts) AS delta_ns,
    (ts - prev_ts) / 1e6 AS delta_ms
  FROM ordered
  WHERE prev_name = 'TX_SEMAPHORE_GET'
    AND name = 'TX_SEMAPHORE_PUT'
    AND dir IN ('RX', 'TX')
)
SELECT
  dir,
  sem_name,
  get_ts,
  put_ts,
  delta_ns,
  delta_ms
FROM paired
ORDER BY dir, delta_ms DESC;

-- ---------------------------------------------------------------------------
-- 11. Event mix — which producer dominates the ring (filter tuning)
-- ---------------------------------------------------------------------------
SELECT
  name,
  COUNT(*)                                                  AS n,
  ROUND(100.0 * COUNT(*) / (SELECT COUNT(*) FROM slice), 1) AS pct
FROM slice
GROUP BY name
ORDER BY n DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 12. DMA transfer (H4: SD_WRITE_BLOCKS -> SD_WRITE_CPLT)
-- ---------------------------------------------------------------------------
select s.id, s.ts, s.dur, s.name, a.key, a.display_value
from slice s
join args a on a.arg_set_id = s.arg_set_id
where s.name = "HAL_SD_WriteBlocks_DMA" and a.key = "debug.block_count";


-- ---------------------------------------------------------------------------
-- 13. Histogram of exact block_count values for DMA write slices
--     Use this when each distinct transfer size should be shown separately.
-- ---------------------------------------------------------------------------
WITH block_counts AS (
  SELECT
    s.id,
    a.int_value AS block_count
  FROM slice s
  JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA'
    AND a.key = 'debug.block_count'
)
SELECT
  block_count,
  COUNT(*) AS n,
  ROUND(100.0 * COUNT(*) / SUM(COUNT(*)) OVER (), 2) AS pct
FROM block_counts
GROUP BY block_count
ORDER BY block_count;


-- ---------------------------------------------------------------------------
-- 14. Histogram of block_count in configurable buckets
--     Change bucket_width, e.g. from 8 to 16 or 32 blocks as needed.
-- ---------------------------------------------------------------------------
WITH params AS (
  SELECT 8 AS bucket_width
), block_counts AS (
  SELECT
    a.int_value AS block_count,
    (a.int_value / params.bucket_width) * params.bucket_width AS bucket_start,
    params.bucket_width
  FROM slice s
  JOIN args a ON a.arg_set_id = s.arg_set_id
  CROSS JOIN params
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA'
    AND a.key = 'debug.block_count'
)
SELECT
  bucket_start,
  bucket_start + bucket_width - 1 AS bucket_end,
  COUNT(*) AS n,
  ROUND(100.0 * COUNT(*) / SUM(COUNT(*)) OVER (), 2) AS pct
FROM block_counts
GROUP BY bucket_start, bucket_width
ORDER BY bucket_start;


-- ===========================================================================
-- BASELINE (Plan-Phase 0.1) — vor jeder Aenderung einmal ausfuehren und die
-- Ergebnisse festhalten. Nach Phase 1/2 identisch wiederholen und vergleichen.
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- B1. Kernzahl: Anteil Einzelblock-Transfers nach ANZAHL und nach BYTES.
--     Erwartung heute ~98 % der Transfers, aber nur wenige % der Bytes.
-- ---------------------------------------------------------------------------
WITH dma AS (
  SELECT s.id, s.dur, a.int_value AS blocks
  FROM slice s
  JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA' AND a.key = 'debug.block_count'
)
SELECT
  COUNT(*)                                                          AS transfers,
  SUM(blocks)                                                       AS blocks_total,
  SUM(CASE WHEN blocks = 1 THEN 1 ELSE 0 END)                       AS single_transfers,
  ROUND(100.0 * SUM(CASE WHEN blocks = 1 THEN 1 ELSE 0 END) / COUNT(*), 2)      AS single_pct_by_count,
  ROUND(100.0 * SUM(CASE WHEN blocks = 1 THEN 1 ELSE 0 END) / SUM(blocks), 2)   AS single_pct_by_blocks,
  ROUND(AVG(blocks), 2)                                             AS avg_blocks,
  MAX(blocks)                                                       AS max_blocks,
  ROUND(SUM(dur) / 1e6, 1)                                          AS dma_busy_ms,
  ROUND(100.0 * SUM(CASE WHEN blocks = 1 THEN dur ELSE 0 END) / SUM(dur), 2)    AS single_pct_of_dma_time
FROM dma;


-- ---------------------------------------------------------------------------
-- B2. Fixkosten pro Transfer vs. Kosten pro Block.
--     us_per_block bei blocks=1 gegen blocks>=64 vergleichen: der Quotient ist
--     der Wirkungsgrad, den Phase 1/2 heben soll.
-- ---------------------------------------------------------------------------
WITH dma AS (
  SELECT s.dur, a.int_value AS blocks
  FROM slice s
  JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA' AND a.key = 'debug.block_count'
)
SELECT
  CASE
    WHEN blocks = 1              THEN '  1'
    WHEN blocks <= 8             THEN '  2-8'
    WHEN blocks <= 32            THEN '  9-32'
    WHEN blocks <= 64            THEN ' 33-64'
    WHEN blocks <= 128           THEN ' 65-128'
    ELSE                              '>128'
  END                                   AS blocks_bucket,
  COUNT(*)                              AS n,
  SUM(blocks)                           AS blocks_total,
  ROUND(AVG(dur) / 1e3, 1)              AS avg_us,
  ROUND(MAX(dur) / 1e3, 1)              AS max_us,
  ROUND(AVG(dur) / 1e3 / AVG(blocks), 2) AS us_per_block
FROM dma
GROUP BY blocks_bucket
ORDER BY MIN(blocks);


-- ---------------------------------------------------------------------------
-- B3. Wie viele DMA-Transfers kostet ein Frame?
--     Zeigt das erwartete Muster Kopf-Teilsektor / Mitte / Schwanz-Teilsektor
--     plus FAT-Verkehr. Ziel nach Phase 1/2: deutlich weniger Transfers/Frame.
-- ---------------------------------------------------------------------------
WITH fw AS (
  SELECT
    s.id,
    s.ts,
    s.dur,
    MAX(CASE WHEN a.key = 'debug.frame_id' THEN a.int_value END) AS frame_id,
    MAX(CASE WHEN a.key = 'debug.bytes'    THEN a.int_value END) AS bytes
  FROM slice s
  LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'fx_file_write'
  GROUP BY s.id
),
dma AS (
  SELECT s.ts, s.dur, a.int_value AS blocks
  FROM slice s
  JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA' AND a.key = 'debug.block_count'
)
SELECT
  fw.frame_id,
  fw.bytes,
  ROUND(fw.bytes / 512.0, 1)                                        AS sectors_of_payload,
  COUNT(dma.ts)                                                     AS dma_transfers,
  SUM(CASE WHEN dma.blocks = 1 THEN 1 ELSE 0 END)                   AS single_transfers,
  SUM(dma.blocks)                                                   AS blocks_written,
  ROUND(fw.dur / 1e6, 2)                                            AS fx_write_ms,
  ROUND(SUM(dma.dur) / 1e6, 2)                                      AS dma_busy_ms
FROM fw
LEFT JOIN dma ON dma.ts >= fw.ts AND dma.ts < fw.ts + fw.dur
GROUP BY fw.id
ORDER BY fw.ts;


-- ---------------------------------------------------------------------------
-- B4. Aggregat von B3 — die eine Zeile, die man nach jeder Phase vergleicht.
-- ---------------------------------------------------------------------------
WITH fw AS (
  SELECT s.id, s.ts, s.dur,
         MAX(CASE WHEN a.key = 'debug.bytes' THEN a.int_value END) AS bytes
  FROM slice s
  LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'fx_file_write'
  GROUP BY s.id
),
dma AS (
  SELECT s.ts, a.int_value AS blocks
  FROM slice s
  JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA' AND a.key = 'debug.block_count'
),
per_frame AS (
  SELECT
    fw.id,
    fw.bytes,
    fw.dur,
    COUNT(dma.ts)                                   AS transfers,
    SUM(CASE WHEN dma.blocks = 1 THEN 1 ELSE 0 END) AS singles,
    SUM(dma.blocks)                                 AS blocks
  FROM fw
  LEFT JOIN dma ON dma.ts >= fw.ts AND dma.ts < fw.ts + fw.dur
  GROUP BY fw.id
)
SELECT
  COUNT(*)                            AS frames,
  ROUND(AVG(bytes))                   AS avg_frame_bytes,
  ROUND(AVG(transfers), 2)            AS avg_transfers_per_frame,
  ROUND(AVG(singles), 2)              AS avg_single_transfers_per_frame,
  ROUND(AVG(blocks), 2)               AS avg_blocks_per_frame,
  ROUND(AVG(dur) / 1e6, 2)            AS avg_fx_write_ms,
  ROUND(MAX(dur) / 1e6, 2)            AS max_fx_write_ms
FROM per_frame;


-- ---------------------------------------------------------------------------
-- B5. Blockadressen-Muster: sind die Einzelblocks Nutzdaten (nahe am letzten
--     Multiblock-Transfer) oder FAT/Directory (weit entfernt, wiederkehrend)?
--     Grosse Abstaende in delta_blocks == Metadaten-Verkehr.
-- ---------------------------------------------------------------------------
WITH dma AS (
  SELECT
    s.ts,
    MAX(CASE WHEN a.key = 'debug.start_block' THEN a.int_value END) AS start_block,
    MAX(CASE WHEN a.key = 'debug.block_count' THEN a.int_value END) AS blocks
  FROM slice s
  LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA'
  GROUP BY s.id
),
seq AS (
  SELECT
    ts,
    start_block,
    blocks,
    LAG(start_block) OVER (ORDER BY ts) AS prev_start,
    LAG(blocks)      OVER (ORDER BY ts) AS prev_blocks
  FROM dma
)
SELECT
  ts,
  start_block,
  blocks,
  start_block - (prev_start + prev_blocks) AS gap_blocks_to_prev_end
FROM seq
WHERE prev_start IS NOT NULL
ORDER BY ts
LIMIT 200;


-- ---------------------------------------------------------------------------
-- B6. ACHTUNG: nicht aussagekraeftig. buffer_addr ist die Adresse, die an die
--     HAL geht — im Fallback-Pfad also der treiberinterne 'scratch', der immer
--     ausgerichtet ist. align_mod4 ist daher immer 0. Nimm B7.
-- ---------------------------------------------------------------------------
WITH dma AS (
  SELECT
    MAX(CASE WHEN a.key = 'debug.block_count' THEN a.int_value END) AS blocks,
    MAX(CASE WHEN a.key = 'debug.buffer_addr' THEN a.int_value END) AS buffer_addr
  FROM slice s
  LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA'
  GROUP BY s.id
)
SELECT
  buffer_addr % 4                                            AS align_mod4,
  COUNT(*)                                                   AS transfers,
  SUM(CASE WHEN blocks = 1 THEN 1 ELSE 0 END)                AS single_transfers,
  SUM(CASE WHEN blocks > 1 THEN 1 ELSE 0 END)                AS multi_transfers,
  ROUND(AVG(blocks), 2)                                      AS avg_blocks,
  MAX(blocks)                                                AS max_blocks
FROM dma
GROUP BY align_mod4
ORDER BY align_mod4;


-- ---------------------------------------------------------------------------
-- B7. Der eigentliche Beweis (Plan-Phase 0.2): Transfers nach Quelladresse.
--     Drei Gruppen, aufloesbar ueber die .map-Datei:
--       'scratch' (fx_stm32_sd_driver.c)  -> Per-Sektor-Fallback bei einem
--                                            nicht wortausgerichteten
--                                            FileX-Puffer
--       'fx_sd_media_memory' (app_filex.c) -> Teilsektor-RMW und FAT ueber den
--                                            Sektor-Cache
--       verstreute Adressen                -> Direktschreiben aus dem
--                                            Encoder-Puffer, die einzigen
--                                            echten Multiblock-Transfers
-- ---------------------------------------------------------------------------
WITH dma AS (
  SELECT
    s.ts,
    s.dur,
    MAX(CASE WHEN a.key = 'debug.block_count' THEN a.int_value END) AS blocks,
    MAX(CASE WHEN a.key = 'debug.buffer_addr' THEN a.int_value END) AS buffer_addr
  FROM slice s
  LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name = 'HAL_SD_WriteBlocks_DMA'
  GROUP BY s.id
)
SELECT
  printf('0x%08X', buffer_addr) AS buffer_hex,
  buffer_addr % 4               AS align_mod4,
  buffer_addr % 32              AS align_mod32,
  COUNT(*)                      AS transfers,
  ROUND(AVG(blocks), 2)         AS avg_blocks,
  ROUND(AVG(dur) / 1e3, 1)      AS avg_us
FROM dma
GROUP BY buffer_addr
ORDER BY transfers DESC
LIMIT 40;