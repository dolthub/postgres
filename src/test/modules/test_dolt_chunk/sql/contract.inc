-- Contract tests: behavior every chunk store implementation must provide.
-- Included once per implementation by test_dolt_chunk.sql, which defines
-- error_of(): expected errors are returned as text, since psql would prefix
-- them with this file's absolute path.

-- Puts are idempotent, and reads return the bytes put under a hash.
SELECT test_chunk_put(chunk_hash('contract alpha'), 'contract alpha') AS first_put;
SELECT test_chunk_put(chunk_hash('contract alpha'), 'contract alpha') AS second_put;
SELECT test_chunk_get(chunk_hash('contract alpha')) = 'contract alpha' AS roundtrip;
SELECT test_chunk_get(chunk_hash('contract missing')) IS NULL AS missing_get;
SELECT test_chunk_exists(chunk_hash('contract alpha')) AS present,
       test_chunk_exists(chunk_hash('contract missing')) AS absent;
SELECT error_of($$SELECT test_chunk_get('\x00')$$);

-- Batches: a put may contain duplicates of each other and of stored chunks.
SELECT test_chunk_put_many(
         ARRAY[chunk_hash('contract beta'), chunk_hash('contract gamma'), chunk_hash('contract beta'), chunk_hash('contract alpha')],
         ARRAY['contract beta', 'contract gamma', 'contract beta', 'contract alpha']::bytea[]) AS stored;
SELECT test_chunk_get_many(ARRAY[chunk_hash('contract gamma'), chunk_hash('contract missing'), chunk_hash('contract beta')])
       = ARRAY['contract gamma', NULL, 'contract beta']::bytea[] AS batch_get;

-- Large chunks round-trip whole; the store reports a limit well above them.
SELECT test_chunk_max_size() >= 1048576 AS large_limit;
SELECT test_chunk_put(chunk_hash(x), x) AS large_put,
       test_chunk_get(chunk_hash(x)) = x AS large_roundtrip,
       test_chunk_exists(chunk_hash(x)) AS large_exists
  FROM (SELECT decode(repeat(md5(g::text), 6250), 'hex') AS x FROM generate_series(1, 1) g) s;

-- Deleting removes a chunk from existence checks; deleting again finds nothing.
SELECT test_chunk_put(chunk_hash('contract delta'), 'contract delta') AS put_delta;
SELECT test_chunk_delete(chunk_hash('contract delta')) AS deleted;
SELECT test_chunk_exists(chunk_hash('contract delta')) AS exists_after_delete;
SELECT test_chunk_delete(chunk_hash('contract delta')) AS deleted_again;

-- Scans see stored chunks; statistics report something.
SELECT test_chunk_scan_count() >= 3 AS scanned;
SELECT chunks >= 0 AND bytes > 0 AS stats FROM test_chunk_store_stats();

-- Verification rejects content that does not match its hash.
SET dolt_chunk_verify_hashes = on;
SELECT test_chunk_put(chunk_hash('contract epsilon'), 'contract epsilon') AS verified_put;
SELECT error_of($$SELECT test_chunk_put(chunk_hash('contract zeta'), 'something else')$$);
RESET dolt_chunk_verify_hashes;

-- Pins keep a chunk's bytes valid while other chunks are put.
SELECT test_chunk_pin_survives_puts(chunk_hash('contract alpha'), 40) AS pin_survives;
