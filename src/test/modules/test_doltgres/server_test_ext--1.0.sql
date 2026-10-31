/* src/test/modules/test_doltgres/server_test_ext--1.0.sql */

\echo Use "CREATE EXTENSION server_test_ext" to load this file. \quit

-- An extension's objects belong to the server, whatever kind each is.
CREATE FUNCTION xt_double(int) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT $1 * 2 $$;
CREATE FUNCTION xt_next(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN $1 + 1; END $$;
CREATE TYPE xt_pair AS (a int, b int);
CREATE TABLE xt_config(k text PRIMARY KEY, v text);
INSERT INTO xt_config VALUES ('greeting', 'hello');
CREATE TABLE xt_copy AS SELECT k, v FROM xt_config;
CREATE VIEW xt_view AS SELECT k, xt_double(length(v)) AS doubled FROM xt_config;
CREATE MATERIALIZED VIEW xt_matview AS SELECT count(*) AS n FROM xt_config;
