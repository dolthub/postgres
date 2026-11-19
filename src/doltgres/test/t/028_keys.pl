# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The keys `access` makes of Postgres' values (design item 4, the storage
# design's II.6), through the test module's doltgres_test_key: for each type
# whose key is made from a datum's word or from plain bytes, rows ordered by
# their keys' bytes come in the order Postgres' own comparison gives them, in
# each direction and with NULLs at either end, and two values have one key
# exactly when Postgres calls them equal; a text's key is the same whether
# the value arrives inline, with a short header, compressed, or out of line;
# an enum's keys order as its labels after a renumber; a value of a type its
# btree class orders by a function is kept whole, the order against
# Postgres' coming with the index method's comparator; and a type with no
# default btree class is refused with Postgres' error.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('keys');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('postgres',
	'CREATE TYPE pair AS (a int4, b text); CREATE TYPE spaced AS (gone int8, kept int2, word text); ALTER TYPE spaced DROP ATTRIBUTE gone'
);
$node->safe_psql(
	'postgres', q{
CREATE DOMAIN posint AS int4 CHECK (VALUE > 0);
CREATE TYPE textrange_pattern AS RANGE (subtype = text, subtype_opclass = text_pattern_ops);
CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE TYPE textrange_icu AS RANGE (subtype = text, collation = "und-x-icu");
CREATE TYPE textrange_c AS RANGE (subtype = text, collation = "C");
CREATE TYPE named AS (n text COLLATE "und-x-icu", k int4);
});

# Each type with the values its keys are checked on, as SQL literals, and
# the collation its order is taken under, if it has one.
my @types = (
	[ 'bool', q{'true', 'false'}, '' ],
	[ '"char"', q{'a', 'Z', '0', '\200', '\377'}, '' ],
	[ 'int2', q{'-32768', '-1', '0', '1', '32767'}, '' ],
	[ 'int4', q{'-2147483648', '-1', '0', '1', '2147483647'}, '' ],
	[
		'int8',
		q{'-9223372036854775808', '-1', '0', '1', '9223372036854775807'}, ''
	],
	[ 'oid', q{'0', '1', '2147483648', '4294967295'}, '' ],
	[ 'money', q{'-1.50', '0', '0.01', '2'}, '' ],
	[
		'date',
		q{'-infinity', '4713-01-01 BC', '1999-12-31', '2000-01-01', 'infinity'},
		''
	],
	[ 'time', q{'00:00', '12:00:00.000001', '23:59:59.999999', '24:00'}, '' ],
	[
		'timestamp',
		q{'-infinity', '1999-12-31 23:59:59.999999', '2000-01-01', 'infinity'},
		''
	],
	[
		'timestamptz',
		q{'-infinity', '1999-12-31 23:59:59+00', '2000-01-01 00:00+02', 'infinity'},
		''
	],
	[ 'pg_lsn', q{'0/0', '0/1', '1/0', 'FFFFFFFF/FFFFFFFF'}, '' ],
	[ 'xid8', q{'0', '1', '4294967296', '18446744073709551615'}, '' ],
	[
		'float4',
		q{'-Infinity', '-1.5', '-0', '0', '1e-30', '2', 'Infinity', 'NaN'},
		''
	],
	[
		'float8',
		q{'-Infinity', '-1.5', '-0', '0', '1e-300', '2', '1e300', 'Infinity', 'NaN'},
		''
	],
	[
		'uuid',
		q{'00000000-0000-0000-0000-000000000000', '00000000-0000-0000-0000-000000000001', '80000000-0000-0000-0000-000000000000', 'ffffffff-ffff-ffff-ffff-ffffffffffff'},
		''
	],
	[
		'macaddr',
		q{'00:00:00:00:00:00', '00:00:00:00:00:01', '80:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff'},
		''
	],
	[
		'macaddr8',
		q{'00:00:00:00:00:00:00:00', '00:00:00:00:00:00:00:01', 'ff:ff:ff:ff:ff:ff:ff:ff'},
		''
	],
	[
		'bytea',
		q{'\x', '\x00', '\x0000', '\x00ff', '\x01', '\x61', '\x6100', '\xff'},
		''
	],
	[ 'text', q{'', 'A', 'a', 'ab', 'abc', 'b', 'é', 'z'}, 'COLLATE "C"' ],
	[
		'varchar',
		q{'', 'A', 'a', 'ab', 'abc', 'b', 'é', 'z'},
		'COLLATE "C"'
	],
	[ 'char(5)', q{'', 'A', 'a', 'a  ', 'ab', 'ab c', 'b'}, 'COLLATE "C"' ],
	[ 'name', q{'', 'A', 'a', 'ab', 'abc', 'b', 'é', 'z'}, 'COLLATE "C"' ],
	[
		'tid',
		q{'(0,0)', '(0,1)', '(1,0)', '(65536,2)', '(4294967295,65535)'},
		''
	],
	[
		'interval',
		q{'-1 month', '29 days 23:59:59.999999', '1 month', '30 days', '720:00:00', '1 month 00:00:00.000001', '1 year'},
		''
	],
	[
		'timetz',
		q{'00:00+00', '01:00+01', '00:00-02', '12:00+12', '23:59:59.999999-15:59', '24:00+00'},
		''
	],
	[
		'inet',
		q{'0.0.0.0/0', '10.0.0.0/8', '192.168.1.0/24', '192.168.1.5/24', '192.168.1.5', '::/0', '::1', '2001:db8::/32', 'ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff'},
		''
	],
	[
		'cidr',
		q{'0.0.0.0/0', '10.0.0.0/8', '10.0.0.0/16', '192.168.1.0/24', '::/0', '2001:db8::/32'},
		''
	],
	[ 'bit(3)', q{'000', '001', '100', '111'}, '' ],
	[
		'varbit',
		q{'', '0', '00', '01', '1', '10000000', '100000001'},
		''
	],
	[ 'oidvector', q{'', '0', '1', '1 0', '1 1', '2', '4294967295'}, '' ],
	[
		'int4[]',
		q{'{}', '{-5}', '{1,2}', '{1,2,3}', '[0:1]={1,3}', '{1,3}', '{{1,3}}', '{1,NULL}', '{2}', '{{1,2},{3,4}}', '{NULL}'},
		''
	],
	[
		'int4range',
		q{'empty', '(,)', '(,5)', '[-5,-1]', '[1,3)', '[1,4)', '[2,3)', '[2,)'},
		''
	],
	[ 'int8range', q{'empty', '(,0)', '[1,2)', '[9223372036854775806,)'}, '' ],
	[
		'int4multirange',
		q{'{}', '{(,)}', '{(,5)}', '{[1,3)}', '{[1,3),[5,7)}', '{[1,3),[6,7)}', '{[1,4)}', '{[2,3),[10,)}'},
		''
	],
	[
		'nummultirange',
		q{'{}', '{[1.5,2)}', '{(1.5,2),[3,4]}', '{(,0]}'},
		''
	],
	[
		'numrange',
		q{'empty', '(,1.5]', '[-1e10,0]', '[1.5,2)', '(1.5,2)', '[1.5,)', '[1.50,2.0)'},
		''
	],
	[
		'daterange',
		q{'empty', '(,2000-01-01)', '[2000-01-01,2000-02-01)', '[2000-01-01,)'},
		''
	],
	[
		'tsrange',
		q{'empty', '(,"2000-01-01")', '["2000-01-01 00:00","2000-01-02 00:00")', '("2000-01-01",)'},
		''
	],
	[
		'text[]',
		q{'{}', '{ab}', '{ab,a}', '{b}', '{NULL}', '{"",x}', '{{a,b},{c,d}}'},
		'COLLATE "C"'
	],
	[
		'numeric',
		q{'-Infinity', '-10000', '-1.5', '-0.0001', '0', '0.00', '0.0001', '1', '1.0', '1.5', '9999.9999', '10000', '1e100', '1e-100', 'Infinity', 'NaN'},
		''
	],
	[ 'pair', q{'(,)', '(1,)', '(1,a)', '(1,b)', '(2,"")', '(-3,z)', '(,a)'}, '' ],
	[ 'spaced', q{'(1,a)', '(1,)', '(,b)', '(-2,c)'}, '' ],
	[
		'text',
		q{'', 'a', 'A', 'b', 'B', 'ab', 'a b', 'Z', 'é', 'e', 'f'},
		'COLLATE "und-x-icu"'
	],
	[ 'varchar(10)', q{'', 'a', 'A', 'b', 'B', 'ab', 'é', 'z'}, 'COLLATE "de-x-icu"' ],
	[ 'char(5)', q{'', 'a', 'A  ', 'b', 'B', 'ab '}, 'COLLATE "und-x-icu"' ],
	[ 'text', q{'', 'a', 'A', 'b', 'B', 'ab', 'AB', 'é', 'e'}, 'COLLATE ci' ],
	[
		'text[]',
		q{'{}', '{a}', '{A}', '{a,B}', '{A,b}', '{b}', '{NULL}'},
		'COLLATE "und-x-icu"'
	],
	[ 'text[]', q{'{}', '{a}', '{A}', '{a,B}', '{b}', '{NULL}'}, 'COLLATE ci' ],
	[
		'textrange_icu',
		q{'empty', '[a,b)', '[A,b)', '[a,B)', '(,a]', '[b,)', '[a,a]'},
		''
	],
	[
		'textrange_c',
		q{'empty', '[a,b)', '[A,b)', '[B,a)', '(,a]', '[b,)', '[a,a]'},
		''
	],
	[ 'named', q{'(a,1)', '(A,1)', '(b,0)', '(B,2)', '(,1)', '(a,)'}, '' ],
	[ 'posint', q{'1', '2', '10', '2147483647'}, '' ],
	[ 'regclass', q{'pg_class', 'pg_type', 'pg_proc', '-', '1'}, '' ],
	[
		'tsvector',
		q{'', 'a', 'b', 'a b', 'ab', 'a:1', 'a:2', 'a:1A', 'a:1,2', 'a:1B b', 'a b:3', 'abc:1,5C xyz'},
		''
	],
	[
		'tsquery',
		q{'', 'a', 'b', 'ab', 'a & b', 'b & a', 'a | b', '!a', 'a <-> b', 'a <2> b', 'a:A', 'a:*', '(a | b) & !c', 'a & (b | c)'},
		''
	],
	[
		'jsonb',
		q{'null', 'true', 'false', '0', '1', '1.0', '-2.5', '1e10', '"a"', '"B"', '"b"', '""', '[]', '[null]', '[1]', '[1,2]', '[[1]]', '[{}]', '{}', '{"a":1}', '{"a":2}', '{"b":1}', '{"a":1,"b":2}', '{"aa":1}', '{"a":{"b":[1,"x"]}}', '[true,"a",null,2]'},
		''
	]);

# The four orders a key column can have, as doltgres_test_key's flags and
# as the ORDER BY that Postgres' own comparison sorts by.
my @orders = (
	[ 'false, false', 'ASC NULLS LAST' ],
	[ 'false, true', 'ASC NULLS FIRST' ],
	[ 'true, true', 'DESC NULLS FIRST' ],
	[ 'true, false', 'DESC NULLS LAST' ]);

for my $type (@types)
{
	my ($name, $values, $collation) = @$type;
	my $rows = join(', ', map { "($_)" } split(/, /, $values));
	$node->safe_psql(
		'postgres', qq{
DROP TABLE IF EXISTS keyed;
CREATE TABLE keyed (v $name $collation);
INSERT INTO keyed VALUES $rows, (NULL), (NULL);
});
	for my $order (@orders)
	{
		my ($flags, $sort) = @$order;
		my $by_key = $node->safe_psql('postgres',
			qq{SELECT array_agg(v::text ORDER BY doltgres_test_key(v, $flags), v::text COLLATE "C") FROM keyed}
		);
		my $by_value = $node->safe_psql('postgres',
			qq{SELECT array_agg(v::text ORDER BY v $collation $sort, v::text COLLATE "C") FROM keyed}
		);
		is($by_key, $by_value, "$name keys order as their values, $sort");
	}
	is( $node->safe_psql(
			'postgres',
			"SELECT count(DISTINCT doltgres_test_key(v, false, false)) = count(DISTINCT v $collation) + 1 FROM keyed"
		),
		't',
		"$name values have one key exactly when they are equal");
}

is( $node->safe_psql(
		'postgres',
		q{SELECT doltgres_test_key(NULL::int4, false, false) = '\x03' AND doltgres_test_key(NULL::text, true, true) = '\x01'}
	),
	't',
	"a NULL's key is its marker alone");

# One text four ways: with a short header and compressed as a table stores
# them, out of line in a column that isn't compressed, and inline with a
# full header as an expression's result.
$node->safe_psql(
	'postgres', q{
CREATE TABLE stored (id int4, v text);
INSERT INTO stored VALUES (1, 'abc'), (2, repeat('a', 10000));
ALTER TABLE stored ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO stored SELECT 3, string_agg(md5(n::text), '') FROM generate_series(1, 400) n;
});
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg((pg_column_size(v) < 127) || ' ' || (pg_column_compression(v) IS NOT NULL) || ' ' || (pg_column_toast_chunk_id(v) IS NOT NULL), ',' ORDER BY id) FROM stored}
	),
	'true false false,true true false,false false true',
	'the stored values are short, compressed, and out of line');
is( $node->safe_psql(
		'postgres',
		q{SELECT bool_and(doltgres_test_key(v COLLATE "C", false, false) = '\x02'::bytea || convert_to(v, 'UTF8') || '\x00'::bytea) FROM stored}
	),
	't',
	'a text read with a short header, compressed, and out of line gives its key'
);
is( $node->safe_psql(
		'postgres',
		q{SELECT doltgres_test_key(repeat('x', 300) COLLATE "C", false, false) = '\x02'::bytea || convert_to(repeat('x', 300), 'UTF8') || '\x00'::bytea}
	),
	't',
	'a text read inline with a full header gives its key');

# Rows stored before an attribute was dropped still hold its value, which
# their keys read past.
$node->safe_psql(
	'postgres', q{
CREATE TYPE aged AS (gone text, kept int4, word text);
CREATE TABLE aged_rows (v aged);
INSERT INTO aged_rows VALUES ('(xxxxxxxx,2,b)'), ('(y,1,c)'), ('(,1,a)'), ('(zz,,a)'), ('(w,1,)');
ALTER TYPE aged DROP ATTRIBUTE gone CASCADE;
});
is( $node->safe_psql(
		'postgres',
		q{SELECT array_agg(v::text ORDER BY doltgres_test_key(v, false, false)) = array_agg(v::text ORDER BY v) FROM aged_rows}
	),
	't',
	'rows stored before an attribute was dropped order as their values');

# Containers inside containers: arrays of rows holding a multirange, an
# array, and a row.
$node->safe_psql(
	'postgres', q{
CREATE TYPE nested AS (m int4multirange, a int4[], p pair);
CREATE TABLE nested_rows (v nested[]);
INSERT INTO nested_rows VALUES
	(ARRAY[ROW('{[1,3)}', '{1,2}', ROW(1, 'a')::pair)::nested]),
	(ARRAY[ROW('{[1,3)}', '{1,2}', ROW(1, 'b')::pair)::nested]),
	(ARRAY[ROW('{[1,3),[5,7)}', '{1}', NULL)::nested]),
	(ARRAY[ROW('{}', NULL, ROW(2, '')::pair)::nested, NULL]),
	(ARRAY[ROW('{[0,1)}', '{3}', ROW(NULL, 'z')::pair)::nested]),
	(ARRAY[]::nested[]),
	(ARRAY[ROW(NULL, '{}', ROW(1, 'a')::pair)::nested]);
});
is( $node->safe_psql(
		'postgres',
		q{SELECT array_agg(v::text ORDER BY doltgres_test_key(v, false, false)) = array_agg(v::text ORDER BY v) FROM nested_rows}
	),
	't',
	'nested containers order as their values');

# Text under ICU collations, in the order the reference gives it, and under
# a nondeterministic one, whose equal texts have one key.
my $words = q{(VALUES ('a'), ('B'), ('A'), ('b'), ('e'), ('ab'), ('a b'), ('')) t(v)};
is( $node->safe_psql(
		'postgres',
		qq{SELECT array_agg(v ORDER BY doltgres_test_key(v COLLATE "und-x-icu", false, false), v COLLATE "C") FROM $words}
	),
	'{"",a,A,"a b",ab,b,B,e}',
	'collated text orders as Postgres orders it');
my $cased = q{(VALUES ('a'), ('B'), ('A'), ('b'), ('e'), ('ab'), ('AB'), ('')) t(v)};
is( $node->safe_psql(
		'postgres',
		qq{SELECT array_agg(v ORDER BY doltgres_test_key(v COLLATE ci, false, false), v COLLATE "C") || count(DISTINCT doltgres_test_key(v COLLATE ci, false, false))::text FROM $cased}
	),
	'{"",A,a,AB,ab,B,b,e,5}',
	'text under a nondeterministic collation has one key for equal texts');

# Jsonb containers of more than 32 children, whose entries hold an end's
# offset at every 32nd child, and long strings.
is( $node->safe_psql(
		'postgres',
		q{SELECT array_agg(v::text ORDER BY doltgres_test_key(v, false, false), v::text) = array_agg(v::text ORDER BY v, v::text) FROM (SELECT jsonb_agg(g) AS v FROM generate_series(1, 40) g UNION ALL SELECT jsonb_agg(g) FROM generate_series(1, 41) g UNION ALL SELECT jsonb_agg(CASE WHEN g = 39 THEN 0 ELSE g END) FROM generate_series(1, 40) g UNION ALL SELECT jsonb_object_agg('k' || g, repeat('x', g)) FROM generate_series(1, 35) g UNION ALL SELECT jsonb_object_agg('k' || g, repeat('x', 36 - g)) FROM generate_series(1, 35) g) t}
	),
	't',
	'jsonb containers of many children order as their values');

# An enum whose labels run out of room between two neighbours, which
# Postgres renumbers, keys as its labels order.
$node->safe_psql('postgres',
	q{CREATE TYPE mood AS ENUM ('sad', 'happy'); CREATE TABLE moods (v mood)});
for my $n (1 .. 30)
{
	$node->safe_psql('postgres', qq{ALTER TYPE mood ADD VALUE 'v$n' BEFORE 'happy'});
}
$node->safe_psql('postgres',
	q{INSERT INTO moods SELECT unnest(enum_range(NULL::mood)); INSERT INTO moods VALUES (NULL)});
is( $node->safe_psql(
		'postgres',
		q{SELECT enumsortorder > 2 FROM pg_enum WHERE enumtypid = 'mood'::regtype AND enumlabel = 'happy'}
	),
	't',
	'the labels were renumbered');
is( $node->safe_psql(
		'postgres',
		q{SELECT array_agg(v::text ORDER BY doltgres_test_key(v, false, false)) = array_agg(v::text ORDER BY v) FROM moods}
	),
	't',
	"an enum's keys order as its labels after a renumber");

# A value its type's btree class orders by a function rather than by a byte
# rule is kept whole: equal values have one key, and their order is the index
# method's comparator's.
is( $node->safe_psql(
		'postgres',
		q{SELECT count(DISTINCT doltgres_test_key(v, false, false)) = count(DISTINCT v::text) FROM (VALUES ('[a,b)'::textrange_pattern), ('[a,c)'), ('[B,a)'), ('[a,b)'), ('empty')) t(v)}
	),
	't',
	'a compared value is its stored bytes');

my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{SELECT doltgres_test_key('(1,2)'::point, false, false)},
	extra_params => [ '-v', 'VERBOSITY=verbose' ]);
my ($index_ret, $index_stdout, $index_stderr) = $node->psql(
	'postgres',
	q{CREATE TABLE points (p point); CREATE INDEX ON points (p)},
	extra_params => [ '-v', 'VERBOSITY=verbose' ]);
like(
	$stderr,
	qr/ERROR:  42704: data type point has no default operator class for access method "btree"/,
	"a type with no default btree class is refused with Postgres' error");
is( ($stderr =~ /(ERROR:.*\n.*HINT:.*)/)[0],
	($index_stderr =~ /(ERROR:.*\n.*HINT:.*)/)[0],
	"the refusal is Postgres' own for an index on the type");

$node->stop;
done_testing();
