CREATE EXTENSION test_dolt_chunk;

--
-- Interface contract, against the built-in journal and then a test-only,
-- session-local in-memory store.
--
CREATE FUNCTION error_of(query text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE query;
  RETURN NULL;
EXCEPTION WHEN OTHERS THEN
  RETURN SQLERRM;
END
$$;
\getenv abs_srcdir PG_ABS_SRCDIR
\set contract :abs_srcdir '/sql/contract.inc'
\i :contract
SELECT test_chunk_use_store('memory') AS previous_store;
\i :contract
-- Rollback semantics are not part of the contract: this store keeps puts.
BEGIN;
SELECT test_chunk_put(chunk_hash('memory rollback'), 'memory rollback');
ROLLBACK;
SELECT test_chunk_exists(chunk_hash('memory rollback')) AS kept_after_rollback;
SELECT test_chunk_use_store('journal') AS previous_store;
SELECT test_chunk_use_store('bogus');

--
-- Journal implementation details.
--
SELECT name, setting FROM pg_settings
  WHERE name IN ('dolt_chunk_journal_tail', 'dolt_chunk_verify_hashes') ORDER BY name;

-- A put of a chunk the recent index holds appends nothing.
SELECT test_chunk_put(chunk_hash('beta'), 'beta') AS first_put;
SELECT test_chunk_put(chunk_hash('beta'), 'beta') AS second_put;
SELECT count(*) FROM dolt_chunk_list() WHERE chunk = chunk_hash('beta');

-- Puts are not transactional: a rolled-back put stays, for garbage collection.
BEGIN;
SELECT test_chunk_put(chunk_hash('gamma'), 'gamma');
ROLLBACK;
SELECT test_chunk_exists(chunk_hash('gamma')) AS kept_after_rollback;
BEGIN;
SAVEPOINT s;
SELECT test_chunk_put(chunk_hash('gamma2'), 'gamma2');
ROLLBACK TO s;
SELECT test_chunk_exists(chunk_hash('gamma2')) AS kept_after_savepoint_rollback;
COMMIT;

-- A deleted chunk can be put again, and is listed once.
SELECT test_chunk_put(chunk_hash('contract delta'), 'contract delta') AS reput_after_delete;
SELECT count(*), min(bytes) FROM dolt_chunk_list() WHERE chunk = chunk_hash('contract delta');
SELECT dolt_chunk_read(chunk_hash('contract delta')) = 'contract delta' AS read_back;
SELECT dolt_chunk_read(chunk_hash('contract missing')) IS NULL AS missing_read;

-- A logged put writes its bytes to the WAL once; an unlogged put writes none.
SELECT pg_current_wal_insert_lsn() AS before \gset
SELECT count(*) FILTER (WHERE test_chunk_put(chunk_hash(x), x)) AS logged_puts
  FROM (SELECT convert_to(repeat(lpad(g::text, 4, '0'), 1000), 'UTF8') AS x FROM generate_series(1, 100) g) s;
SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'before') BETWEEN 400000 AND 420000 AS logged_wal;
SELECT pg_current_wal_insert_lsn() AS before \gset
SELECT count(*) FILTER (WHERE test_chunk_put(chunk_hash(x), x, false)) AS unlogged_puts
  FROM (SELECT convert_to(repeat(lpad(g::text, 5, 'u'), 800), 'UTF8') AS x FROM generate_series(1, 100) g) s;
SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'before') < 4096 AS unlogged_wal;
SELECT count(*) FILTER (WHERE test_chunk_get(chunk_hash(x)) = x) AS unlogged_readable
  FROM (SELECT convert_to(repeat(lpad(g::text, 5, 'u'), 800), 'UTF8') AS x FROM generate_series(1, 100) g) s;

-- Read-only transactions cannot write.
BEGIN READ ONLY;
SELECT test_chunk_put(chunk_hash('delta'), 'delta');
ROLLBACK;

-- Repeated reads return the same bytes, and every chunk put is readable.
SELECT test_chunk_get(chunk_hash('contract alpha')) = 'contract alpha' AS first_read;
SELECT test_chunk_get(chunk_hash('contract alpha')) = 'contract alpha' AS second_read;
SELECT count(*) FILTER (WHERE test_chunk_put(chunk_hash(x), x)) AS inserted
  FROM (SELECT decode(repeat(lpad(to_hex(g), 2, '0'), 8000), 'hex') AS x
        FROM generate_series(1, 20) g) s;
SELECT count(*) FILTER (WHERE test_chunk_get(chunk_hash(x)) = x) AS all_readable
  FROM (SELECT decode(repeat(lpad(to_hex(g), 2, '0'), 8000), 'hex') AS x
        FROM generate_series(1, 20) g) s;

-- The journal's SQL access is private.
CREATE ROLE regress_chunk_reader;
SET ROLE regress_chunk_reader;
SELECT count(*) FROM dolt_chunk_list();
SELECT dolt_chunk_read(chunk_hash('beta'));
SELECT session_reads FROM dolt_chunk_stats();
RESET ROLE;
DROP ROLE regress_chunk_reader;
