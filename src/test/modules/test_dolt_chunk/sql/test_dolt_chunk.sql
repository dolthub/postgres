CREATE EXTENSION test_dolt_chunk;

--
-- Interface contract, against the built-in heap store and then a test-only,
-- non-transactional in-memory store.
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
SELECT test_chunk_use_store('heap') AS previous_store;
SELECT test_chunk_use_store('bogus');

--
-- Heap implementation details.
--

-- Storage is inline and uncompressed, keyed by hash, part and inserting transaction.
SELECT attname, attstorage FROM pg_attribute
  WHERE attrelid = 'pg_dolt_chunk'::regclass AND attnum > 0 ORDER BY attnum;
SELECT reltoastrelid = 0 AS no_toast FROM pg_class WHERE oid = 'pg_dolt_chunk'::regclass;
SELECT indisunique, indisprimary FROM pg_index WHERE indexrelid = 'pg_dolt_chunk_hash_index'::regclass;
SELECT name, setting, unit FROM pg_settings
  WHERE name IN ('dolt_chunk_verify_hashes') ORDER BY name;

-- Puts never probe for a committed copy: every put of this chunk in the
-- contract stored one.
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = chunk_hash('contract alpha');

-- A chunk longer than one heap tuple spans parts, each inline and uncompressed.
SELECT test_chunk_put(chunk_hash(x), x) FROM (SELECT decode(repeat('cd', 20000), 'hex') AS x) s;
SELECT chunkpart, chunksize, length(chunkdata), pg_column_compression(chunkdata) IS NULL AS uncompressed
  FROM pg_dolt_chunk WHERE chunkhash = chunk_hash(decode(repeat('cd', 20000), 'hex')) ORDER BY chunkpart;
SELECT test_chunk_get(chunk_hash(x)) = x AS multipart_roundtrip
  FROM (SELECT decode(repeat('cd', 20000), 'hex') AS x) s;
SELECT test_chunk_delete(chunk_hash(decode(repeat('cd', 20000), 'hex'))) AS multipart_deleted;
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = chunk_hash(decode(repeat('cd', 20000), 'hex'));

-- Puts are transactional here: own writes are visible, rollback removes them.
BEGIN;
SELECT test_chunk_put(chunk_hash('beta'), 'beta');
SELECT test_chunk_exists(chunk_hash('beta')) AS own_write;
SELECT test_chunk_put(chunk_hash('beta'), 'beta') AS second_put;
ROLLBACK;
SELECT test_chunk_exists(chunk_hash('beta')) AS after_rollback;
SELECT test_chunk_put(chunk_hash('beta'), 'beta') AS reinserted;
BEGIN;
SAVEPOINT s;
SELECT test_chunk_put(chunk_hash('gamma'), 'gamma');
ROLLBACK TO s;
SELECT test_chunk_exists(chunk_hash('gamma')) AS after_savepoint_rollback;
COMMIT;

-- A deleted chunk can be put again.
SELECT test_chunk_put(chunk_hash('contract delta'), 'contract delta') AS reput_after_delete;
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = chunk_hash('contract delta');

-- Existence probes do not set hint bits on rows they examine. With data
-- checksums, a hint on a page's first change after a checkpoint logs a
-- full-page image. pageinspect reads the row without touching hints.
CREATE EXTENSION pageinspect;
CREATE FUNCTION chunk_xmin_hinted(hash bytea) RETURNS bool LANGUAGE sql AS $$
  SELECT (t_infomask & 256) <> 0
    FROM generate_series(0, pg_relation_size('pg_dolt_chunk') / 8192 - 1) AS b,
         heap_page_items(get_raw_page('pg_dolt_chunk', b::int)) AS i
   WHERE t_data IS NOT NULL AND position(hash IN t_data) > 0
$$;
SELECT test_chunk_put(chunk_hash('hintless'), 'hintless');
SELECT test_chunk_exists(chunk_hash('hintless')) AS exists;
SELECT chunk_xmin_hinted(chunk_hash('hintless')) AS hinted_after_probes;
-- Control: an ordinary MVCC read of the row sets the hint.
SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = chunk_hash('hintless');
SELECT chunk_xmin_hinted(chunk_hash('hintless')) AS hinted_after_select;
DROP FUNCTION chunk_xmin_hinted(bytea);
DROP EXTENSION pageinspect;

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

-- The catalog is private.
CREATE ROLE regress_chunk_reader;
SET ROLE regress_chunk_reader;
SELECT count(*) FROM pg_dolt_chunk;
RESET ROLE;
DROP ROLE regress_chunk_reader;
