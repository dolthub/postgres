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

# Which relations get the fork's table access method routine (design item 9),
# through the test module's doltgres_test_table_routine, which asks the entry
# the relcache's call site calls: a user's tables, materialized views, and
# TOAST tables, whatever access method they name, and nothing initdb made; a
# relation made to rewrite a catalog keeps the routine its handler returns,
# by its pg_class row's relrewrite or, while it is being created, by what its
# creator said, which doltgres_test_relation_creating says as the creation's
# call site does.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('handler');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');

# A relation of every kind Postgres has, with tables of each persistence and
# one of another table access method.
$node->safe_psql(
	'postgres', q{
CREATE TABLE plain (k int4 PRIMARY KEY, t text);
CREATE UNLOGGED TABLE unlogged (k int4, t text);
CREATE ACCESS METHOD other_heap TYPE TABLE HANDLER heap_tableam_handler;
CREATE TABLE other (k int4, t text) USING other_heap;
CREATE MATERIALIZED VIEW matview AS SELECT k, t FROM plain;
CREATE SEQUENCE seq;
CREATE VIEW vw AS SELECT k FROM plain;
CREATE TYPE composite AS (a int4);
CREATE TABLE parted (k int4) PARTITION BY RANGE (k);
CREATE TABLE part PARTITION OF parted FOR VALUES FROM (0) TO (10);
CREATE INDEX parted_k ON parted (k);
CREATE FOREIGN DATA WRAPPER wrapper;
CREATE SERVER server FOREIGN DATA WRAPPER wrapper;
CREATE FOREIGN TABLE foreign_table (k int4) SERVER server;
});

# asked returns each of a query's relations with the entry's answer for it.
sub asked
{
	my ($where) = @_;
	return $node->safe_psql('postgres',
		"SELECT relname, doltgres_test_table_routine(oid) FROM pg_class WHERE $where ORDER BY relname COLLATE \"C\""
	);
}

is( $node->safe_psql(
		'postgres', q{
SELECT relkind, bool_and(doltgres_test_table_routine(oid)), bool_or(doltgres_test_table_routine(oid))
FROM pg_class WHERE oid >= 16384 GROUP BY relkind ORDER BY relkind::text COLLATE "C"}),
	"I|f|f\nS|f|f\nc|f|f\nf|f|f\ni|f|f\nm|t|t\np|f|f\nr|t|t\nt|t|t\nv|f|f",
	"of a user's relations of every kind, the tables, materialized views, and TOAST tables are the fork's"
);
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg(kind, '' ORDER BY kind) FROM
		  (SELECT DISTINCT relkind::text COLLATE "C" AS kind FROM pg_class) AS kinds}),
	'IScfimprtv',
	'the kinds asked are every kind this Postgres has');
is( asked(
		q{relname IN ('plain', 'unlogged', 'other', 'part', 'matview') OR oid IN
		  (SELECT reltoastrelid FROM pg_class WHERE relname IN ('plain', 'other', 'matview'))}
	) =~ s/pg_toast_[0-9]+/toast/gr,
	"matview|t\nother|t\npart|t\ntoast|t\ntoast|t\ntoast|t\nplain|t\nunlogged|t",
	'the tables of each persistence and access method, a partition, and their TOAST tables are the fork\'s'
);
is( $node->safe_psql(
		'postgres', q{
CREATE TEMPORARY TABLE temporary (k int4, t text);
SELECT doltgres_test_table_routine('temporary'),
	doltgres_test_table_routine((SELECT reltoastrelid FROM pg_class WHERE oid = 'temporary'::regclass))}),
	't|t',
	'a temporary table and its TOAST table are the fork\'s');
like(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*), count(*) FILTER (WHERE doltgres_test_table_routine(oid)) FROM pg_class
		  WHERE oid < 16384 AND relkind IN ('r', 't', 'm')}),
	qr/^[1-9][0-9]*\|0$/,
	'no table or TOAST table initdb made is the fork\'s, a catalog\'s or the information schema\'s');

# A relation made to rewrite another says so in its pg_class row: Postgres'
# make_new_heap sets it on the table a rewrite fills and on that table's TOAST
# table, whose files a catalog's rewrite swaps with the catalog's.
$node->safe_psql(
	'postgres', q{
UPDATE pg_class SET relrewrite = 'pg_proc'::regclass WHERE relname = 'plain';
UPDATE pg_class SET relrewrite = (SELECT reltoastrelid FROM pg_class WHERE relname = 'pg_proc')
	WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'other');
UPDATE pg_class SET relrewrite = 'other'::regclass WHERE relname = 'unlogged';
});
is( asked(
		q{relname IN ('plain', 'unlogged', 'other') OR oid =
		  (SELECT reltoastrelid FROM pg_class WHERE relname = 'other')}
	) =~ s/pg_toast_[0-9]+/toast/r,
	"other|t\ntoast|f\nplain|f\nunlogged|t",
	'a relation made to rewrite a catalog keeps the routine its handler returns'
);
$node->safe_psql('postgres',
	q{UPDATE pg_class SET relrewrite = 0 WHERE relrewrite <> 0 AND oid >= 16384});
is(asked(q{relname = 'plain'}),
	'plain|t', 'a relation whose rewrite ended is the fork\'s again');

# While a relation is being created its entry doesn't hold what it rewrites,
# and the creation's call site says it: for that relation alone, in the
# transaction or subtransaction that creates it alone, until the next
# creation; and the entry's own word comes first once it is set.
my $creating =
  q{DO $$ BEGIN PERFORM doltgres_test_relation_creating('plain'::regclass, 'pg_class'::regclass); END $$};
my $next =
  q{DO $$ BEGIN PERFORM doltgres_test_relation_creating('other'::regclass, 0); END $$};
my $both =
  q{SELECT doltgres_test_table_routine('plain'), doltgres_test_table_routine('other')};
is( $node->safe_psql('postgres', "BEGIN; $creating; $both; COMMIT"),
	'f|t',
	'a relation being created answers by what its creator said it rewrites, in that transaction level alone'
);
is($node->safe_psql('postgres', "$creating; $both"),
	't|t', 'a creation speaks for nothing once its transaction ended');

# A creation that fails after its call site aborts the transaction or
# subtransaction that made it, so its record speaks for no other relation once
# that level ends; the case below, a subtransaction rolled back after its
# creation's record, is the design item 9 case of a failed creation.
is( $node->safe_psql(
		'postgres',
		"BEGIN; SAVEPOINT s; $creating; ROLLBACK TO s; $both; SAVEPOINT s; $creating; RELEASE s; $both; COMMIT"
	),
	"t|t\nt|t",
	'a creation speaks for nothing once its subtransaction ended');
is( $node->safe_psql(
		'postgres',
		"BEGIN; $creating; $next; $both; COMMIT"
	),
	't|t',
	'the next creation takes the place of the one before it');
is( $node->safe_psql(
		'postgres', qq{
UPDATE pg_class SET relrewrite = 'other'::regclass WHERE relname = 'plain';
BEGIN; $creating; $both; COMMIT}),
	't|t',
	'what a relation\'s entry says it rewrites comes before what its creation said'
);

# A fork table has the TOAST relation heap gives it, of the access method
# heap's routine names, as on Postgres 19.
is( $node->safe_psql(
		'postgres', q{
CREATE TABLE narrow (k int4, c varchar(10));
CREATE TABLE wide (k int4, t text);
CREATE TABLE plain_only (k int4, t text STORAGE PLAIN);
CREATE TABLE other_wide (k int4, t text) USING other_heap;
SELECT c.relname, c.reltoastrelid <> 0, coalesce(a.amname, '') FROM pg_class c
	LEFT JOIN pg_class t ON t.oid = c.reltoastrelid LEFT JOIN pg_am a ON a.oid = t.relam
	WHERE c.relname IN ('narrow', 'wide', 'plain_only', 'other_wide') ORDER BY c.relname COLLATE "C"}),
	"narrow|f|\nother_wide|t|other_heap\nplain_only|f|\nwide|t|heap",
	'a fork table has the TOAST relation heap gives it, of the access method heap\'s names'
);

# rewritten runs a rewrite under the test module's watcher and returns what it
# saw of the relations made for it, at their creation and at the swap.
sub rewritten
{
	my ($rewrite) = @_;
	return $node->safe_psql('postgres',
		"DO \$\$ BEGIN PERFORM doltgres_test_watch_rewrites(); END \$\$; $rewrite; SELECT rtrim(doltgres_test_rewrites_seen(), E'\\n')"
	);
}

# The relations a catalog's VACUUM FULL makes keep Postgres' code from their
# creation through the swap, for a catalog with a TOAST table, one without,
# a mapped one, and the information schema's, and each reads back the same.
# pg_class's rows of the relations VACUUM FULL gives new files change, so its
# rows are read without their file and size columns.
my %rows = (
	'pg_proc' => 'c::text',
	'pg_am' => 'c::text',
	'pg_class' => 'c.oid::text || c.relname::text || c.relkind::text',
	'information_schema.sql_features' => 'c::text');
foreach my $catalog (sort keys %rows)
{
	my $read =
	  "SELECT count(*), sum(hashtext($rows{$catalog})) FROM $catalog AS c";
	my $before = $node->safe_psql('postgres', $read);
	unlike(rewritten("VACUUM FULL $catalog"),
		qr/fork/, "the relations VACUUM FULL $catalog makes keep Postgres' code");
	is($node->safe_psql('postgres', $read),
		$before, "$catalog reads back the same after its VACUUM FULL");
}
is( rewritten('VACUUM FULL pg_proc'),
	"made r postgres\nmade t postgres\nswapped r postgres\nswapped t postgres\nswapped i postgres\nswapped t postgres",
	'the transient table, its TOAST table, and that table\'s index are each seen');

# A user's table named as a catalog's transient table is, and its rewrite,
# stay the fork's, since no name decides which code stores a relation; a TOAST
# table's index is unique, so it keeps nbtree's routine (index::ported).
my $proc = $node->safe_psql('postgres', q{SELECT 'pg_proc'::regclass::oid});
$node->safe_psql('postgres',
	qq{CREATE TABLE pg_temp_$proc (k int4, t text); INSERT INTO pg_temp_$proc VALUES (1, 'one')});
is($node->safe_psql('postgres', "SELECT doltgres_test_table_routine('pg_temp_$proc')"),
	't', 'a user\'s table named as a catalog\'s transient table is the fork\'s');
unlike(rewritten("VACUUM FULL pg_temp_$proc"),
	qr/[rt] postgres/, 'the relations its rewrite makes are the fork\'s');
is($node->safe_psql('postgres', "SELECT k, t FROM pg_temp_$proc"),
	'1|one', 'it reads back after its rewrite');

# A user table's rewrite by links gives it a new TOAST table, the fork's before
# and after its relrewrite is reset.
$node->safe_psql('postgres',
	q{CREATE TABLE linked (k int4, t text); INSERT INTO linked VALUES (1, repeat('x', 10000))});
unlike(rewritten('ALTER TABLE linked ALTER COLUMN k TYPE int8'),
	qr/[rt] postgres/, 'the relations a rewrite by links makes are the fork\'s');
is( $node->safe_psql(
		'postgres', q{
SELECT doltgres_test_table_routine(t.oid), t.relrewrite FROM pg_class c JOIN pg_class t ON t.oid = c.reltoastrelid
	WHERE c.relname = 'linked'}),
	't|0',
	'its new TOAST table is the fork\'s once its relrewrite is reset');
is($node->safe_psql('postgres', 'SELECT k, length(t) FROM linked'),
	'1|10000', 'it reads back after its rewrite');

# The test module's heap twins keep heap's routine, and their indexes nbtree's,
# so the fork's tests can compare its storage with heap's: a twin's rows are in
# its own heap file. A fork table beside one, and a user's table USING heap,
# are the fork's, after a reconnect and a restart too.
$node->safe_psql(
	'postgres', q{
CREATE TABLE twinned (k int4, t text) USING doltgres_twin;
CREATE INDEX twinned_k ON twinned (k);
CREATE TABLE beside (k int4, t text);
CREATE INDEX beside_k ON beside (k);
CREATE TABLE heap_named (k int4, t text) USING heap;
INSERT INTO twinned VALUES (1, 'one');
});
my $twins = q{
SELECT relname, doltgres_test_table_routine(oid) FROM pg_class
	WHERE relname IN ('twinned', 'beside', 'heap_named')
	OR oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'twinned')
	ORDER BY relname COLLATE "C";
SELECT doltgres_test_index_routine('twinned_k'), doltgres_test_index_routine('beside_k');
SELECT ctid, k, t, pg_relation_size('twinned') FROM twinned};
my $expected =
  "beside|t\nheap_named|t\ntoast|f\ntwinned|f\nf|t\n(0,1)|1|one|8192";
is($node->safe_psql('postgres', $twins) =~ s/pg_toast_[0-9]+/toast/r,
	$expected,
	'a heap twin and its TOAST table keep heap\'s routine and its index nbtree\'s, while a fork table and a table USING heap are the fork\'s'
);
$node->restart;
is($node->safe_psql('postgres', $twins) =~ s/pg_toast_[0-9]+/toast/r,
	$expected, 'and so after a restart');

$node->stop;
done_testing();
