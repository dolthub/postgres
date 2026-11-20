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

# The row layouts DDL changes (STORAGE_DESIGN §II.10; design item 5; bugs row
# 131): a keyed table's pending transforms and a keyless table's rows re-keyed
# in place before its commit, the inheritors ADD and DROP COLUMN recurse into,
# the columns a dependency's cascade drops, and a rewrite later in the same
# statement, each read back as a heap twin's.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('layouts');
$node->init;
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE FUNCTION seven() RETURNS int4 VOLATILE LANGUAGE sql AS 'SELECT 7';
});

# twins runs a script over fork tables and over their heap twins, "%" standing
# for the tables' prefix, empty for the fork's and "heap_" for the twins', and
# "@AM" for the twins' access method, and checks that each gives the same
# output; "%" in an output is taken out. "@FOLD(table)" folds the fork's table
# now, reading false for each.
sub twins
{
	my ($case, $script) = @_;
	my %output;
	for my $prefix ('', 'heap_')
	{
		my $am = $prefix eq '' ? '' : 'USING doltgres_twin';
		(my $sql = $script) =~ s/%/$prefix/g;
		$sql =~ s/\@AM/$am/g;
		$sql =~ s/\@FOLD\((\w+)\)/$prefix eq '' ? "SELECT doltgres_test_fold('$1') IS NULL;" : 'SELECT false;'/ge;
		$output{$prefix} = $node->safe_psql('postgres', $sql) =~ s/heap_//gr;
	}
	is($output{''}, $output{heap_}, $case);
	return $output{''};
}

# Bugs row 131's repro: a DROP COLUMN, and an ADD COLUMN with a default before
# it, on a keyless table and on a keyed one, with rows written before and after.
for my $key ('', 'PRIMARY KEY')
{
	my $kind = $key eq '' ? 'keyless' : 'keyed';
	twins(
		$kind eq 'keyed'
		? "a keyed table's DROP COLUMN reads its rows as heap's"
		: "a keyless table's DROP COLUMN reads its rows as heap's",
		qq{
CREATE TABLE %t2_$kind (a integer $key, b bool DEFAULT true NOT NULL, c text DEFAULT 'xyzzy' NOT NULL) \@AM;
INSERT INTO %t2_$kind VALUES (1), (2);
ALTER TABLE %t2_$kind DROP COLUMN b;
INSERT INTO %t2_$kind VALUES (3, 'after');
SELECT * FROM %t2_$kind ORDER BY a;
});
	twins(
		$kind eq 'keyed'
		? "a keyed table's ADD COLUMN with a default then DROP COLUMN reads its rows as heap's"
		: "a keyless table's ADD COLUMN with a default then DROP COLUMN reads its rows as heap's",
		qq{
CREATE TABLE %t3_$kind (a integer $key, b bool DEFAULT true NOT NULL, c text) \@AM;
INSERT INTO %t3_$kind VALUES (1, false, 'one'), (2, true, 'two');
ALTER TABLE %t3_$kind ADD COLUMN d int8 DEFAULT 42, ADD COLUMN e text DEFAULT 'filled';
ALTER TABLE %t3_$kind DROP COLUMN b;
INSERT INTO %t3_$kind VALUES (3, 'three', 9, 'mine');
UPDATE %t3_$kind SET c = c || '!' WHERE a = 1;
SELECT * FROM %t3_$kind ORDER BY a;
});
}

# A fold after the DDLs commit re-forms the rows written before them through
# the chained transforms, keyed and keyless, and the folded rows read as heap's.
for my $key ('', 'PRIMARY KEY')
{
	my $kind = $key eq '' ? 'keyless' : 'keyed';
	twins(
		"a $kind table's rows folded after its DDLs read as heap's",
		qq{
CREATE TABLE %refolded_$kind (a integer $key, b bool DEFAULT true NOT NULL, c text) \@AM;
INSERT INTO %refolded_$kind VALUES (1, false, 'one'), (2, true, 'two');
ALTER TABLE %refolded_$kind ADD COLUMN d int8 DEFAULT 42;
ALTER TABLE %refolded_$kind DROP COLUMN b;
INSERT INTO %refolded_$kind VALUES (3, 'three', 9);
\@FOLD(refolded_$kind)
SELECT * FROM %refolded_$kind ORDER BY a;
});
}

# A keyless table's index: its entries follow the rows re-keyed in place, so an
# index scan finds each row as a sequential scan does.
twins(
	"a keyless table's index scan agrees with its rows after a DROP COLUMN",
	q{
CREATE TABLE %indexed (a int4, b text, c int4) @AM;
CREATE INDEX %indexed_c ON %indexed (c);
INSERT INTO %indexed SELECT g, 'b' || g, mod(g, 5) FROM generate_series(1, 50) g;
INSERT INTO %indexed SELECT 1, 'other', 1;
ALTER TABLE %indexed DROP COLUMN b;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT a, c FROM %indexed WHERE c = 1 ORDER BY a;
SELECT count(*) FROM %indexed WHERE c >= 0;
RESET enable_seqscan; RESET enable_bitmapscan;
SELECT a, c, count(*) FROM %indexed GROUP BY a, c HAVING count(*) > 1;
});

# The inheritors ADD and DROP COLUMN recurse into inside the parent's
# subcommand: an inheritance child and partitions, keyless and keyed.
twins(
	"an inheritance child's rows read as heap's after ADD and DROP COLUMN on its parent",
	q{
CREATE TABLE %ip (a int4, b text, c int4) @AM;
CREATE TABLE %ic (d int4) INHERITS (%ip) @AM;
CREATE TABLE %ick (k int4 PRIMARY KEY) INHERITS (%ip) @AM;
CREATE INDEX %ic_c ON %ic (c);
INSERT INTO %ip VALUES (1, 'p', 10);
INSERT INTO %ic VALUES (2, 'c', 20, 200), (3, 'c', 30, 300);
INSERT INTO %ick VALUES (4, 'k', 40, 4);
ALTER TABLE %ip ADD COLUMN e int4 DEFAULT 5;
ALTER TABLE %ip DROP COLUMN b;
INSERT INTO %ic VALUES (5, 50, 500, 6);
SELECT * FROM %ip ORDER BY a;
SELECT * FROM %ic ORDER BY a;
SELECT * FROM %ick ORDER BY a;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT a, c FROM %ic WHERE c = 30;
});
twins(
	"partitions' rows read as heap's after ADD and DROP COLUMN on their parent",
	q{
CREATE TABLE %pp (a int4, b text, c int4) PARTITION BY LIST (a) @AM;
CREATE TABLE %pp1 PARTITION OF %pp FOR VALUES IN (1, 2);
CREATE TABLE %pp2 PARTITION OF %pp FOR VALUES IN (3, 4);
CREATE INDEX %pp_c ON %pp (c);
INSERT INTO %pp VALUES (1, 'x', 10), (2, 'y', 20), (3, 'z', 30), (4, 'w', 40);
ALTER TABLE %pp DROP COLUMN b;
ALTER TABLE %pp ADD COLUMN d text DEFAULT 'dee';
INSERT INTO %pp VALUES (4, 41, 'mine');
SELECT tableoid::regclass, * FROM %pp ORDER BY a, c;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT a, c, d FROM %pp WHERE c = 30;
});

# A rewrite in the same statement, which reads the table's rows in its layout
# from before the statement, the columns it drops included: a type's change
# whose USING reads a column the statement drops, and a volatile default.
for my $key ('', 'PRIMARY KEY')
{
	my $kind = $key eq '' ? 'keyless' : 'keyed';
	twins(
		"a $kind table's DROP COLUMN and a type's change reading it in one statement read as heap's",
		qq{
CREATE TABLE %typed_$kind (a int4 $key, b text, c int4) \@AM;
INSERT INTO %typed_$kind VALUES (1, 'one', 10), (2, 'three', 20);
ALTER TABLE %typed_$kind DROP COLUMN b, ALTER COLUMN c TYPE int8 USING c * 2 + length(b);
SELECT * FROM %typed_$kind ORDER BY a;
});
	twins(
		"a $kind table's DROP COLUMN then a volatile default in one statement read as heap's",
		qq{
CREATE TABLE %volatile_$kind (a int4 $key, b text, c int4) \@AM;
INSERT INTO %volatile_$kind VALUES (1, 'one', 10), (2, 'two', 20);
ALTER TABLE %volatile_$kind DROP COLUMN b, ADD COLUMN d int4 DEFAULT seven();
SELECT * FROM %volatile_$kind ORDER BY a;
});
}

# The columns a dependency's cascade drops outside ALTER TABLE: a type's, a
# domain's, and an extension's, from a keyed and a keyless table with rows
# written before and after.
for my $key ('', 'PRIMARY KEY')
{
	my $kind = $key eq '' ? 'keyless' : 'keyed';
	twins(
		$kind eq 'keyed'
		? "a keyed table's columns DROP TYPE, DROP DOMAIN, and DROP EXTENSION cascade to read as heap's"
		: "a keyless table's columns DROP TYPE, DROP DOMAIN, and DROP EXTENSION cascade to read as heap's",
		qq{
CREATE TYPE %mood_$kind AS ENUM ('sad', 'happy');
CREATE DOMAIN %positive_$kind AS int4 CHECK (VALUE > 0);
CREATE EXTENSION citext;
CREATE TABLE %cascaded_$kind (a int4 $key, m %mood_$kind, p %positive_$kind, t citext, c text) \@AM;
CREATE INDEX %cascaded_${kind}_c ON %cascaded_$kind (c);
INSERT INTO %cascaded_$kind VALUES (1, 'sad', 5, 'Tee', 'one'), (2, 'happy', 6, 'tee', 'two');
DROP TYPE %mood_$kind CASCADE;
INSERT INTO %cascaded_$kind VALUES (3, 7, 'x', 'three');
DROP DOMAIN %positive_$kind CASCADE;
DROP EXTENSION citext CASCADE;
INSERT INTO %cascaded_$kind VALUES (4, 'four');
SELECT * FROM %cascaded_$kind ORDER BY a;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT a, c FROM %cascaded_$kind WHERE c = 'two';
});
}

# A transaction's own reads after its DROP COLUMN, a subtransaction's DROP
# COLUMN rolled back, and a REPEATABLE READ snapshot taken before a DROP COLUMN
# that commits, which reads the rows as they were.
twins(
	"a DROP COLUMN reads as heap's in its transaction and after a rolled-back subtransaction's",
	q{
CREATE TABLE %txn (a int4, b text, c int4) @AM;
INSERT INTO %txn VALUES (1, 'one', 10), (2, 'two', 20);
BEGIN;
ALTER TABLE %txn DROP COLUMN b;
INSERT INTO %txn VALUES (3, 30);
SELECT * FROM %txn ORDER BY a;
SAVEPOINT s;
ALTER TABLE %txn DROP COLUMN c;
SELECT * FROM %txn ORDER BY a;
ROLLBACK TO s;
SELECT * FROM %txn ORDER BY a;
COMMIT;
SELECT * FROM %txn ORDER BY a;
});
my %held;
for my $prefix ('', 'heap_')
{
	my $am = $prefix eq '' ? '' : 'USING doltgres_twin';
	$node->safe_psql('postgres',
		"CREATE TABLE ${prefix}snap (a int4, b text, c int4) $am; INSERT INTO ${prefix}snap VALUES (1, 'one', 10), (2, 'two', 20)");
	my $session = $node->background_psql('postgres');
	$session->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', "ALTER TABLE ${prefix}snap DROP COLUMN b; INSERT INTO ${prefix}snap VALUES (3, 30)");
	$held{$prefix} = $session->query_safe("SELECT * FROM ${prefix}snap ORDER BY a");
	$session->quit;
}
is($held{''}, $held{heap_}, 'a snapshot taken before a DROP COLUMN reads the rows as heap does');

# A crash after the DDLs replays the chains and the re-keyed rows.
my $all = q{
SELECT * FROM %t2_keyless ORDER BY a;
SELECT * FROM %t3_keyed ORDER BY a;
SELECT * FROM %ic ORDER BY a;
SELECT tableoid::regclass, * FROM %pp ORDER BY a, c;
SELECT * FROM %txn ORDER BY a;
SELECT * FROM %cascaded_keyless ORDER BY a;
};
my $before = twins('the altered tables read as heap\'s before a crash', $all);
$node->stop('immediate');
$node->start;
is(twins('the altered tables read as heap\'s after a crash', $all), $before, 'a crash keeps what the DDLs wrote');

# A row routed into a partition through its parent after a column was dropped in
# the middle, and then one at the end, is formed by the partition's own columns,
# though the parent's projection hands a partition that needs no conversion a
# slot showing each dropped column as a live typed NULL; a partition made after
# the drops, and an inheritance child written directly or through its parent,
# read as heap's too.
twins(
	"rows routed into partitions and written to an inheritance child after dropped columns read as heap's",
	q{
CREATE TABLE %rp (a int4, b text, c int4, d text) PARTITION BY LIST (a) @AM;
CREATE TABLE %rp1 PARTITION OF %rp FOR VALUES IN (1, 2);
CREATE TABLE %rp2 PARTITION OF %rp FOR VALUES IN (3, 4);
INSERT INTO %rp VALUES (1, 'b1', 10, 'd1');
ALTER TABLE %rp DROP COLUMN b;
INSERT INTO %rp VALUES (2, 20, 'd2'), (4, 40, 'd4');
ALTER TABLE %rp DROP COLUMN d;
INSERT INTO %rp VALUES (3, 30);
CREATE TABLE %rp3 PARTITION OF %rp FOR VALUES IN (5, 6);
INSERT INTO %rp VALUES (5, 50);
UPDATE %rp SET c = c + 1 WHERE a IN (1, 4);
UPDATE %rp SET a = 6 WHERE a = 2;
SELECT tableoid::regclass, * FROM %rp ORDER BY a;
CREATE TABLE %rq (a int4, b text, c int4, d text) @AM;
CREATE TABLE %rqc (e int4) INHERITS (%rq) @AM;
INSERT INTO %rqc VALUES (1, 'b1', 10, 'd1', 100);
ALTER TABLE %rq DROP COLUMN b;
INSERT INTO %rqc VALUES (2, 20, 'd2', 200);
ALTER TABLE %rq DROP COLUMN d;
INSERT INTO %rqc VALUES (3, 30, 300);
INSERT INTO %rq VALUES (4, 40);
UPDATE %rq SET c = c + 1 WHERE a IN (1, 3);
SELECT tableoid::regclass, * FROM %rq ORDER BY a;
});

# Bugs row 244: a row stored before ADD COLUMNs whose defaults the transform
# sets from its own bytes, a 1,500-byte one and three 16-byte ones beside a
# value out of line, reads as heap's, keyed and keyless, and a keyless table's
# rows fold and read as heap's again.
for my $key ('', 'PRIMARY KEY')
{
	my $kind = $key eq '' ? 'keyless' : 'keyed';
	my $folded = $kind eq 'keyless' ? qq{
\@FOLD(long_$kind)
SELECT k, v, length(w), md5(w) FROM %long_$kind ORDER BY k;
\@FOLD(sixteen_$kind)
SELECT k, length(v), a, b, c FROM %sixteen_$kind ORDER BY k;
} : '';
	twins(
		"a $kind table's rows read as heap's after ADD COLUMN defaults past the slot's slack",
		qq{
CREATE TABLE %long_$kind (k int4 $key, v text) \@AM;
INSERT INTO %long_$kind VALUES (1, 'a');
ALTER TABLE %long_$kind ADD COLUMN w text DEFAULT repeat('x', 1500);
SELECT k, v, length(w), md5(w) FROM %long_$kind ORDER BY k;
CREATE TABLE %sixteen_$kind (k int4 $key, v text) \@AM;
ALTER TABLE %sixteen_$kind ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO %sixteen_$kind VALUES (1, repeat('e', 1984));
ALTER TABLE %sixteen_$kind ADD COLUMN a text DEFAULT 'sixteen bytes!!!', ADD COLUMN b text DEFAULT 'sixteen bytes!!!', ADD COLUMN c text DEFAULT 'sixteen bytes!!!';
SELECT k, length(v), a, b, c FROM %sixteen_$kind ORDER BY k;
$folded});
}

done_testing();
