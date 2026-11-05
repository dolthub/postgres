/* src/test/modules/test_dolt_chunk/test_dolt_chunk--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_dolt_chunk" to load this file. \quit

-- Install the built-in 'journal' store or the test-only 'memory' store for
-- this session; returns the previous store's name.
CREATE FUNCTION test_chunk_use_store(name text) RETURNS text
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- An unlogged put is durable at commit but reaches no standby.
CREATE FUNCTION test_chunk_put(hash bytea, data bytea, logged bool DEFAULT true) RETURNS bool
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_put_many(hashes bytea[], data bytea[]) RETURNS int4
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_get(hash bytea) RETURNS bytea
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_get_many(hashes bytea[]) RETURNS bytea[]
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_exists(hash bytea) RETURNS bool
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_delete(hash bytea) RETURNS bool
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_scan_count() RETURNS int8
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_max_size() RETURNS int4
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_store_stats(OUT chunks float8, OUT bytes int8)
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_pin_survives_puts(hash bytea, count int4) RETURNS bool
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Benchmarks over synthetic chunks: chunk i of a seed has a pseudo-random
-- hash and |size| pseudo-random bytes. Each returns seconds spent in the
-- store calls only, excluding data generation. A size of 0 draws each chunk's
-- size uniformly from 512 to 8000 bytes.
CREATE FUNCTION test_chunk_bench_put(seed int4, first int8, count int8, size int4) RETURNS float8
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_chunk_bench_get(seed int4, nchunks int8, count int8, rngseed int4) RETURNS float8
	AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A chunk's hash: SHA-256 truncated to the store's hash size.
CREATE FUNCTION chunk_hash(data bytea) RETURNS bytea
LANGUAGE sql IMMUTABLE STRICT AS $$ SELECT substr(sha256(data), 1, 20) $$;
