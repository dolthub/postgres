-- Records how Postgres 19 orders values of each type, for the key encoding tests. Each row is the type, the value's
-- rank in ORDER BY (equal values share a rank), and the value's binary send form in hex. Run it with
-- tests/pg19_orders.sh, which writes tests/pg19_orders.tsv, in a database whose default collation is C.
\set ON_ERROR_STOP 1
SET extra_float_digits = 3;
SET TIME ZONE 'UTC';
CREATE TYPE pair AS (a int4, b text);

SELECT 'bool', dense_rank() OVER (ORDER BY v), encode(boolsend(v), 'hex')
FROM unnest(ARRAY[true, false]) v ORDER BY v;

SELECT 'char', dense_rank() OVER (ORDER BY v), encode(charsend(v), 'hex')
FROM unnest(ARRAY['a', 'b', 'A', ' ', '~', E'\x7f', E'\x01']::"char"[]) v ORDER BY v;

SELECT 'int2', dense_rank() OVER (ORDER BY v), encode(int2send(v), 'hex')
FROM unnest(ARRAY[-32768, -256, -1, 0, 1, 255, 256, 32767]::int2[]) v ORDER BY v;

SELECT 'int4', dense_rank() OVER (ORDER BY v), encode(int4send(v), 'hex')
FROM unnest(ARRAY[-2147483648, -65536, -1, 0, 1, 7, 65536, 2147483647]::int4[]) v ORDER BY v;

SELECT 'int8', dense_rank() OVER (ORDER BY v), encode(int8send(v), 'hex')
FROM unnest(ARRAY[-9223372036854775808, -4294967296, -1, 0, 1, 4294967296, 9223372036854775807]::int8[]) v
ORDER BY v;

SELECT 'oid', dense_rank() OVER (ORDER BY v), encode(oidsend(v), 'hex')
FROM unnest(ARRAY[0, 1, 2147483647, 2147483648, 4294967295]::oid[]) v ORDER BY v;

SELECT 'float4', dense_rank() OVER (ORDER BY v), encode(float4send(v), 'hex')
FROM unnest(ARRAY['-Infinity', '-3.4e38', '-1', '-1e-45', '-0', '0', '1e-45', '1.17549435e-38', '1', '1.0000001',
    '3.4e38', 'Infinity', 'NaN']::float4[]) v ORDER BY v;

SELECT 'float8', dense_rank() OVER (ORDER BY v), encode(float8send(v), 'hex')
FROM unnest(ARRAY['-Infinity', '-1.7976931348623157e308', '-1', '-5e-324', '-0', '0', '5e-324', '1',
    '1.0000000000000002', '1.7976931348623157e308', 'Infinity', 'NaN']::float8[]) v ORDER BY v;

SELECT 'numeric', dense_rank() OVER (ORDER BY v), encode(numeric_send(v), 'hex')
FROM unnest(ARRAY['-Infinity', '-1e100', '-123456789.123456789', '-101', '-100', '-99', '-10', '-1.5', '-1',
    '-1.00', '-0.5', '-0.05', '-1e-100', '0', '0.0', '-0', '0.000', '1e-100', '0.0001', '0.001', '0.01', '0.05',
    '0.1', '0.5', '0.50', '1', '1.0', '1.00', '1.5', '5', '9', '10', '10.0', '50', '99', '99.99', '100', '101',
    '9999', '10000', '10001', '123456789.123456789', '1e100', '1e1000', 'Infinity', 'NaN']::numeric[]) v
ORDER BY v;

SELECT 'text', dense_rank() OVER (ORDER BY v COLLATE "C"), encode(textsend(v), 'hex')
FROM unnest(ARRAY['', ' ', 'a', 'a ', 'ab', 'abc', 'b', 'B', 'ä', 'é', E'\x01', E'\x7f', '中', '😀', 'zz']) v
ORDER BY v COLLATE "C";

SELECT 'bpchar', dense_rank() OVER (ORDER BY v COLLATE "C"), encode(bpcharsend(v), 'hex')
FROM unnest(ARRAY['', 'a', 'a  ', 'ab', 'a b', 'b']::bpchar[]) v ORDER BY v COLLATE "C";

SELECT 'bytea', dense_rank() OVER (ORDER BY v), encode(byteasend(v), 'hex')
FROM unnest(ARRAY['\x', '\x00', '\x0000', '\x0001', '\x00ff', '\x01', '\x0100', '\xff', '\xff00', '\xffff']::bytea[]) v
ORDER BY v;

SELECT 'varbit', dense_rank() OVER (ORDER BY v), encode(varbit_send(v), 'hex')
FROM unnest(ARRAY['', '0', '00', '000000000', '01', '1', '10', '100', '1000000001', '11', '11111111',
    '111111110', '111111111']::varbit[]) v ORDER BY v;

SELECT 'uuid', dense_rank() OVER (ORDER BY v), encode(uuid_send(v), 'hex')
FROM unnest(ARRAY['00000000-0000-0000-0000-000000000000', '00000000-0000-0000-0000-000000000001',
    '7fffffff-ffff-ffff-ffff-ffffffffffff', '80000000-0000-0000-0000-000000000000',
    'ffffffff-ffff-ffff-ffff-ffffffffffff']::uuid[]) v ORDER BY v;

SELECT 'inet', dense_rank() OVER (ORDER BY v), encode(inet_send(v), 'hex')
FROM unnest(ARRAY['0.0.0.0/0', '9.255.0.0/16', '10.0.0.0/8', '10.0.0.0/16', '10.0.0.1', '10.0.0.1/8',
    '10.1.0.0/16', '10.1.0.0/8', '192.168.1.0/24', '192.168.1.5/24', '192.168.1.5', '255.255.255.255', '::/0',
    '::1', '::ffff:10.0.0.1', 'fe80::/10', 'fe80::1/10', 'ffff::/16']::inet[]) v ORDER BY v;

SELECT 'cidr', dense_rank() OVER (ORDER BY v), encode(cidr_send(v), 'hex')
FROM unnest(ARRAY['0.0.0.0/0', '10.0.0.0/8', '10.0.0.0/16', '10.1.0.0/16', '10.0.0.0/32', '::/0',
    'fe80::/10']::cidr[]) v ORDER BY v;

SELECT 'interval', dense_rank() OVER (ORDER BY v), encode(interval_send(v), 'hex')
FROM unnest(ARRAY['-infinity', '-1 year', '-1 day', '-24 hours', '-1 microsecond', '0', '1 microsecond',
    '1 day', '24 hours', '30 days', '1 month', '720 hours', '1 month -30 days', '1 mon 1 microsecond', '1 year',
    '360 days', 'infinity']::interval[]) v ORDER BY v;

SELECT 'timetz', dense_rank() OVER (ORDER BY v), encode(timetz_send(v), 'hex')
FROM unnest(ARRAY['00:00+14', '00:00+00', '01:00+01', '11:00-01', '12:00+00', '13:00+01', '23:59-12',
    '24:00-12', '12:00:00.000001+00']::timetz[]) v ORDER BY v;

SELECT 'date', dense_rank() OVER (ORDER BY v), encode(date_send(v), 'hex')
FROM unnest(ARRAY['-infinity', '4713-01-01 BC', '1999-12-31', '2000-01-01', '2026-10-13', '5874897-12-31',
    'infinity']::date[]) v ORDER BY v;

SELECT 'timestamp', dense_rank() OVER (ORDER BY v), encode(timestamp_send(v), 'hex')
FROM unnest(ARRAY['-infinity', '4713-01-01 00:00 BC', '1999-12-31 23:59:59.999999', '2000-01-01',
    '2026-10-13 05:48', '294276-12-31 23:59:59.999999', 'infinity']::timestamp[]) v ORDER BY v;

SELECT 'int4[]', dense_rank() OVER (ORDER BY v), encode(array_send(v), 'hex')
FROM (VALUES ('{}'::int4[]), ('{1}'), ('{1,2}'), ('{1,2,3}'), ('{1,NULL}'), ('{1,NULL,3}'), ('{NULL}'), ('{NULL,1}'), ('{2}'), ('{-1}'), ('{{1,2},{3,4}}'), ('{{1,2,3,4}}'), ('{{1},{2},{3},{4}}'), ('[0:1]={1,2}'), ('[2:3]={1,2}'), ('{1,2,3,4}'), ('[0:0]={1}')) t(v) ORDER BY v;

SELECT 'text[]', dense_rank() OVER (ORDER BY v COLLATE "C"), encode(array_send(v), 'hex')
FROM (VALUES ('{}'::text[]), ('{a}'), ('{a,b}'), ('{ab}'), ('{"",a}'), ('{NULL}'), ('{b}')) t(v) ORDER BY v COLLATE "C";

SELECT 'int4range', dense_rank() OVER (ORDER BY v), encode(range_send(v), 'hex')
FROM unnest(ARRAY['empty', '(,)', '(,5)', '(,5]', '[1,2)', '[1,3)', '[1,)', '(1,3)', '[2,3)', '[5,)']::int4range[]) v
ORDER BY v;

SELECT 'numrange', dense_rank() OVER (ORDER BY v), encode(range_send(v), 'hex')
FROM unnest(ARRAY['empty', '(,)', '(,1)', '(,1]', '[1,1]', '[1,2)', '[1,2]', '[1.0,2]', '(1,2)', '(1,2]',
    '[1,)', '(1,)', '[2,3]']::numrange[]) v ORDER BY v;

SELECT 'int4multirange', dense_rank() OVER (ORDER BY v), encode(multirange_send(v), 'hex')
FROM unnest(ARRAY['{}', '{[1,2)}', '{[1,2),[3,4)}', '{[1,2),[5,6)}', '{[1,3)}', '{(,1)}', '{[2,)}']::int4multirange[]) v
ORDER BY v;

SELECT 'pair', dense_rank() OVER (ORDER BY v), encode(record_send(v), 'hex')
FROM unnest(ARRAY[ROW(1, 'a'), ROW(1, 'b'), ROW(1, NULL), ROW(NULL, 'a'), ROW(NULL, NULL), ROW(0, 'z'),
    ROW(1, '')]::pair[]) v ORDER BY v;

SELECT 'jsonb', dense_rank() OVER (ORDER BY v), encode(jsonb_send(v), 'hex')
FROM unnest(ARRAY['null', 'true', 'false', '0', '1', '1.0', '-1', '1e3', '"a"', '"b"', '""', '"aa"', '[]', '{}',
    '[null]', '[1]', '["a"]', '[[]]', '[{}]', '[true]', '[1,2]', '[2,1]', '[1,[2]]', '[[1],2]', '[1,"a"]',
    '{"a":1}', '{"b":1}', '{"aa":1}', '{"a":2}', '{"a":null}', '{"a":1,"b":2}', '{"b":1,"aa":2}',
    '{"a":[1]}', '{"a":{"b":1}}', '[[1,2]]', '[1,2,3]']::jsonb[]) v ORDER BY v;

SELECT 'tsvector', dense_rank() OVER (ORDER BY v), encode(tsvectorsend(v), 'hex')
FROM unnest(ARRAY['', 'a', 'b', 'aa', 'ab', 'é', '中', 'a:1', 'a:2', 'a:16383', 'a:1A', 'a:1B', 'a:1C', 'a:1,2',
    'a:2,3', 'a:1,2,3', 'a b', 'a:1 b', 'a b:1', 'a:1 b:1', 'a:1A b:2', 'c d e', 'abc:1,2,3 d']::tsvector[]) v
ORDER BY v;

SELECT 'tsquery', dense_rank() OVER (ORDER BY v), encode(tsquerysend(v), 'hex')
FROM unnest(ARRAY['', 'a', 'b', 'c', 'd', 'e', 'ab', 'foo', 'bar', 'baz', 'qux', 'é', '中', 'a:A', 'a:*', 'a:*BC',
    '!a', '!b', '!!a', 'a & b', 'b & a', 'a | b', 'a & c', 'a <-> b', 'a <0> b', 'a <2> b', 'b <-> a',
    'a & b & c', '(a & b) | c', 'a & (b | c)', '!(a & b)', 'a <-> (b | c)', 'foo & !bar']::tsquery[]) v
ORDER BY v;
