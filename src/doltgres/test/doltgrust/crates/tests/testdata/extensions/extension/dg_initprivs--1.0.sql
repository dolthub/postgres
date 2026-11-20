\echo Use "CREATE EXTENSION dg_initprivs" to load this file. \quit

CREATE TABLE dg_ip_tab (a int, b int);
GRANT SELECT ON dg_ip_tab TO PUBLIC;
GRANT UPDATE (b) ON dg_ip_tab TO ip_role;
CREATE FUNCTION dg_ip_func() RETURNS int LANGUAGE sql AS 'SELECT 1';
REVOKE EXECUTE ON FUNCTION dg_ip_func() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION dg_ip_func() TO ip_role;
CREATE SCHEMA dg_ip_schema;
GRANT USAGE ON SCHEMA dg_ip_schema TO PUBLIC;
SELECT lo_create(4242424);
GRANT SELECT ON LARGE OBJECT 4242424 TO PUBLIC;
