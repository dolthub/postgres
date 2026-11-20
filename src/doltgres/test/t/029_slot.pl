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

# The fork's tuple table slot (design item 5), through the test module's
# doltgres_test_slot: each row of a heap table is stored in a slot of the
# fork's kind as its table's tree would store it, with or without a primary
# key, and what Postgres' own slot_getattr then reads from the slot is
# compared with heap's datum for the same value, word for word and byte for
# byte, header and alignment included. The slot's other callbacks are checked
# the same way: a copy to another slot, a clear, its minimal and heap tuples,
# a copy from a slot of another kind, with as many attributes or fewer, the
# executor's values made the slot's own, and the system attributes it refuses
# until a version is read.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('slot');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# A table with a column of each kind of value: passed by value at each width,
# with a bool and a "char" whose high bit is set, of a fixed length at each
# alignment, and varlenas short enough for a one-byte header, just too long
# for one, never packed, compressed, and out of line; with NULLs, a dropped
# column, and a column added after its first rows.
$node->safe_psql(
	'postgres', q{
CREATE TABLE slot_rows (
	k int4 NOT NULL,
	t text NOT NULL,
	b bool,
	c "char",
	s int2,
	i int4,
	l int8,
	f4 float4,
	f8 float8,
	n name,
	v interval,
	u uuid,
	gone int4,
	d date,
	y bytea,
	plain text,
	big text
);
ALTER TABLE slot_rows ALTER COLUMN plain SET STORAGE PLAIN;
INSERT INTO slot_rows VALUES
	(1, 'short', true, '\351', -2, -3, -4, 'NaN', '-0', 'a name',
	 '1 year 2 days 3 seconds', '00112233-4455-6677-8899-aabbccddeeff', 7,
	 '2000-01-01', '\x00ff', 'never packed', repeat('x', 5000)),
	(2, repeat('y', 126), false, 'A', 32767, 2147483647, 9223372036854775807,
	 1.5, 1e300, '', '-1 month', NULL, NULL, 'infinity', '', '', ''),
	(3, repeat('y', 127), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	 NULL, NULL, NULL, NULL, NULL, NULL, NULL);
ALTER TABLE slot_rows ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO slot_rows (k, t, big)
	SELECT 4, 'out of line', string_agg(md5(g::text), '')
	FROM generate_series(1, 400) g;
ALTER TABLE slot_rows DROP COLUMN gone;
INSERT INTO slot_rows (k, t, c) VALUES (5, 'after the drop', '\200');
ALTER TABLE slot_rows ADD COLUMN later int8;
INSERT INTO slot_rows (k, t, later) VALUES (6, '', -1);

CREATE TABLE slot_keys (
	p char(4) NOT NULL,
	m macaddr NOT NULL,
	u uuid NOT NULL,
	x int8,
	w varchar
);
INSERT INTO slot_keys VALUES
	('ab', '08:00:2b:01:02:03', 'ffeeddcc-bbaa-9988-7766-554433221100', 1, 'w'),
	('abcd', 'ff:ff:ff:ff:ff:ff', '00000000-0000-0000-0000-000000000000',
	 NULL, NULL);

CREATE TABLE slot_exact (
	n name NOT NULL,
	t tid NOT NULL,
	z timetz NOT NULL,
	a inet NOT NULL,
	c cidr NOT NULL,
	b bit(9) NOT NULL,
	v varbit NOT NULL,
	o oidvector NOT NULL,
	x int4
);
INSERT INTO slot_exact VALUES
	('a name', '(7,3)', '01:00-02', '192.168.1.5/24', '2001:db8::/32',
	 B'101010101', B'1', '5 4000000000', 1),
	('', '(0,0)', '24:00+15:59', '::1', '0.0.0.0/0', B'000000000', B'', '',
	 NULL);

CREATE TABLE slot_one_name (n name NOT NULL, x int4);
INSERT INTO slot_one_name VALUES ('a', 1), ('', NULL);
CREATE TABLE slot_one_oids (o oidvector NOT NULL);
INSERT INTO slot_one_oids VALUES ('1 2 3'), ('');
});
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg((pg_column_compression(big) IS NOT NULL) || ' ' || (pg_column_toast_chunk_id(big) IS NOT NULL), ',' ORDER BY k) FROM slot_rows WHERE k IN (1, 4)}
	),
	'true false,false true',
	'the long values are stored compressed and out of line');

# slot runs one step of the test module's slot test over a table whose first
# columns are its primary key, and returns how many rows it compared.
sub slot
{
	my ($table, $keys, $descending, $step) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_slot('$table', $keys, $descending, $step)");
}

is( $node->safe_psql('postgres', q{SELECT doltgres_test_slot_atts('slot_rows')}),
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_attribute WHERE attrelid = 'slot_rows'::regclass AND attnum > 0}
	),
	"each attribute's packed word is its pg_attribute row");

is(slot('slot_rows', 0, 'false', 0),
	6, "a row stored in the fork's slot reads back as heap's own row");
is(slot('slot_rows', 0, 'false', 0),
	6, "a stored row's system columns are heap's own");
foreach my $keys (1, 2)
{
	foreach my $descending ('false', 'true')
	{
		is(slot('slot_rows', $keys, $descending, 0),
			6,
			"a row keyed by its first $keys columns, descending $descending, reads back as heap's own row"
		);
	}
}
foreach my $descending ('false', 'true')
{
	is(slot('slot_keys', 3, $descending, 0),
		2,
		"a row whose key holds a lossy column and two of a fixed length, descending $descending, reads back as heap's own row"
	);
}
foreach my $descending ('false', 'true')
{
	is(slot('slot_exact', 8, $descending, 0),
		2,
		"a row whose key holds a name, a tid, a time with time zone, network addresses, bit strings, and an oidvector, descending $descending, reads back as heap's own row"
	);
}

# A value read from a key can take more bytes than its key holds: a name is
# its characters in the key and 64 bytes as a datum, and an oidvector's
# array form has a header its key doesn't.
foreach my $table ('slot_one_name', 'slot_one_oids')
{
	is(slot($table, 1, 'false', 0),
		2,
		"a row of $table, whose one key column takes more bytes than its key, reads back as heap's own row"
	);
	is(slot($table, 1, 'true', 1),
		2, "and so does its copy in a second slot of the fork's kind");
}

is(slot('slot_rows', 2, 'false', 1),
	6, "a row copied from a slot of the fork's kind reads back the same");
is(slot('slot_rows', 2, 'true', 2),
	6, 'a cleared slot is empty and takes another row');
is(slot('slot_rows', 1, 'false', 3),
	6, 'a minimal tuple of the slot holds the same row');
is(slot('slot_rows', 2, 'false', 4),
	6, 'a heap tuple of the slot holds the same row');
is(slot('slot_rows', 0, 'false', 5),
	6, 'a row copied from a slot of another kind reads back the same');
is(slot('slot_rows', 0, 'false', 6),
	6,
	"the executor's values in the fork's slot outlive the memory they pointed into"
);

is(slot('slot_rows', 1, 'false', 7),
	6, "a stored row's system attribute is read from its version");
is(slot('slot_rows', 1, 'false', 8),
	6, "a row another transaction wrote isn't the current transaction's");
my ($ret, $stdout, $stderr);

# Postgres' virtual slot takes the row of a slot with fewer attributes, so
# the fork's slot does; a row of more attributes than it has room for is
# refused.
is(slot('slot_rows', 0, 'false', 9),
	6, 'a row copied from a slot with fewer attributes holds that many');
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{SELECT doltgres_test_slot('slot_rows', 0, false, 10)},
	extra_params => [ '-v', 'VERBOSITY=verbose' ]);
like(
	$stderr,
	qr/ERROR:  XX000: a slot can't take the row of the slot it was given/,
	'a row of more attributes than the slot has is refused');

# A key column whose key is longer than its table's bound is cut in the row's
# key, and the row's tuple then holds its value (design item 4's form D). A
# key of eight text columns has a bound of 991 bytes, and a text's key is two
# bytes longer than the text: rows one byte under the bound, at it, one byte
# over, well over, and two whose cut keys share their first bytes.
$node->safe_psql(
	'postgres', q{
CREATE TABLE slot_cut (a text, b text, c text, d text, e text, f text, g text, h text, n int4);
INSERT INTO slot_cut SELECT repeat('k', len), 'b', 'c', 'd', 'e', 'f', 'g', 'h', len
	FROM unnest(ARRAY[988, 989, 990, 1500]) AS len;
INSERT INTO slot_cut SELECT repeat('k', 1499) || last, 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0
	FROM unnest(ARRAY['1', '2']) AS last;
});
foreach my $descending ('false', 'true')
{
	is(slot('slot_cut', 8, $descending, 0),
		6,
		"a row whose key column is cut at its table's bound, descending $descending, reads back as heap's own row"
	);
}
is(slot('slot_cut', 8, 'false', 1),
	6, 'a row whose key column is cut is copied to another slot whole');

# A row stored before a column was added with a default holds no field for
# it, and reads back the default, as heap's own row does from its
# attmissingval; the source slot is heap's buffer slot.
$node->safe_psql(
	'postgres', q{
CREATE TABLE slot_added (k int4 PRIMARY KEY, t text);
INSERT INTO slot_added VALUES (1, 'a'), (2, NULL);
ALTER TABLE slot_added ADD COLUMN n int4 DEFAULT 5, ADD COLUMN s text DEFAULT 'five';
INSERT INTO slot_added VALUES (3, 'c', 6, 'six');
});
is(slot('slot_added', 1, 'false', 0),
	3, 'a row stored before a column was added with a default reads back the default');

# A stored row's system columns follow heap's header where its deleter
# changed it, each as the reference shows it: a cursor opened before its own
# transaction's delete reads the row's combo command id; another session's
# FOR UPDATE leaves the writer's command and shows the locker as xmax; and a
# rolled-back delete still shows its xmax and its command.
$node->safe_psql('postgres',
	'CREATE TABLE slot_cur (k int4 PRIMARY KEY, t text); CREATE TABLE slot_lock (k int4 PRIMARY KEY, t text); CREATE TABLE slot_undone (k int4 PRIMARY KEY, t text)'
);
is( $node->safe_psql(
		'postgres', q{
BEGIN;
INSERT INTO slot_cur VALUES (1, 'a');
DECLARE c CURSOR FOR SELECT doltgres_test_slot('slot_cur', 1, false, 0), (SELECT cmin || '|' || cmax || '|' || (xmin = xmax) FROM slot_cur);
DELETE FROM slot_cur;
FETCH c;
COMMIT;
}),
	'1|0|0|true',
	"a row read under a cursor opened before its own transaction's delete shows heap's combo command id");
$node->safe_psql('postgres', q{INSERT INTO slot_lock VALUES (1, 'a')});
my $locker = $node->background_psql('postgres');
$locker->query_safe('BEGIN; SELECT * FROM slot_lock FOR UPDATE');
is( $node->safe_psql(
		'postgres',
		q{SELECT doltgres_test_slot('slot_lock', 1, false, 0), (SELECT cmin || '|' || cmax || '|' || (xmax <> 0) FROM slot_lock)}
	),
	'1|0|0|true',
	"a row another session locks FOR UPDATE shows the locker and the writer's command");
$locker->query_safe('COMMIT');
$locker->quit;
is( $node->safe_psql(
		'postgres', q{
INSERT INTO slot_undone VALUES (1, 'a');
BEGIN; DELETE FROM slot_undone; ROLLBACK;
SELECT doltgres_test_slot('slot_undone', 1, false, 0), (SELECT cmin || '|' || cmax || '|' || (xmax <> 0) FROM slot_undone);
}),
	'1|0|0|true',
	"a row whose delete rolled back shows the aborted deleter");

# A row its own subtransaction wrote is the current transaction's until that
# subtransaction rolls back, on one slot. The rollback is the internal one
# PL/pgSQL's exception blocks make, not the statement ROLLBACK TO: both end
# in AbortSubTransaction and CleanupSubTransaction, and a slot can't outlive
# the one call the step makes.
$node->safe_psql('postgres',
	q{CREATE TABLE slot_sub (k int4 PRIMARY KEY, t text); INSERT INTO slot_sub VALUES (1, 'a')});
is(slot('slot_sub', 1, 'false', 11),
	1, 'a row its subtransaction wrote is current until the subtransaction rolls back');

# A row this transaction inserted and a rolled-back savepoint deleted keeps
# heap's combo command id and the savepoint's xmax, which a version of known
# numbers gives: the insert's command and the delete's raw command, the combo
# command id its delete took, as the reference shows the same script.
is( $node->safe_psql(
		'postgres', q{
CREATE TABLE slot_known (k int4 PRIMARY KEY, t text);
BEGIN;
INSERT INTO slot_known VALUES (1, 'a');
SAVEPOINT s;
DELETE FROM slot_known;
ROLLBACK TO s;
SELECT doltgres_test_slot('slot_known', 1, false, 12, (SELECT txid_current() || ',0,' || ((txid_current() >> 32 << 32) + (SELECT xmax::text::int8 FROM slot_known)) || ',' || (SELECT cmax FROM slot_known))),
	(SELECT cmin || '|' || cmax FROM slot_known);
COMMIT;
}),
	'1|0|0',
	"a row its rolled-back savepoint deleted shows heap's combo command id from known numbers");

# A row past the threshold moves out of line the values heap moves: its
# biggest EXTERNAL values first, until it is under the target, keeping a MAIN
# value inline. Heap never compresses an EXTERNAL value, so its choice is the
# fork's.
$node->safe_psql(
	'postgres', q{
CREATE TABLE slot_toast (k int4 PRIMARY KEY, a text, b text, c text);
ALTER TABLE slot_toast ALTER COLUMN a SET STORAGE EXTERNAL, ALTER COLUMN b SET STORAGE MAIN, ALTER COLUMN c SET STORAGE EXTERNAL;
INSERT INTO slot_toast VALUES (1, repeat('a', 1500), repeat('b', 1000), repeat('c', 1200)), (2, repeat('a', 100), repeat('b', 1000), repeat('c', 3000));
});
is(slot('slot_toast', 1, 'false', 0),
	2, 'a row past the threshold moves the values heap moves out of line');

# Bugs row 141: whether a value moves is decided on the row's heap tuple, so
# beside an int4 primary key a 2,016-byte or a 2,001-byte EXTERNAL value moves
# out of line, as heap's tuple passes the threshold though the fork's doesn't,
# and a 2,000-byte one, whose heap tuple is the threshold's 2,032 bytes, stays.
$node->safe_psql(
	'postgres', q{
CREATE TABLE slot_toast_edge (k int4 PRIMARY KEY, v text);
ALTER TABLE slot_toast_edge ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO slot_toast_edge VALUES (1, repeat('v', 2016)), (2, repeat('v', 2001)), (3, repeat('v', 2000));
});
is(slot('slot_toast_edge', 1, 'false', 0),
	3, "a value moves out of line by its row's heap tuple, as heap's does");

$node->stop;
done_testing();
