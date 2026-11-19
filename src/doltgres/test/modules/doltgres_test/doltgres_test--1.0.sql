/* src/doltgres/test/modules/doltgres_test/doltgres_test--1.0.sql */
-- Copyright 2026 Dolthub, Inc.
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION doltgres_test" to load this file. \quit

CREATE FUNCTION doltgres_test_store(value int8) RETURNS void
AS 'MODULE_PATHNAME', 'dg_store' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_load() RETURNS int8
AS 'MODULE_PATHNAME', 'dg_load' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_lock_pairs(pairs int8, way int4) RETURNS float8
AS 'MODULE_PATHNAME', 'dg_lock_pairs' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_escalate() RETURNS void
AS 'MODULE_PATHNAME', 'dg_escalate' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_host(which int4) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_host' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_file(step int4) RETURNS text
AS 'MODULE_PATHNAME', 'dg_file' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_wal(value int8) RETURNS pg_lsn
AS 'MODULE_PATHNAME', 'dg_wal' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_wal_unknown(len int4) RETURNS void
AS 'MODULE_PATHNAME', 'dg_wal_unknown' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_delay(step int4) RETURNS void
AS 'MODULE_PATHNAME', 'dg_delay' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_check() RETURNS void
AS 'MODULE_PATHNAME', 'dg_check' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_lock(step int4) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_lock' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_shared(step int4, location int8, value int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_shared' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_zstd(input int4, dictionary int4) RETURNS bytea
AS 'MODULE_PATHNAME', 'dg_zstd' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_toast_lz4(value bytea) RETURNS bytea
AS 'MODULE_PATHNAME', 'dg_toast_lz4' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_cache(step int4, key int8, value int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_cache' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_delta(step int4, a int8, b int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_delta' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_sort_key(value text) RETURNS bytea
AS 'MODULE_PATHNAME', 'dg_sort_key' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_sort_key_prefix(value text, size int4) RETURNS bytea
AS 'MODULE_PATHNAME', 'dg_sort_key_prefix' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_status(xid int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_status' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_in_progress(xid int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_in_progress' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_writer(which int4) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_writer' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_command(used int4) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_command' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_is_current(xid int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_is_current' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_status_table(step int4, xid int8, value int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_status_table' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_kept_wal(from_lsn int8, to_lsn int8) RETURNS void
AS 'MODULE_PATHNAME', 'dg_kept_wal' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_repository(id int4) RETURNS void
AS 'MODULE_PATHNAME', 'dg_repository' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_registered() RETURNS void
AS 'MODULE_PATHNAME', 'dg_registered' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_session(save bool) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_session' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_key(value anyelement, descending bool, nulls_first bool) RETURNS bytea
AS 'MODULE_PATHNAME', 'dg_key' LANGUAGE C;

CREATE FUNCTION doltgres_test_words() RETURNS int8
AS 'MODULE_PATHNAME', 'dg_words' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_slot(rel regclass, keys int4, descending bool, step int4, version text DEFAULT '') RETURNS int8
AS 'MODULE_PATHNAME', 'dg_slot' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_slot_atts(rel regclass) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_slot_atts' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_table_routine(rel regclass) RETURNS bool
AS 'MODULE_PATHNAME', 'dg_table_routine' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_relation_creating(rel oid, rewrites oid) RETURNS void
AS 'MODULE_PATHNAME', 'dg_relation_creating' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_owned(step int4, value int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_owned' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_states(repository int8, count int8) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_states' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_relation_locks() RETURNS int8
AS 'MODULE_PATHNAME', 'dg_relation_locks' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_base(target regclass, source regclass, extra_ids int4 DEFAULT 0) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_base' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_scan(target regclass, backward bool, equals int8) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_scan' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_fetch(target regclass, row_tid tid) RETURNS text
AS 'MODULE_PATHNAME', 'dg_fetch' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_tid_valid(target regclass, row_tid tid) RETURNS bool
AS 'MODULE_PATHNAME', 'dg_tid_valid' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_latest(target regclass, row_tid tid) RETURNS tid
AS 'MODULE_PATHNAME', 'dg_latest' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_fetch_memory(target regclass, row_tid tid, times int4) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_fetch_memory' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_tidrange(target regclass, mintid tid, maxtid tid, backward bool) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_tidrange' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_tidrange_again(target regclass, mintid tid, maxtid tid, again_min tid, again_max tid)
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_tidrange_again' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_blocks(target regclass) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_blocks' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_estimate(target regclass, own bool) RETURNS text
AS 'MODULE_PATHNAME', 'dg_estimate' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_parallel(target regclass, workers int4, again bool, resave oid, mintid tid,
    maxtid tid)
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_parallel' LANGUAGE C;

CREATE FUNCTION doltgres_test_insert(target regclass, source regclass, multi bool, options int4) RETURNS SETOF tid
AS 'MODULE_PATHNAME', 'dg_insert' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_fetch(target regclass, row_tid tid) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_fetch' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_scan_dirty(target regclass) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_scan_dirty' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_toast(target regclass, row_tid tid, attnum int4, slice_offset int4, slice_length int4, value_id oid) RETURNS text
AS 'MODULE_PATHNAME', 'dg_toast' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_fetch_key(target regclass, source regclass, copy int8) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_index_fetch_key' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_bitmap(target regclass, exact tid[], lossy int4[]) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_bitmap' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_standing(target regclass, query text) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_standing_query' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_analyze(target regclass, fork bool, targrows int4, seed int4) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_analyze' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_write(target regclass, fork bool, query text) RETURNS int8
AS 'MODULE_PATHNAME', 'dg_write' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_delete(target regclass, fork bool, row_tid tid) RETURNS int4
AS 'MODULE_PATHNAME', 'dg_delete' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_scan_nonvacuumable(target regclass) RETURNS SETOF text
AS 'MODULE_PATHNAME', 'dg_scan_nonvacuumable' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_speculative(target regclass, fork bool, k int4, v text, succeeded bool, completes bool,
	indexed bool DEFAULT false) RETURNS text
AS 'MODULE_PATHNAME', 'dg_speculative' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_delete(target regclass, fork bool, tids tid[], known bool[], promising bool[],
	space int4) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_delete' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_fold(target regclass) RETURNS bool
AS 'MODULE_PATHNAME', 'dg_fold' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_routine(index regclass) RETURNS bool
AS 'MODULE_PATHNAME', 'dg_index_routine' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_vacuum(target regclass, index_cleanup text) RETURNS void
AS 'MODULE_PATHNAME', 'dg_vacuum' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_free_space(target regclass, block int4) RETURNS text
AS 'MODULE_PATHNAME', 'dg_free_space' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_base_sizes(target regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_base_sizes' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_entries(target regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_entries' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_buildempty(target regclass) RETURNS void
AS 'MODULE_PATHNAME', 'dg_index_buildempty' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_held(target regclass, row_tid tid) RETURNS bool
AS 'MODULE_PATHNAME', 'dg_held' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_build(target regclass, index regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_build' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_base(target regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_base' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_vacuumed(target regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_vacuumed' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_fold(index regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_fold' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_validate(target regclass, index regclass) RETURNS void
AS 'MODULE_PATHNAME', 'dg_index_validate' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_collect() RETURNS text
AS 'MODULE_PATHNAME', 'dg_collect' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_disk_space() RETURNS text
AS 'MODULE_PATHNAME', 'dg_disk_space' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_database_lock(target oid, hold bool) RETURNS bool
AS 'MODULE_PATHNAME', 'dg_database_lock' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_new_file(target regclass) RETURNS void
AS 'MODULE_PATHNAME', 'dg_new_file' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_truncate(target regclass) RETURNS void
AS 'MODULE_PATHNAME', 'dg_truncate' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_rewrite(target regclass, index regclass, sorted bool, fork bool) RETURNS text
AS 'MODULE_PATHNAME', 'dg_rewrite' LANGUAGE C;

CREATE FUNCTION doltgres_test_changes(target regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_changes' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_built_nodes() RETURNS text
AS 'MODULE_PATHNAME', 'dg_built_nodes' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_move(target regclass, tablespace text) RETURNS void
AS 'MODULE_PATHNAME', 'dg_move' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_mapped(target regclass) RETURNS bool
AS 'MODULE_PATHNAME', 'dg_mapped' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_identity(kind int4, number oid, attnum int4) RETURNS text
AS 'MODULE_PATHNAME', 'dg_identity' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_object_access_replaced() RETURNS void
AS 'MODULE_PATHNAME', 'dg_object_access_replaced' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_epochs() RETURNS text
AS 'MODULE_PATHNAME', 'dg_epochs' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_tree_height(index regclass) RETURNS int4
AS 'MODULE_PATHNAME', 'dg_index_tree_height' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_parallel(target regclass, index regclass, participants int4, backward bool)
RETURNS SETOF text AS 'MODULE_PATHNAME', 'dg_index_parallel' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_index_members(index regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_index_members' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_base_levels(target regclass) RETURNS int4
AS 'MODULE_PATHNAME', 'dg_base_levels' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_watch_rewrites() RETURNS void
AS 'MODULE_PATHNAME', 'dg_watch_rewrites' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_rewrites_seen() RETURNS text
AS 'MODULE_PATHNAME', 'dg_rewrites_seen' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_twin_handler(internal) RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'dg_twin_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD doltgres_twin TYPE TABLE HANDLER doltgres_test_twin_handler;

-- doltgres_test_twins makes heap twins the default table access method of the
-- current database and session, so a suite written before the switch makes
-- every table heap, as it was then.
CREATE PROCEDURE doltgres_test_twins() LANGUAGE plpgsql
AS $$
BEGIN
	EXECUTE format('ALTER DATABASE %I SET default_table_access_method = doltgres_twin', current_database());
	PERFORM set_config('default_table_access_method', 'doltgres_twin', false);
END
$$;

CREATE FUNCTION doltgres_test_totals(target regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_totals' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_count(target regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dg_count' LANGUAGE C STRICT;
CREATE FUNCTION doltgres_test_drop_constraint(target regclass, name text) RETURNS void
AS 'MODULE_PATHNAME', 'dg_drop_constraint' LANGUAGE C STRICT;

CREATE FUNCTION doltgres_test_unchain() RETURNS void
AS 'MODULE_PATHNAME', 'dg_unchain' LANGUAGE C STRICT;
