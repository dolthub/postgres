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

# A checkpoint's status run (bugs row 149): it gives a transaction that
# committed before the checkpoint its commit record's end, read from its entry
# or its process's slot, so after a crash, and on a standby made from a base
# backup, a fold applies every version it wrote; a transaction still running
# at the checkpoint is decided by its own commit record.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('status_run');
$node->init(allows_streaming => 1);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# written commits one transaction of a backend of its own that inserts rows
# into a fork table through the fork's routine, and returns its xid.
sub written
{
	my ($table, $rows) = @_;
	my $out = $node->safe_psql(
		'postgres', qq{
BEGIN;
SELECT doltgres_test_write('$table', true, 'INSERT INTO $table SELECT g, ''v'' FROM generate_series(1, $rows) g');
SELECT pg_current_xact_id();
COMMIT;
});
	return (split /\n/, $out)[1];
}

# outcome returns a top-level xid's outcome word in a server's status table.
sub outcome
{
	my ($on, $xid) = @_;
	return $on->safe_psql('postgres', "SELECT doltgres_test_status_table(4, $xid, 0)");
}

# rows returns how many rows a fork table's scan reads on a server.
sub rows
{
	my ($on, $table) = @_;
	return $on->safe_psql('postgres', "SELECT count(*) FROM doltgres_test_scan('$table', false, -1)");
}

# based returns how many rows a fork table's newest base holds on a server.
sub based
{
	my ($on, $table) = @_;
	return $on->safe_psql('postgres', "SELECT split_part(doltgres_test_base_sizes('$table'), ' ', 2)");
}

# held holds a server's fold launcher at its injection point, so no fold
# starts, the one a checkpoint asks for included.
sub held
{
	my ($on) = @_;
	$on->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
	$on->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'"
	) or die 'the fold launcher of ' . $on->name . ' is not held';
}

# released lets a server's held fold launcher run again.
sub released
{
	my ($on) = @_;
	$on->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
	);
}

# A transaction committed before a checkpoint, then a crash: the start loads
# its commit record's end, so its rows read and a fold applies them.
$node->safe_psql('postgres', 'CREATE TABLE crashed (k int4 PRIMARY KEY, v text)');
my $crashed = written('crashed', 2);
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
isnt(outcome($node, $crashed), '0', 'a crash keeps the end of a commit made before its checkpoint');
is(rows($node, 'crashed'), '2', 'the committed rows read after the crash');
$node->safe_psql('postgres', "SELECT doltgres_test_fold('crashed')");
is(based($node, 'crashed'), '2',
	'a fold after the crash applies a transaction committed before its checkpoint, whichever fold installs it');
is(rows($node, 'crashed'), '2', 'the folded rows read back');

# A hot standby made from a base backup taken after a commit: the backup's
# checkpoint gives the commit its end, so the standby reads its rows and, once
# promoted, folds them.
# Both servers' fold launchers are held, so the folds the backup's and the
# promotion's checkpoints ask for leave the rows to the promoted standby's own
# fold (bugs row 194).
held($node);
$node->safe_psql('postgres', 'CREATE TABLE followed (k int4 PRIMARY KEY, v text)');
my $followed = written('followed', 2);
$node->backup('run_backup');
my $standby = PostgreSQL::Test::Cluster->new('status_run_standby');
$standby->init_from_backup($node, 'run_backup', has_streaming => 1);
$standby->start;
held($standby);
$node->wait_for_replay_catchup($standby);
isnt(outcome($standby, $followed), '0', 'a standby keeps the end of a commit made before its backup\'s checkpoint');
is(rows($standby, 'followed'), '2', 'the standby reads the committed rows');
$standby->promote;
is($standby->safe_psql('postgres', "SELECT doltgres_test_fold('followed')"),
	't', 'a promoted standby\'s fold applies a transaction committed before its backup\'s checkpoint');
is(rows($standby, 'followed'), '2', 'the promoted standby\'s folded rows read back');
released($standby);
released($node);
$standby->stop;

# A transaction still running at a checkpoint and committed after it: the run
# gives it no end, and its own commit record decides it in the crash's redo.
$node->safe_psql('postgres', 'CREATE TABLE across (k int4 PRIMARY KEY, v text)');
my $running = $node->background_psql('postgres');
$running->query_safe(q{BEGIN; SELECT doltgres_test_write('across', true, 'INSERT INTO across VALUES (1, ''v'')')});
my $across = $running->query_safe('SELECT pg_current_xact_id()');
$node->safe_psql('postgres', 'CHECKPOINT');
$running->query_safe('COMMIT');
$running->quit;
$node->stop('immediate');
$node->start;
isnt(outcome($node, $across), '0', 'redo gives a commit past the checkpoint its end');
$node->safe_psql('postgres', "SELECT doltgres_test_fold('across')");
is(based($node, 'across'), '1',
	'a fold after the crash applies a transaction committed after the checkpoint, whichever fold installs it');
is(rows($node, 'across'), '1', 'its row reads back');

$node->stop;

# The same transaction, still running at a checkpoint and committed after it,
# on a server whose fold launcher folds its table soon after the crash (bugs
# row 428): the launcher's fold applies it first, so the case's own fold
# installs nothing and answers false, and the row is in the base either way.
my $raced = PostgreSQL::Test::Cluster->new('status_run_raced');
$raced->init;
$raced->append_conf('postgresql.conf', 'fold_timeout = 1s');
$raced->start;
$raced->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$raced->safe_psql('postgres', 'CREATE TABLE across (k int4 PRIMARY KEY, v text)');
$running = $raced->background_psql('postgres');
$running->query_safe(q{BEGIN; SELECT doltgres_test_write('across', true, 'INSERT INTO across VALUES (1, ''v'')')});
$raced->safe_psql('postgres', 'CHECKPOINT');
$running->query_safe('COMMIT');
$running->quit;
$raced->stop('immediate');
$raced->start;
$raced->poll_query_until('postgres', q{SELECT split_part(doltgres_test_base_sizes('across'), ' ', 2) = '1'})
  or die 'the fold launcher never folded across';
is($raced->safe_psql('postgres', "SELECT doltgres_test_fold('across')"),
	'f', "a fold after the launcher's applies nothing and installs no base");
is(based($raced, 'across'), '1', "the launcher's fold applied the transaction committed after the checkpoint");
is(rows($raced, 'across'), '1', 'its row reads back');

# A transaction committed before a checkpoint, then a crash, on the same
# server (bugs row 436): the launcher's fold applies it first, so the case's
# own fold answers false, and its rows are in the base either way.
$raced->safe_psql('postgres', 'CREATE TABLE crashed (k int4 PRIMARY KEY, v text)');
$raced->safe_psql('postgres',
	q{SELECT doltgres_test_write('crashed', true, 'INSERT INTO crashed SELECT g, ''v'' FROM generate_series(1, 2) g')});
$raced->safe_psql('postgres', 'CHECKPOINT');
$raced->stop('immediate');
$raced->start;
$raced->poll_query_until('postgres', q{SELECT split_part(doltgres_test_base_sizes('crashed'), ' ', 2) = '2'})
  or die 'the fold launcher never folded crashed';
is($raced->safe_psql('postgres', "SELECT doltgres_test_fold('crashed')"),
	'f', "a fold after the launcher's applies nothing of the commit made before the checkpoint");
is(based($raced, 'crashed'), '2', "the launcher's fold applied the transaction committed before the checkpoint");
is(rows($raced, 'crashed'), '2', 'the folded rows read back');
$raced->stop;
done_testing();
