\echo Use "CREATE EXTENSION dg_initprivs_db" to load this file. \quit

DO $$ BEGIN EXECUTE format('GRANT CREATE ON DATABASE %I TO ip_db_role', current_database()); END $$;
GRANT SET ON PARAMETER log_lock_waits TO ip_db_role;
