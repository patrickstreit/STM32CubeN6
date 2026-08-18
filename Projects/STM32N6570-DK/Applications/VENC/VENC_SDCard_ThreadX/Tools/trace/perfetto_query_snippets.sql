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
    s.id as id,
    s.ts,
    s.name,
    MAX(CASE WHEN a.key = 'debug.semaphore_ptr' THEN a.int_value END) AS sem_ptr,
    MAX(CASE WHEN a.key = 'debug.semaphore_ptr_name' THEN a.string_value END) AS sem_name,
    MAX(CASE WHEN a.key = 'debug.current_count' THEN a.int_value END) AS sem_count,
    MAX(CASE WHEN a.key = 'debug.wait_option'   THEN a.int_value END) AS wait_option
  FROM slice s
  LEFT JOIN args a ON a.arg_set_id = s.arg_set_id
  WHERE s.name IN ('TX_SEMAPHORE_GET', 'TX_SEMAPHORE_PUT')
  GROUP BY s.id
),
paired AS (
  SELECT
    id,
    sem_ptr,
    sem_name,
    LAG(ts) OVER (PARTITION BY sem_ptr ORDER BY ts) AS prev_ts,
    ts AS cur_ts,
    LAG(name) OVER (PARTITION BY sem_ptr ORDER BY ts) AS prev_name,
    name AS cur_name
  FROM sem
)
SELECT
  id,
  sem_ptr,
  sem_name,
  prev_name,
  cur_name,
  (cur_ts - prev_ts) AS delta_ns,
  (cur_ts - prev_ts) / 1e6 AS delta_ms,
  prev_ts,
  cur_ts
FROM paired
WHERE prev_ts IS NOT NULL
ORDER BY delta_ms DESC;

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
