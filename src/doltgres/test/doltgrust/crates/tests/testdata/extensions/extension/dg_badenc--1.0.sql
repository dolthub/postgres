\echo Use "CREATE EXTENSION dg_badenc" to load this file. \quit

CREATE FUNCTION dg_badenc_word() RETURNS text LANGUAGE sql AS $$SELECT 'aÿ'$$;
