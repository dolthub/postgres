\echo Use "CREATE EXTENSION dg_trusted" to load this file. \quit
CREATE TABLE dg_trusted_owner AS SELECT current_user AS runner, '@extowner@'::text AS owner;
CREATE FUNCTION dg_trusted_func() RETURNS int LANGUAGE sql AS 'SELECT 42';
