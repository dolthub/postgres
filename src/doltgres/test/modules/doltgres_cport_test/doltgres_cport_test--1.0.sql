/* src/doltgres/test/modules/doltgres_cport_test/doltgres_cport_test--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION doltgres_cport_test" to load this file. \quit

CREATE FUNCTION doltgres_cport_test_host(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_host', 'doltgres_cport_test_host'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_trap()
RETURNS void
AS 'doltgres_cport_test_host', 'doltgres_cport_test_trap'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_panic_in_cleanup()
RETURNS void
AS 'doltgres_cport_test_host', 'doltgres_cport_test_panic_in_cleanup'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_catch_in_cleanup()
RETURNS text
AS 'doltgres_cport_test_host', 'doltgres_cport_test_catch_in_cleanup'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_encoding(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_encoding', 'doltgres_cport_test_encoding'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_std(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_std', 'doltgres_cport_test_std'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_std_runner(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_std', 'doltgres_cport_test_std_runner'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_chunks(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_chunks', 'doltgres_cport_test_chunks'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_alloc_failure()
RETURNS void
AS 'doltgres_cport_test_host', 'doltgres_cport_test_alloc_failure'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_simc(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_simc', 'doltgres_cport_test_simc'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_encoding_pg19_orders(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_encoding_pg19_orders', 'doltgres_cport_test_encoding_pg19_orders'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_pg(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_pg', 'doltgres_cport_test_pg'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_shmem(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_shmem', 'doltgres_cport_test_shmem'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_pghost(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_pghost', 'doltgres_cport_test_pghost'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_trees(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_trees', 'doltgres_cport_test_trees'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_repo(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_repo', 'doltgres_cport_test_repo'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_txn(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_txn', 'doltgres_cport_test_txn'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_doltgres(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_doltgres', 'doltgres_cport_test_doltgres'
LANGUAGE C;

CREATE FUNCTION doltgres_cport_test_access(OUT name text, OUT outcome text)
RETURNS SETOF record
AS 'doltgres_cport_test_access', 'doltgres_cport_test_access'
LANGUAGE C;
