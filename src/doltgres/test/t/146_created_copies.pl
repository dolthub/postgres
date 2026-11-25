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

# CREATE DATABASE ... TEMPLATE copies the template's rows (bugs row 404): the
# new database's repository is a copy of the template's bases with its
# committed delta written as the copy's runs, by either strategy, with or
# without a fold, through a checkpoint, a crash after the commit, and a
# standby's replay; a creation whose transaction never commits leaves nothing
# behind, whether it aborts or the server crashes before its commit, and a
# crash between the commit and the clearing of the creation's mark keeps the
# copy. Before, the new database read none of the template's rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
	"checkpoint_timeout = 1h\nfold_threshold = 1GB\nfold_timeout = 3600s\nallow_in_place_tablespaces = on"
);
$node->start;
my $versioned = $node->data_dir . '/pg_versioned';
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# oid returns a database's id.
sub oid
{
	my ($name) = @_;
	return $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = '$name'");
}

# quiet waits until no session is connected to a database, a fold worker's
# included, since CREATE DATABASE from it and DROP DATABASE of it refuse one
# where stock ends an autovacuum worker (bugs row 420).
sub quiet
{
	my ($cluster, $database) = @_;
	$cluster->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE datname = '$database'")
	  or die "a session stayed connected to $database";
}

# The template: an indexed table with a TOASTed column, its rows committed
# and not folded.
quiet($node, 'template1');
$node->safe_psql('postgres', 'CREATE DATABASE filled');
$node->safe_psql(
	'filled', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE t (k int4 PRIMARY KEY, v text);
CREATE INDEX t_v ON t (v);
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 100) g;
INSERT INTO t VALUES (101, repeat(md5('wide'), 200));
});
my $sum = $node->safe_psql('filled', 'SELECT count(*), sum(length(v)) FROM t');

# A copy by each strategy reads every row of the template, through its index
# too.
for my $strategy ('wal_log', 'file_copy')
{
	quiet($node, 'filled');
	$node->safe_psql('postgres',
		"CREATE DATABASE copy_$strategy TEMPLATE filled STRATEGY $strategy");
	is($node->safe_psql("copy_$strategy", 'SELECT count(*), sum(length(v)) FROM t'),
		$sum, "a copy by $strategy reads the template's unfolded rows");
	is( $node->safe_psql(
			"copy_$strategy",
			"SET enable_seqscan = off; SELECT k FROM t WHERE v = 'r42'"),
		'42',
		"a copy by $strategy finds a row through the template's index");
}

# A copy made after a fold of the template reads the folded rows and the
# delta's, and a write to the copy leaves the template as it was.
$node->safe_psql('filled',
	"SELECT doltgres_test_fold('t'); INSERT INTO t VALUES (102, 'after')");
quiet($node, 'filled');
$node->safe_psql('postgres', 'CREATE DATABASE folded TEMPLATE filled');
is($node->safe_psql('folded', 'SELECT count(*) FROM t'),
	'102', "a copy after the template's fold reads its folded and unfolded rows");
$node->safe_psql('folded', 'DELETE FROM t WHERE k <= 50');
is($node->safe_psql('filled', 'SELECT count(*) FROM t'),
	'102', "a write to the copy leaves the template's rows");

# A checkpoint before the copy's first use keeps its rows, as does a crash
# after the creation's commit.
quiet($node, 'filled');
$node->safe_psql('postgres', 'CREATE DATABASE checkpointed TEMPLATE filled');
$node->safe_psql('postgres', 'CHECKPOINT');
is($node->safe_psql('checkpointed', 'SELECT count(*) FROM t'),
	'102', 'a checkpoint between the creation and its first use keeps its rows');
quiet($node, 'filled');
$node->safe_psql('postgres', 'CREATE DATABASE crashed TEMPLATE filled');
$node->stop('immediate');
$node->start;
is($node->safe_psql('crashed', 'SELECT count(*) FROM t'),
	'102', "a crash after the creation's commit keeps the copy's rows");

# creating runs a creation of database held from template filled in a
# background session until it waits at an injection point, returning the
# session.
sub creating
{
	my ($point) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	quiet($node, 'filled');
	my $creator = $node->background_psql('postgres', on_error_stop => 0);
	$creator->query_until(qr/start/,
		"\\echo start\nCREATE DATABASE held TEMPLATE filled;\n");
	$node->wait_for_event('client backend', $point);
	return $creator;
}

# A crash after the copy and before the commit leaves no repository once the
# next checkpoint removes what the end of redo dropped, and the creation run
# again copies the rows.
my $creator = creating('doltgres-store-creation-copied');
my ($held) = grep { -e "$versioned/$_/creating" } map { s{.*/}{}r } glob("$versioned/*");
ok(defined $held, 'a creation before its commit leaves its copy marked');
$node->stop('immediate');
eval { $creator->quit };
$node->start;
$node->safe_psql('postgres', 'CHECKPOINT');
ok(!-e "$versioned/$held",
	'a crash before the commit leaves no repository of the creation');
quiet($node, 'filled');
$node->safe_psql('postgres', 'CREATE DATABASE held TEMPLATE filled');
is($node->safe_psql('held', 'SELECT count(*) FROM t'),
	'102', 'a creation after a crash before its commit copies the rows');
quiet($node, 'held');
$node->safe_psql('postgres', 'DROP DATABASE held');

# An abort after the copy leaves no repository either once the next
# checkpoint removes what the abort dropped.
$creator = creating('doltgres-store-creation-copied');
($held) = grep { -e "$versioned/$_/creating" } map { s{.*/}{}r } glob("$versioned/*");
$node->safe_psql('postgres',
	"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'doltgres-store-creation-copied'"
);
$creator->quit;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-store-creation-copied'); CHECKPOINT");
ok(!-e "$versioned/$held", 'an abort after the copy leaves no repository');

# A crash after the commit and before its mark is cleared keeps the copy, and
# recovery clears the mark.
$creator = creating('doltgres-store-creation-ending');
($held) = grep { -e "$versioned/$_/creating" } map { s{.*/}{}r } glob("$versioned/*");
$node->stop('immediate');
eval { $creator->quit };
$node->start;
is($node->safe_psql('held', 'SELECT count(*) FROM t'),
	'102', "a crash between the commit and the mark's clearing keeps the copy");
ok(!-e "$versioned/$held/creating", 'recovery clears the mark of a committed creation');

# Dropping the copy leaves the template, and dropping the template leaves a
# copy by file copy.
quiet($node, 'folded');
$node->safe_psql('postgres', 'DROP DATABASE folded');
is($node->safe_psql('filled', 'SELECT count(*) FROM t'),
	'102', "dropping a copy leaves the template's rows");
quiet($node, 'checkpointed');
$node->safe_psql('postgres', 'DROP DATABASE checkpointed');
quiet($node, 'filled');
$node->safe_psql('postgres',
	'CREATE DATABASE kept TEMPLATE filled STRATEGY file_copy');
quiet($node, 'filled');
$node->safe_psql('postgres', 'DROP DATABASE filled');
is($node->safe_psql('kept', 'SELECT count(*) FROM t'),
	'102', "dropping the template leaves a copy's rows");

# A standby replays a creation into a copy of its own template, and a standby
# restarted after the creation's commit reads the copy's rows.
$node->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'backup', has_streaming => 1);
$standby->start;
quiet($node, 'kept');
$node->safe_psql('postgres', 'CREATE DATABASE replayed TEMPLATE kept');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('replayed', 'SELECT count(*) FROM t'),
	'102', "a standby's replay of a creation copies its template's rows");
$standby->restart;
is($standby->safe_psql('replayed', 'SELECT count(*) FROM t'),
	'102', "a standby restarted after the creation's commit reads the copy's rows");
$standby->stop;

# A copy in another tablespace than its template's reads the template's rows,
# through its index too.
$node->safe_psql('postgres', "CREATE TABLESPACE spaced LOCATION ''");
quiet($node, 'kept');
$node->safe_psql('postgres',
	'CREATE DATABASE spaced TEMPLATE kept TABLESPACE spaced');
is($node->safe_psql('spaced', 'SELECT count(*) FROM t'),
	'102', "a copy in another tablespace reads its template's rows");
is( $node->safe_psql(
		'spaced', "SET enable_seqscan = off; SELECT k FROM t WHERE v = 'r42'"),
	'42',
	"a copy in another tablespace finds a row through its template's index");

# A database made with no template named copies template1's rows.
$node->safe_psql('template1',
	'CREATE TABLE seeded (k int4); INSERT INTO seeded VALUES (1), (2)');
quiet($node, 'template1');
$node->safe_psql('postgres', 'CREATE DATABASE defaulted');
is($node->safe_psql('defaulted', 'SELECT count(*) FROM seeded'),
	'2', "a database made from template1 by default reads template1's rows");

# A commit whose end couldn't clear its creation's mark leaves it, which the
# next checkpoint clears.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-store-creation-ending-fails', 'notice')"
);
quiet($node, 'kept');
$node->safe_psql('postgres', 'CREATE DATABASE unsettled TEMPLATE kept');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-store-creation-ending-fails')");
my $unsettled = oid('unsettled');
ok(-e "$versioned/$unsettled/creating",
	"a commit whose end couldn't clear its creation's mark leaves it");
$node->safe_psql('postgres', 'CHECKPOINT');
ok(!-e "$versioned/$unsettled/creating",
	"the next checkpoint clears the mark of a committed creation");
is($node->safe_psql('unsettled', 'SELECT count(*) FROM t'),
	'102', 'the creation whose mark a checkpoint cleared reads its rows');

# A streaming standby replays a creation from a never-folded keyless table,
# its digest of its own template matching the primary's, and the primary's
# crash recovery keeps the copy.
$node->backup('replica_backup');
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($node, 'replica_backup', has_streaming => 1);
$replica->start;
quiet($node, 'template1');
$node->safe_psql('postgres', 'CREATE DATABASE keyless');
$node->safe_psql('keyless',
	'CREATE TABLE nk (a int4, b text); INSERT INTO nk SELECT g, md5(g::text) FROM generate_series(1, 50) g'
);
quiet($node, 'keyless');
$node->safe_psql('postgres', 'CREATE DATABASE keyless_copy TEMPLATE keyless');
$node->wait_for_replay_catchup($replica);
is($replica->safe_psql('keyless_copy', 'SELECT count(*) FROM nk'),
	'50', "a standby's replay of a never-folded keyless table's copy matches its primary's digest");
$node->stop('immediate');
$node->start;
is($node->safe_psql('keyless_copy', 'SELECT count(*) FROM nk'),
	'50', "crash recovery keeps a never-folded keyless table's copy");

# A template table whose state crash recovery made, with no backend opening
# it since, folds as unknown on the primary, and a standby that read the
# table accepts the creation.
quiet($node, 'template1');
$node->safe_psql('postgres', 'CREATE DATABASE unknown_key');
$node->safe_psql('unknown_key',
	'CREATE TABLE uk (a int4); INSERT INTO uk SELECT generate_series(1, 30)');
$node->wait_for_replay_catchup($replica);
$replica->safe_psql('unknown_key', 'SELECT count(*) FROM uk');
$node->stop('immediate');
$node->start;
quiet($node, 'unknown_key');
$node->safe_psql('postgres', 'CREATE DATABASE unknown_copy TEMPLATE unknown_key');
$node->wait_for_replay_catchup($replica);
is($replica->safe_psql('unknown_copy', 'SELECT count(*) FROM uk'),
	'30', "a standby accepts a creation whose storage's key the primary didn't know");

# A standby whose replay digests its template differently than the primary
# did refuses recovery, naming both digests.
$replica->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-store-creation-diverged', 'notice')");
quiet($node, 'keyless');
$node->safe_psql('postgres', 'CREATE DATABASE diverged TEMPLATE keyless');
ok( $replica->wait_for_log(
		qr/Template database \d+ of database \d+ digests [0-9a-f]{64} here and digested [0-9a-f]{64} on the primary/
	),
	"a standby whose copy's digest differs from its primary's refuses recovery");
$replica->stop('immediate', fail_ok => 1);

$node->stop;

# A template whose delta is far past the delta's budget, so most of its
# committed versions are spilled: wide TOASTed rows under an expression index,
# part of them folded. Its copy reads every row, through the index too, with
# no checkpoint due to relieve the budget: the creation's commit asks for the
# copy's folds, and no fold worker is started for it before.
my $small = PostgreSQL::Test::Cluster->new('budget');
$small->init;
$small->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h");
$small->start;
quiet($small, 'template1');
$small->safe_psql('postgres', 'CREATE DATABASE wide');
$small->safe_psql(
	'wide', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE w (k int4 PRIMARY KEY, v text);
CREATE INDEX w_e ON w ((length(v) + k));
INSERT INTO w SELECT g, repeat(md5(g::text), 70) FROM generate_series(1, 4000) g;
SELECT doltgres_test_fold('w');
INSERT INTO w SELECT g, repeat(md5(g::text), 70) FROM generate_series(4001, 5000) g;
});
my $wide = $small->safe_psql('wide', 'SELECT count(*), sum(length(v)) FROM w');

# A checkpoint after the template's load leaves the crashes below only the
# creations to replay.
$small->safe_psql('postgres', 'CHECKPOINT');
quiet($small, 'wide');
$small->safe_psql('postgres', 'CREATE DATABASE wide_copy TEMPLATE wide');
is($small->safe_psql('wide_copy', 'SELECT count(*), sum(length(v)) FROM w'),
	$wide, "a copy of a template whose delta is past the budget reads every row");
my $wide_copy = $small->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'wide_copy'");
unlike(
	slurp_file($small->logfile),
	qr/database $wide_copy does not exist/,
	"no fold worker is started for a database before its creation commits");
is( $small->safe_psql(
		'wide_copy',
		'SET enable_seqscan = off; SELECT k FROM w WHERE length(v) + k = 2240 + 4500'),
	'4500',
	"a copy of a template whose delta is past the budget finds a row through its expression index"
);

# The copy's toasted values read back whole after an immediate restart, the
# folded rows' from its packs and the unfolded rows' from its runs.
$small->stop('immediate');
$small->start;
is($small->safe_psql('wide_copy', 'SELECT count(*), sum(length(v)) FROM w'),
	$wide, "a copy's folded and unfolded toasted values read back after a restart");

# A crash right after a creation commits, before any checkpoint, leaves the
# start's loader the copy's runs at the same budget, which it loads.
quiet($small, 'wide');
$small->safe_psql('postgres', 'CREATE DATABASE wide_crashed TEMPLATE wide');
$small->stop('immediate');
$small->start;
is($small->safe_psql('wide_crashed', 'SELECT count(*), sum(length(v)) FROM w'),
	$wide, "a start loads a copy past the budget that a crash left unsaved");

# A writer in a third database that the budget holds while a copy loads
# finishes, and so does the copy.
quiet($small, 'template1');
$small->safe_psql('postgres', 'CREATE DATABASE third');
$small->safe_psql('third', 'CREATE TABLE t3 (k int4 PRIMARY KEY, v text)');
my $third = $small->background_psql('third');
$third->query_until(qr/start/,
	"\\echo start\nINSERT INTO t3 SELECT g, repeat(md5(g::text), 70) FROM generate_series(1, 2000) g;\n\\echo done\n"
);
quiet($small, 'wide');
$small->safe_psql('postgres', 'CREATE DATABASE wide_alongside TEMPLATE wide');
$third->query_until(qr/done/, '');
$third->quit;
is( $small->safe_psql('third', 'SELECT count(*) FROM t3') . '|'
	  . $small->safe_psql('wide_alongside', 'SELECT count(*) FROM w'),
	'2000|5000',
	"a third database's writer and a copy loading past the budget both finish");
$small->stop;

# A copy's first insert takes a tuple id no copied row holds, whether its
# template was folded after its last checkpoint, not folded since it, folded
# and then checkpointed, or folded, checkpointed, and restarted before the
# copy (H3, bugs row 519).
my $counted = PostgreSQL::Test::Cluster->new('counted');
$counted->init;
$counted->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
$counted->start;
for my $shape (qw(folded unfolded checkpointed restarted))
{
	$counted->safe_psql('postgres', "CREATE DATABASE from_$shape");
	$counted->safe_psql("from_$shape",
		'CREATE EXTENSION doltgres_test; CREATE TABLE t (id int4, v text)');
	$counted->safe_psql("from_$shape",
		"INSERT INTO t SELECT g, 'first' FROM generate_series(1, 50) g; CHECKPOINT");
	$counted->safe_psql("from_$shape",
		"INSERT INTO t SELECT g, 'second' FROM generate_series(51, 100) g");
	$counted->safe_psql("from_$shape", q{SELECT doltgres_test_fold('t')})
	  unless $shape eq 'unfolded';
	$counted->safe_psql("from_$shape", 'CHECKPOINT') if $shape =~ /^(checkpointed|restarted)$/;
	$counted->restart if $shape eq 'restarted';
	quiet($counted, "from_$shape");
	$counted->safe_psql('postgres', "CREATE DATABASE copy_$shape TEMPLATE from_$shape");
	is( $counted->safe_psql(
			"copy_$shape",
			"INSERT INTO t VALUES (1000, 'new'); SELECT count(*) || ' ' || count(DISTINCT ctid) FROM t"),
		'101 101',
		"a copy of the $shape template inserts at a tuple id no copied row holds");
}
$counted->stop;

# A copy's table that no state named at the copy, whose template was folded
# after the last checkpoint, keeps its copied cut through a crash: the
# start's redo makes it from the copy's runs, and its next insert takes a
# tuple id no copied row holds (H3, bugs row 521).
my $cut = PostgreSQL::Test::Cluster->new('cut');
$cut->init;
$cut->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
$cut->start;
$cut->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CREATE DATABASE from_cut');
$cut->safe_psql('from_cut',
	'CREATE EXTENSION doltgres_test; CREATE TABLE t (id int4, v text); CHECKPOINT');
$cut->safe_psql('from_cut',
	"INSERT INTO t SELECT g, 'first' FROM generate_series(1, 50) g");
$cut->safe_psql('from_cut',
	q{SELECT doltgres_test_fold('t'); SELECT doltgres_test_save()});
my $pool = $cut->safe_psql('postgres', 'SELECT doltgres_test_relation_locks()');
$cut->safe_psql('postgres', "SELECT doltgres_test_states(900001, $pool)");
quiet($cut, 'from_cut');
$cut->safe_psql('postgres', 'CREATE DATABASE copy_cut TEMPLATE from_cut');
$cut->safe_psql('copy_cut', 'DELETE FROM t WHERE id = 25');
$cut->stop('immediate');
$cut->start;
is( $cut->safe_psql(
		'copy_cut',
		"INSERT INTO t VALUES (1000, 'new'); SELECT count(*) || ' ' || count(DISTINCT ctid) FROM t"),
	'50 50',
	"a crashed copy of a template folded after its checkpoint inserts at a tuple id no copied row holds");
$cut->stop;

done_testing();
