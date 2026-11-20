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


# The identity pairs of PLAN design item 10: a table's creation records its own
# and its columns' object ids under its schema's, which the schema's creation or
# its repository's records, an index's build its own under its table's, and a
# new TOAST relation its pair under its table's, derived from their kinds,
# parents, and names, in the creating transaction, so no writer derives or waits;
# DROP TABLE and DROP INDEX remove them; and ALTER TABLE's door keeps the
# columns' pairs. doltgres_test_identity reads the id the database's identity
# tree pairs with an object's number.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('identity');
$node->init(allows_streaming => 1);
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE DATABASE other');
for my $db ('postgres', 'other')
{
	$node->safe_psql($db, 'CREATE EXTENSION injection_points');
	$node->safe_psql($db, 'CREATE EXTENSION doltgres_test');
}

# oid returns a relation's OID in a database.
sub oid
{
	my ($db, $rel) = @_;
	return $node->safe_psql($db, "SELECT '$rel'::regclass::oid");
}

# id returns the id a database's identity tree pairs with a schema's OID (kind
# 1), a relation's (kind 2), or a column's attribute number under a table's
# OID (kind 3), or "none".
sub id
{
	my ($db, $kind, $number, $attnum) = @_;
	return $node->safe_psql($db, "SELECT doltgres_test_identity($kind, $number, " . ($attnum // 0) . ')');
}

# column returns the id of a table's column by its name.
sub column
{
	my ($db, $table, $name) = @_;
	my $attnum = $node->safe_psql($db,
		"SELECT attnum FROM pg_attribute WHERE attrelid = '$table'::regclass AND attname = '$name'");
	return id($db, 3, oid($db, $table), $attnum);
}

# forked runs a statement over a table through the fork's routine.
sub forked
{
	my ($db, $table, $sql) = @_;
	$sql =~ s/'/''/g;
	$node->safe_psql($db, "SELECT doltgres_test_write('$table', true, '$sql')");
}

my $public = $node->safe_psql('postgres', "SELECT 'public'::regnamespace::oid");

$node->safe_psql($_, 'CREATE TABLE t (a int4 PRIMARY KEY, b text)') for 'postgres', 'other';
my %ids;
for my $db ('postgres', 'other')
{
	my $public = $node->safe_psql($db, "SELECT 'public'::regnamespace::oid");
	$ids{$db} = [ id($db, 1, $public), id($db, 2, oid($db, 't')), column($db, 't', 'a'), column($db, 't', 'b') ];
}
ok(!grep({ $_ eq 'none' } @{ $ids{postgres} }), "a CREATE TABLE records its schema's, its own, and its columns' pairs");
is_deeply($ids{other}, $ids{postgres}, 'the same CREATE in two databases derives the same ids');
my %distinct = map { $_ => 1 } @{ $ids{postgres} };
is(scalar(keys %distinct), 4, "a schema's, a table's, and its columns' ids differ");
my $t = $ids{postgres}[1];

my ($r, $inside) = split /\n/, $node->safe_psql('postgres',
	"BEGIN; CREATE TABLE r (a int4 PRIMARY KEY); SELECT 'r'::regclass::oid; SELECT doltgres_test_identity(2, 'r'::regclass::oid, 0); ROLLBACK;");
ok($inside ne 'none' && id('postgres', 2, $r) eq 'none',
	"a CREATE TABLE's pair reads in its transaction, and a rolled back one leaves none");

my %toast = map {
	$_ => id($_, 2, $node->safe_psql($_, "SELECT reltoastrelid FROM pg_class WHERE oid = 't'::regclass"))
} 'postgres', 'other';
ok($toast{postgres} ne 'none' && $toast{postgres} ne $t, "a CREATE TABLE records its TOAST relation's pair");
is($toast{other}, $toast{postgres}, 'the same CREATE in two databases derives the same TOAST id');

$node->safe_psql('postgres', 'CREATE INDEX t_b ON t (a)');
my $t_b = oid('postgres', 't_b');
my $index = id('postgres', 2, $t_b);
ok($index ne 'none' && $index ne $t, "a CREATE INDEX records its pair");
$node->safe_psql('postgres', 'DROP INDEX t_b');
is(id('postgres', 2, $t_b), 'none', "a DROP INDEX removes the index's pair");

$node->safe_psql('postgres', 'ALTER TABLE t RENAME TO t_renamed; TRUNCATE t_renamed');
forked('postgres', 't_renamed', "INSERT INTO t_renamed VALUES (3, 'z')");
is(id('postgres', 2, oid('postgres', 't_renamed')), $t, 'a RENAME and a TRUNCATE keep the id');
my $old = oid('postgres', 't_renamed');
$node->safe_psql('postgres', 'DROP TABLE t_renamed');
is(id('postgres', 2, $old), 'none', "a DROP TABLE removes the table's pair");
is(id('postgres', 3, $old, 1), 'none', "a DROP TABLE removes its columns' pairs");
$node->safe_psql('postgres', 'CREATE TABLE t (a int4 PRIMARY KEY, b text)');
forked('postgres', 't', "INSERT INTO t VALUES (1, 'x')");
my $again = oid('postgres', 't');
ok($again ne $old && id('postgres', 2, $again) eq $t, 'a table made again after its DROP derives its id again');

$node->safe_psql('postgres', 'CREATE SCHEMA s; CREATE TABLE s.x (a int4 PRIMARY KEY)');
forked('postgres', 's.x', 'INSERT INTO s.x VALUES (1)');
my $s = $node->safe_psql('postgres', "SELECT 's'::regnamespace::oid");
my $schema = id('postgres', 1, $s);
$node->safe_psql('postgres', 'BEGIN; DROP SCHEMA s CASCADE; ROLLBACK');
is(id('postgres', 1, $s), $schema, "a rolled back DROP SCHEMA keeps the schema's pair");
$node->safe_psql('postgres', 'DROP SCHEMA s CASCADE');
is(id('postgres', 1, $s), 'none', "a DROP SCHEMA removes the schema's pair");
$node->safe_psql('postgres', 'CREATE SCHEMA s; CREATE TABLE s.x (a int4 PRIMARY KEY)');
forked('postgres', 's.x', 'INSERT INTO s.x VALUES (1)');
my $again_s = $node->safe_psql('postgres', "SELECT 's'::regnamespace::oid");
ok($again_s ne $s && id('postgres', 1, $again_s) eq $schema, 'a schema made again after its DROP derives its id again');

my %columns;
for my $db ('postgres', 'other')
{
	$node->safe_psql($db, 'CREATE TABLE c (a int4 PRIMARY KEY, b int4, d text)');
	forked($db, 'c', 'INSERT INTO c VALUES (1, 2, 3)');
	$columns{$db}{b} = column($db, 'c', 'b');
	$columns{$db}{d} = column($db, 'c', 'd');
	$node->safe_psql($db, 'ALTER TABLE c ADD COLUMN e int4');
	$columns{$db}{e} = column($db, 'c', 'e');
}
ok($columns{postgres}{e} ne 'none', "an ADD COLUMN records the new column's pair");
is($columns{other}{e}, $columns{postgres}{e}, 'the same ADD COLUMN in two databases derives the same id');
$node->safe_psql('postgres', 'ALTER TABLE c RENAME COLUMN b TO bb');
is(column('postgres', 'c', 'bb'), $columns{postgres}{b}, 'a RENAME COLUMN keeps the id');
$node->safe_psql('postgres', 'ALTER TABLE c ALTER COLUMN bb TYPE text');
is(column('postgres', 'c', 'bb'), $columns{postgres}{b}, "an ALTER COLUMN TYPE to another category keeps the id");
$node->safe_psql('postgres', 'ALTER TABLE c DROP COLUMN d');
is(id('postgres', 3, oid('postgres', 'c'), 3), $columns{postgres}{d}, "a DROP COLUMN keeps the column's pair");
$node->safe_psql('postgres', 'ALTER TABLE c ADD COLUMN d text');
my $d = column('postgres', 'c', 'd');
ok($d ne 'none' && $d ne $columns{postgres}{d},
	"a column added under a dropped one's name and category takes another id");

$node->safe_psql(
	'postgres', q{
CREATE TABLE p (a int4, b text) PARTITION BY RANGE (a);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10);
CREATE TABLE parent (a int4);
CREATE TABLE child () INHERITS (parent);
CREATE TABLE merged (c int4) INHERITS (parent);
});
forked('postgres', 'p1', "INSERT INTO p1 VALUES (1, 'x')");
forked('postgres', $_, 'INSERT INTO ' . $_ . ' (a) VALUES (1)') for 'child', 'merged';
my $merged = column('postgres', 'merged', 'c');
$node->safe_psql('postgres', 'ALTER TABLE p ADD COLUMN c int4; ALTER TABLE parent ADD COLUMN c int4');
ok(column('postgres', 'p1', 'c') ne 'none', "an ADD COLUMN on a partitioned parent pairs the partition's new column");
ok(column('postgres', 'child', 'c') ne 'none', "an ADD COLUMN on an inheritance parent pairs the child's new column");
is(column('postgres', 'merged', 'c'), $merged, "a child that already had the column keeps its pair");

{
	my $first = $node->background_psql('postgres', on_error_stop => 0);
	$first->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-identity-probed', 'wait')");
	$first->query_until(qr/started/, "\\echo started\nCREATE TABLE race (a int4 PRIMARY KEY);\n");
	$node->wait_for_event('client backend', 'doltgres-identity-probed');
	$node->safe_psql('postgres', "SET lock_timeout = '10s'; CREATE TABLE racer (a int4 PRIMARY KEY); INSERT INTO racer VALUES (1)");
	my $racer = id('postgres', 2, oid('postgres', 'racer'));
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-identity-probed'); SELECT injection_points_wakeup('doltgres-identity-probed')");
	$first->query_safe('SELECT 1');
	$first->quit;
	my $race = id('postgres', 2, oid('postgres', 'race'));
	ok($racer ne 'none' && $race ne 'none' && $race ne $racer,
		"a CREATE TABLE paused between its probe and its insert holds up no other table's creation and first write");
}

$node->backup('identity_backup');
my $standby = PostgreSQL::Test::Cluster->new('identity_standby');
$standby->init_from_backup($node, 'identity_backup', has_streaming => 1);
$standby->start;
$node->safe_psql('postgres', 'CREATE TABLE replayed (a int4 PRIMARY KEY, b text)');
forked('postgres', 'replayed', "INSERT INTO replayed VALUES (1, 'x')");
$node->wait_for_replay_catchup($standby);
my $replayed = oid('postgres', 'replayed');
is($standby->safe_psql('postgres', "SELECT doltgres_test_identity(2, $replayed, 0)"),
	id('postgres', 2, $replayed), 'a standby replays the pairs with no catalog access');
$standby->stop;

my @kept = (id('postgres', 2, oid('postgres', 'c')), column('postgres', 'c', 'e'));
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'CREATE TABLE after (a int4 PRIMARY KEY)');
forked('postgres', 'after', 'INSERT INTO after VALUES (1)');
my $after = id('postgres', 2, oid('postgres', 'after'));
$node->stop('immediate');
$node->start;
is_deeply([ id('postgres', 2, oid('postgres', 'c')), column('postgres', 'c', 'e') ],
	\@kept, "a start loads the pairs a checkpoint saved");
is(id('postgres', 2, oid('postgres', 'after')), $after, "a crash's recovery replays the pairs written after the checkpoint");

my @hooked = split /\n/, $node->safe_psql(
	'postgres', q{
SELECT doltgres_test_object_access_replaced();
CREATE TABLE hooked (a int4 PRIMARY KEY);
SELECT doltgres_test_write('hooked', true, 'INSERT INTO hooked VALUES (1)');
SELECT doltgres_test_identity(2, 'hooked'::regclass::oid, 0) <> 'none';
SELECT 'hooked'::regclass::oid AS hooked \gset
DROP TABLE hooked;
SELECT doltgres_test_identity(2, :hooked, 0);
});
is("@hooked[-3 .. -1]", '1 t none',
	"an object access hook that replaces the one before it without chaining leaves the pairs recorded and removed");

# Bugs row 159: a full collection marks the manifests' identity trees as roots,
# so once a table's pairs are saved, a collection keeps the identity tree's
# chunks: the next checkpoint passes, and the pairs read, after a restart too.
$node->safe_psql('postgres', 'CREATE TABLE collected (a int4 PRIMARY KEY)');
forked('postgres', 'collected', 'INSERT INTO collected VALUES (1)');
$node->safe_psql('postgres', 'CHECKPOINT');
my $collected = id('postgres', 2, oid('postgres', 'collected'));
like($node->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	qr/^\d+ \d+ \d+$/, 'a full collection after the identity tree is saved completes');
my ($checkpointed) = $node->psql('postgres', 'CHECKPOINT');
is($checkpointed, 0, "the checkpoint after the collection finds every chunk of the identity tree");
is(id('postgres', 2, oid('postgres', 'collected')), $collected, "the identity tree's pairs read after the collection");
$node->restart;
is(id('postgres', 2, oid('postgres', 'collected')), $collected, 'and after a restart');


# Bugs row 164: a checkpoint's save drops the identity versions it folded,
# giving their counts back at their writer's entry. That checkpoint's run
# leaves them out, so a crash after it loads none of them back and the
# writer's entry is gone, so the count is exact at each step; the shutdown
# checkpoint after it writes no identity run, and the pairs read from the
# manifest after the restart.
my $pairing = (split /\n/,
	  $node->safe_psql(
		'postgres',
		q{BEGIN; CREATE TABLE unfolded (a int4 PRIMARY KEY); SELECT txid_current(); COMMIT}
	  ))[-1];
$node->safe_psql('postgres', 'INSERT INTO unfolded VALUES (1)');
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('unfolded')});
my $counted = "SELECT doltgres_test_status_table(1, $pairing, 0)";
my $pairs = $node->safe_psql('postgres', $counted);
ok($pairs > 0, "the DDL writer's entry counts its identity pairs");
$node->safe_psql('postgres', 'CHECKPOINT');
is($node->safe_psql('postgres', $counted), '0', "a checkpoint's save drops them and gives their counts back");
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $counted),
	'-1', "a crash's start loads none of them back, since the checkpoint's run left them out, and their writer's entry is gone");
my $unfolded = id('postgres', 2, oid('postgres', 'unfolded'));
isnt($unfolded, 'none', 'the dropped pairs read from the manifest');
my $runs = $node->data_dir . '/pg_versioned/' . $node->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = 'postgres'") . '/delta';
$node->stop;
my ($newest) = reverse sort map { s{.*/}{}r } glob("$runs/*");
ok(!-e "$runs/$newest/identity", 'the shutdown checkpoint after the drop writes no identity run');
$node->start;
is(id('postgres', 2, oid('postgres', 'unfolded')), $unfolded, 'and after the restart');

# CommPF's ruling (a): no writer derives a schema's pair. A new database's
# repository holds its initial schemas' pairs before any write, as the same ids
# in every database; CREATE SCHEMA records its schema's pair in its own
# transaction, which a rollback takes back; and two transactions' first writes
# to new tables in one schema don't wait on each other.
$node->safe_psql('postgres', 'CREATE DATABASE seeded');
$node->safe_psql('seeded', 'CREATE EXTENSION doltgres_test');
my @initial = map { id('seeded', 1, $_) } 11, 99, 2200;
ok(!grep({ $_ eq 'none' } @initial), "a new database's repository holds its initial schemas' pairs before any write");
is_deeply(\@initial, [ map { id('postgres', 1, $_) } 11, 99, 2200 ], 'as the same ids in every database');
my $rolled = $node->safe_psql('postgres',
	"BEGIN; CREATE SCHEMA rolled; SELECT 'rolled'::regnamespace::oid; ROLLBACK");
$node->safe_psql('postgres', 'CREATE SCHEMA fresh');
my $fresh = $node->safe_psql('postgres', "SELECT 'fresh'::regnamespace::oid");
ok(id('postgres', 1, $fresh) ne 'none' && id('postgres', 1, $rolled) eq 'none',
	'a CREATE SCHEMA records its pair, and a rolled back one leaves none');
my @writers = map { $node->background_psql('postgres') } 1 .. 2;
for my $at (0, 1)
{
	my $table = "fresh.t$at";
	$writers[$at]->query_safe("SET lock_timeout = '10s'");
	$writers[$at]->query_safe(
		"BEGIN; CREATE TABLE $table (a int4 PRIMARY KEY); SELECT doltgres_test_write('$table', true, 'INSERT INTO $table VALUES (1)')"
	);
}
$_->query_safe('COMMIT') for @writers;
$_->quit for @writers;
ok(!grep({ id('postgres', 2, oid('postgres', "fresh.t$_")) eq 'none' } 0, 1),
	"two transactions' first writes to new tables in one schema don't wait on each other");

# A temporary table belongs to no branch: its creation records no pair, its
# index's neither, and the backend's temporary schema has none. A fresh
# cluster's template1 holds a pair for each of its schemas: bootstrap's with its
# repository, and information_schema's from its CREATE SCHEMA in initdb's
# post-bootstrap SQL, which runs once template1's repository is made.
my ($temp, $temp_schema) = split / /, (split /\n/, $node->safe_psql('postgres', q{
CREATE TEMP TABLE tt (a int4 PRIMARY KEY);
SELECT doltgres_test_write('tt', true, 'INSERT INTO tt VALUES (1)');
SELECT 'tt'::regclass::oid || ' ' || pg_my_temp_schema()::oid;
}))[-1];
ok(id('postgres', 2, $temp) eq 'none' && id('postgres', 1, $temp_schema) eq 'none',
	"a temporary table records no pair, and its schema has none");
$node->safe_psql('template1', 'CREATE EXTENSION doltgres_test');
is($node->safe_psql('template1',
		q{SELECT coalesce(string_agg(nspname, ','), '') FROM pg_namespace WHERE doltgres_test_identity(1, oid, 0) = 'none'}),
	'', "a fresh cluster's template1 holds a pair for each of its schemas");
$node->safe_psql('template1', 'DROP EXTENSION doltgres_test');

# A checkpoint's save folds a CREATE SCHEMA's pairs into the manifest and leaves
# them out of its identity run, so a restart loads none of them back and a fresh
# cluster's delta holds what it held before the schema.
my $refolded = PostgreSQL::Test::Cluster->new('refolded');
$refolded->init;
$refolded->start;
$refolded->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
my $used = 'SELECT doltgres_test_delta(0, 0, 0)';
my $fresh_used = $refolded->safe_psql('postgres', $used);
$refolded->safe_psql('postgres', 'CREATE SCHEMA refolded');
$refolded->safe_psql('postgres', 'CHECKPOINT');
$refolded->restart;
is($refolded->safe_psql('postgres', $used), $fresh_used,
	"a restart after a CHECKPOINT folded a schema's pairs loads none of them back");
isnt($refolded->safe_psql('postgres', q{SELECT doltgres_test_identity(1, 'refolded'::regnamespace::oid, 0)}),
	'none', 'and the pair reads from the manifest');
$refolded->stop;

# Two sessions' first inserts into one table: the second completes while the
# first's transaction is open, as on heap, since neither records a pair. A TOAST
# relation's pair reads in the transaction that made it, a CREATE TABLE's and an
# ALTER TABLE's that adds the first varlena column, and a rewrite moves the
# table's TOAST pair to its new TOAST relation.
$node->safe_psql('postgres', 'CREATE TABLE shared (a int4)');
{
	my $holder = $node->background_psql('postgres');
	$holder->query_safe('BEGIN; INSERT INTO shared VALUES (1)');
	is($node->safe_psql('postgres', "SET lock_timeout = '10s'; INSERT INTO shared VALUES (2); SELECT count(*) FROM shared"),
		'1', "a table's second first writer completes while the first's transaction is open");
	$holder->query_safe('COMMIT');
	$holder->quit;
}
my $toasted = $node->safe_psql('postgres', q{
BEGIN;
CREATE TABLE tv (a int4, b text);
SELECT doltgres_test_identity(2, reltoastrelid, 0) FROM pg_class WHERE oid = 'tv'::regclass;
COMMIT;
});
ok($toasted ne 'none', "a CREATE TABLE's TOAST pair reads in its transaction");
$node->safe_psql('postgres', 'CREATE TABLE nv (a int4)');
my $added = $node->safe_psql('postgres', q{
BEGIN;
ALTER TABLE nv ADD COLUMN b text;
SELECT doltgres_test_identity(2, reltoastrelid, 0) FROM pg_class WHERE oid = 'nv'::regclass;
COMMIT;
});
ok($added ne 'none', "an ALTER TABLE that adds the first varlena column records the TOAST pair in its transaction");
{
	my $holder = $node->background_psql('postgres');
	$holder->query_safe("BEGIN; INSERT INTO nv VALUES (1, 'x')");
	is($node->safe_psql('postgres', "SET lock_timeout = '10s'; INSERT INTO nv VALUES (2, 'y'); SELECT count(*) FROM nv"),
		'1', "a later first write to a table with a TOAST relation waits on no other");
	$holder->query_safe('COMMIT');
	$holder->quit;
}
my $old_toast = $node->safe_psql('postgres', "SELECT reltoastrelid FROM pg_class WHERE oid = 'nv'::regclass");
$node->safe_psql('postgres', 'ALTER TABLE nv ALTER COLUMN a TYPE int8');
my $new_toast = $node->safe_psql('postgres', "SELECT reltoastrelid FROM pg_class WHERE oid = 'nv'::regclass");
ok($new_toast ne $old_toast && id('postgres', 2, $new_toast) eq $added && id('postgres', 2, $old_toast) eq 'none',
	"a rewrite moves the table's TOAST pair to its new TOAST relation");

$node->stop;
done_testing();
