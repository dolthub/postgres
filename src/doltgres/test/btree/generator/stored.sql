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

-- The corpus's values of the key types whose stored form Postgres' input functions make: run.sh runs it on Postgres
-- 19, which stores each value in a table of a transaction it rolls back, and prints each value's type, length, and
-- stored bytes, header included, as pageinspect reads them from the table's page.
BEGIN;
CREATE EXTENSION pageinspect;
CREATE FUNCTION stored(relation regclass) RETURNS TABLE (bytes text, n bigint) LANGUAGE sql AS
    $$ SELECT encode(t_attrs[1], 'hex'), lp FROM heap_page_item_attrs(get_raw_page(relation::text, 0), relation) $$;
CREATE TABLE c01 (v numeric);
INSERT INTO c01 VALUES (0), (-1.5), (1.50), (123456789.000123), ('NaN'), ('Infinity'), ('-Infinity'), (1e100), (-1e-100);
CREATE TABLE c02 (v jsonb);
INSERT INTO c02 VALUES ('null'), ('1'), ('1.0'), ('"a"'), ('[1, "b", null, true]'), ('{"a": 1, "b": [false, {}]}');
CREATE TABLE c03 (v oidvector);
INSERT INTO c03 VALUES (''), ('1 2 3'), ('4294967295');
CREATE TABLE c04 (v inet);
INSERT INTO c04 VALUES ('0.0.0.0/0'), ('192.168.1.5/24'), ('10.0.0.0/8'), ('::1'), ('2001:db8::1/64');
CREATE TABLE c05 (v cidr);
INSERT INTO c05 VALUES ('192.168.1.0/24'), ('2001:db8::/32');
CREATE TABLE c06 (v interval);
INSERT INTO c06 VALUES ('0'), ('1 year 2 mons 3 days 04:05:06.789'), ('-1 day'), ('24 hours'), ('infinity');
CREATE TABLE c07 (v timetz);
INSERT INTO c07 VALUES ('00:00+00'), ('12:34:56.789-07'), ('23:59:59+14'), ('12:00-14');
CREATE TABLE c08 (v bit(8));
INSERT INTO c08 VALUES (B'00000000'), (B'10101010'), (B'11111111');
CREATE TABLE c09 (v varbit);
INSERT INTO c09 VALUES (B''), (B'1'), (B'1010101011');
CREATE TABLE c10 (v tsvector);
INSERT INTO c10 VALUES (''), ('a fat cat'), ('a:1A fat:2,3 cat:5B');
CREATE TABLE c11 (v tsquery);
INSERT INTO c11 VALUES ('a'), ('fat & (rat | cat)'), ('!a <-> b:*');
SELECT a.atttypid, t.typlen, s.bytes
FROM pg_attribute a JOIN pg_type t ON t.oid = a.atttypid, LATERAL stored(a.attrelid) s
WHERE a.attrelid::regclass::text ~ '^c[0-9]{2}$' AND a.attnum = 1
ORDER BY a.attrelid::regclass::text, s.n;
ROLLBACK;
