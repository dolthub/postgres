\echo Use "CREATE EXTENSION dg_config" to load this file. \quit
CREATE TABLE dg_config_settings (name text, value text);
SELECT pg_catalog.pg_extension_config_dump('dg_config_settings', 'WHERE name <> ''builtin''');
CREATE TABLE dg_config_log (entry text);
SELECT pg_catalog.pg_extension_config_dump('dg_config_log', '');
SELECT pg_catalog.pg_extension_config_dump('dg_config_log', 'WHERE false');
CREATE TABLE dg_config_other (x int);
