\echo Use "CREATE EXTENSION dg_latin1" to load this file. \quit

CREATE FUNCTION dg_latin1_word() RETURNS text LANGUAGE sql AS $$SELECT 'café ½'$$;
