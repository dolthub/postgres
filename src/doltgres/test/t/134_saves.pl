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

# Bugs row 340: item 6's saves outside a checkpoint. At delta_memory 1MB,
# committed transactions' identity versions, which only a save frees, don't
# stop later writers: a refused writer runs the save for them, so many
# committed CREATE TABLEs finish with no checkpoint; a states' pool at its
# smallest, filled with unsaved states, makes room by a save with no
# checkpoint; and crash recovery replays a fold exactly.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('saves');
$node->init;
$node->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');

my $checkpoints =
  'SELECT num_timed + num_requested FROM pg_stat_checkpointer';

# Many committed CREATE TABLEs, each its own transaction, finish at 1MB with
# no checkpoint: the writers the budget refuses save the committed identity
# versions.
my $before = $node->safe_psql('postgres', $checkpoints);
my ($code, $out, $err) = $node->psql('postgres',
	"SET statement_timeout = '300s'; DO \$\$ BEGIN FOR i IN 1 .. 2000 LOOP EXECUTE format('CREATE TABLE committed_%s (k int4, v text)', i); COMMIT; END LOOP; END \$\$"
);
is($code . $err,
	'0',
	"2,000 committed CREATE TABLEs at 1MB finish with no checkpoint run");
is($node->safe_psql('postgres', $checkpoints),
	$before, "and no checkpoint ran meanwhile");

$node->stop;

# A states' pool at its smallest, filled with storages' unsaved states, makes
# room for another by saving with no checkpoint.
my $pooled = PostgreSQL::Test::Cluster->new('pooled');
$pooled->init;
$pooled->append_conf('postgresql.conf',
	"max_connections = 10\nmax_locks_per_transaction = 10\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$pooled->start;
$pooled->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');

# Coverage of a restart after a non-checkpoint save (no failing-before: redo
# replays every identity change after the last checkpoint's redo point, so a
# run's older version never outlives a newer one): a table dropped and made
# again under its name, whose id pairs with its new OID, and a table made and
# dropped, which no pair names, both before the saves the pool's makes run.
$pooled->safe_psql('postgres', 'CREATE TABLE changed (k int4); CHECKPOINT');
my $old = $pooled->safe_psql('postgres', "SELECT 'changed'::regclass::oid");
my $id =
  $pooled->safe_psql('postgres', "SELECT doltgres_test_identity(2, $old, 0)");
$pooled->safe_psql('postgres', 'DROP TABLE changed');
$pooled->safe_psql('postgres', 'CREATE TABLE changed (k int4)');
my $new = $pooled->safe_psql('postgres', "SELECT 'changed'::regclass::oid");
$pooled->safe_psql('postgres', 'CREATE TABLE gone (k int4)');
my $gone = $pooled->safe_psql('postgres', "SELECT 'gone'::regclass::oid");
$pooled->safe_psql('postgres', 'DROP TABLE gone');
my $slots =
  $pooled->safe_psql('postgres', 'SELECT doltgres_test_relation_locks()');
my $pool_before = $pooled->safe_psql('postgres', $checkpoints);
my $made = $slots + 50;
($code, $out, $err) = $pooled->psql('postgres',
	"SET statement_timeout = '300s'; DO \$\$ BEGIN FOR i IN 1 .. $made LOOP EXECUTE format('CREATE TABLE pooled_%s (k int4)', i); EXECUTE format('INSERT INTO pooled_%s VALUES (1)', i); COMMIT; END LOOP; END \$\$"
);
is($code . $err,
	'0', "writing into more storages than the states' pool holds finishes");
is($pooled->safe_psql('postgres', $checkpoints),
	$pool_before, "and no checkpoint ran meanwhile");
is( $pooled->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_class WHERE relname LIKE 'pooled\\_%' AND relkind = 'r'"
	),
	$made,
	"and every table it made is there");
$pooled->stop('immediate');
$pooled->start;
is( $pooled->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $new, 0)"),
	$id,
	"after a crash past the pool's saves, a table made again under its name pairs its id with its new OID"
);
is( $pooled->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $old, 0)"),
	'none',
	"and its old OID with nothing");
is( $pooled->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $gone, 0)"),
	'none',
	"and a table made and dropped before the saves has no pair");
$pooled->stop;

# Crash recovery replays a fold after its rows' commit records exactly: the
# replayed fold takes out the committed row version the live fold took out,
# since a transaction whose commit redo replayed reads as committed there.
my $replayed = PostgreSQL::Test::Cluster->new('replayed');
$replayed->init;
$replayed->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h");
$replayed->start;
$replayed->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$replayed->safe_psql('postgres',
	'CREATE TABLE folded (k int4 PRIMARY KEY, v text); CHECKPOINT');
$replayed->safe_psql('postgres', "INSERT INTO folded VALUES (1, 'a')");
$replayed->safe_psql('postgres', "SELECT doltgres_test_fold('folded')");
is( $replayed->safe_psql(
		'postgres', "SELECT doltgres_test_held('folded', '(0,1)')"),
	'f',
	"a fold takes out a committed row's version");
$replayed->safe_psql('postgres', 'SELECT pg_switch_wal()');
$replayed->stop('immediate');
$replayed->start;
is( $replayed->safe_psql(
		'postgres', "SELECT doltgres_test_held('folded', '(0,1)')"),
	'f',
	"and after a crash its replay after the row's commit record takes it out too"
);
is($replayed->safe_psql('postgres', 'SELECT v FROM folded WHERE k = 1'),
	'a', "and the row reads back from the base");
$replayed->stop;

# Bugs row 350: a durable manifest names only what is durable. A save waits
# for the WAL through the record of each base it gathers, lists every pack
# whose add its repository defers, and waits for the WAL through its fold
# point when it folds an identity pair. The walwriter waits ten seconds
# between flushes, so nothing but the save flushes the WAL in these cases.
my $logged = PostgreSQL::Test::Cluster->new('logged');
$logged->init;
$logged->append_conf('postgresql.conf',
	"autovacuum = off\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h\nwal_writer_delay = 10s\nrestart_after_crash = on"
);
$logged->start;
$logged->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE EXTENSION injection_points; CREATE EXTENSION pg_walinspect'
);

# landed runs a save in a session of its own, paused once its manifest is
# written, and returns the session.
my $landed = sub {
	my $saver = $logged->background_psql('postgres', on_error_stop => 0);
	$saver->query_safe('SELECT injection_points_set_local()');
	$saver->query_safe(
		"SELECT injection_points_attach('doltgres-repo-save-landed', 'wait')"
	);
	$saver->query_until(qr/start/,
		"\\echo start\nSELECT doltgres_test_save();\n");
	$logged->wait_for_event('client backend', 'doltgres-repo-save-landed');
	return $saver;
};

# A fold paused after its install, a save that gathers its base, and a crash:
# the save flushed the fold's record, so the restart reads the row once. The
# tables of the row-once cases have no key, so a row both a base and the delta
# hold reads twice.
$logged->safe_psql('postgres',
	'CREATE TABLE logged_fold (k int4, v text); CHECKPOINT');
my $start =
  $logged->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$logged->safe_psql('postgres', "INSERT INTO logged_fold VALUES (1, 'a')");
my $folder = $logged->background_psql('postgres', on_error_stop => 0);
$folder->query_safe('SELECT injection_points_set_local()');
$folder->query_safe(
	"SELECT injection_points_attach('doltgres-fold-installed', 'wait')");
$folder->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_fold('logged_fold');\n");
$logged->wait_for_event('client backend', 'doltgres-fold-installed');
my $saver = $landed->();
is( $logged->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_get_wal_records_info('$start', pg_current_wal_flush_lsn()) WHERE record_type = 'BaseAdvanced'"
	),
	'1',
	"a save's manifest names a fold's base only once the base's record is flushed"
);
$logged->stop('immediate');
eval { $folder->quit };
eval { $saver->quit };
$logged->start;
is($logged->safe_psql('postgres', 'SELECT count(*) FROM logged_fold'),
	'1', "and after a crash the restart reads the folded row once");
is( $logged->safe_psql(
		'postgres', "SELECT doltgres_test_held('logged_fold', '(0,1)')"),
	'f',
	"and the replay of the fold's record takes its version out of the delta");

# A save whose own base tree's pack has a deferred add, and a crash: the save
# listed the pack before its manifest named it, so the restart finds every
# root the manifest names.
$logged->safe_psql('postgres',
	"CREATE TABLE deferred_pack (k int4 PRIMARY KEY, v text); INSERT INTO deferred_pack SELECT g, 'd' || g FROM generate_series(1, 100) g"
);
$logged->safe_psql('postgres', "SELECT doltgres_test_fold('deferred_pack')");
$saver = $landed->();
$logged->stop('immediate');
eval { $saver->quit };
$logged->start;
is( $logged->safe_psql('postgres', 'SELECT count(*) FROM deferred_pack'),
	'100',
	"after a crash past a save, the restart finds every root its manifest names"
);

# A table made with an asynchronous commit, a save that folds its identity
# pair, and a crash: the save flushed the commit's record, so the restart has
# the table with its pair.
my $async = $logged->background_psql('postgres');
$async->query_safe(
	"SET synchronous_commit = off; CREATE TABLE async_made (k int4)");
my $made_oid =
  $logged->safe_psql('postgres', "SELECT 'async_made'::regclass::oid");
my $made_id = $logged->safe_psql('postgres',
	"SELECT doltgres_test_identity(2, $made_oid, 0)");
$saver = $landed->();
$logged->stop('immediate');
eval { $async->quit };
eval { $saver->quit };
$logged->start;
is( $logged->safe_psql(
		'postgres', "SELECT count(*) FROM pg_class WHERE oid = $made_oid"),
	'1',
	"after a crash past a save that folded an asynchronous commit's identity pair, the table is there"
);
is( $logged->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $made_oid, 0)"),
	$made_id,
	"with its pair");

# A save past its fold point and a fold paused between its record's insertion
# and its base's publish (a fold point waits out the install's checkpoint
# delay, so only a save that took its point first gathers in the window): a
# reader reads the row once across the save and the publish, and the save
# names the base the fold replaces, so after a crash the restart reads each
# row once.
my $pointed = sub {
	my $saver = $logged->background_psql('postgres', on_error_stop => 0);
	$saver->query_safe('SELECT injection_points_set_local()');
	$saver->query_safe(
		"SELECT injection_points_attach('doltgres-runs-save-pointed', 'wait')"
	);
	$saver->query_until(qr/start/,
		"\\echo start\nSELECT doltgres_test_save();\n");
	$logged->wait_for_event('client backend', 'doltgres-runs-save-pointed');
	return $saver;
};
my $logging = sub {
	my ($table) = @_;
	my $logger = $logged->background_psql('postgres', on_error_stop => 0);
	$logger->query_safe('SELECT injection_points_set_local()');
	$logger->query_safe(
		"SELECT injection_points_attach('doltgres-state-logging', 'wait')");
	$logger->query_until(qr/start/,
		"\\echo start\nSELECT doltgres_test_fold('$table');\n");
	$logged->wait_for_event('client backend', 'doltgres-state-logging');
	return $logger;
};
my $saved = sub {
	my ($saver) = @_;
	$logged->safe_psql('postgres',
		"SELECT injection_points_wakeup('doltgres-runs-save-pointed')");
	$logged->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE query LIKE 'SELECT doltgres_test_save()%' AND state = 'active'"
	) or die "the save never finished";
	$saver->quit;
};
$logged->safe_psql('postgres',
	'CREATE TABLE logging_fold (k int4, v text); CHECKPOINT');
$logged->safe_psql('postgres', "INSERT INTO logging_fold VALUES (1, 'p')");
$saver = $pointed->();
$folder = $logging->('logging_fold');
my $reader = $logged->background_psql('postgres');
is( $reader->query_safe(
		'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM logging_fold'
	),
	'1',
	"a reader while a fold's record is in and its base unpublished reads the row once"
);
$saved->($saver);
$logged->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-state-logging')");
$folder->quit;
is($reader->query_safe('SELECT count(*) FROM logging_fold'),
	'1', "and still once after a save in the window and the base's publish");
$reader->query_safe('COMMIT');
$reader->quit;
my $second = $logged->safe_psql('postgres',
	"SELECT pg_current_wal_insert_lsn() - '0/0'::pg_lsn");
$logged->safe_psql('postgres', "INSERT INTO logging_fold VALUES (2, 'q')");
$saver = $pointed->();
$folder = $logging->('logging_fold');
$saved->($saver);
$logged->stop('immediate');
eval { $folder->quit };
$logged->start;
my (undef, $folded) = split / /,
  $logged->safe_psql('postgres',
	"SELECT doltgres_test_base_points('logging_fold')");
is( ($folded < $second ? 'below' : 'past') . ' '
	  . $logged->safe_psql(
		'postgres', "SELECT doltgres_test_held('logging_fold', '(0,2)')"),
	'below t',
	"after a crash past a save that gathered before a base was published, the base is the one the save named and the delta holds the later row"
);
is($logged->safe_psql('postgres', 'SELECT count(*) FROM logging_fold'),
	'2', "and the restart reads each row once");

# An error inside a logging install's window stops the server, and recovery
# replays the records before it, so the row reads once.
$logged->safe_psql('postgres',
	"CREATE TABLE stopped_fold (k int4, v text); INSERT INTO stopped_fold VALUES (1, 's')"
);
$folder = $logged->background_psql('postgres', on_error_stop => 0);
$folder->query_safe('SELECT injection_points_set_local()');
$folder->query_safe(
	"SELECT injection_points_attach('doltgres-state-logging', 'error')");
eval { $folder->query("SELECT doltgres_test_fold('stopped_fold')") };
eval { $folder->quit };
ok( $logged->log_contains(
		'PANIC:  error triggered for injection point doltgres-state-logging'),
	"an error inside a logging install's window stops the server");
$logged->poll_query_until('postgres', 'SELECT true')
  or die "the server never came back after the stop";
is($logged->safe_psql('postgres', 'SELECT count(*) FROM stopped_fold'),
	'1', "and recovery reads the row once");

# Bugs row 354: no record a process logs from a base's content lands before
# the base's own record, since a base is published only after its record is
# in. VACUUM reading the previous base while a fold's record is in and its
# base unpublished logs its frees after the fold's record, and a standby that
# replays both reads every row at the tuple id its primary does.
my $vacuuming = PostgreSQL::Test::Cluster->new('vacuuming');
$vacuuming->init(allows_streaming => 1);
$vacuuming->append_conf('postgresql.conf',
	"autovacuum = off\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$vacuuming->start;
$vacuuming->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE EXTENSION injection_points');
$vacuuming->backup('vacuuming_backup');
my $vacuumed = PostgreSQL::Test::Cluster->new('vacuumed');
$vacuumed->init_from_backup($vacuuming, 'vacuuming_backup',
	has_streaming => 1);
$vacuumed->start;
$vacuuming->safe_psql('postgres',
	"CREATE TABLE vacuumed (k int4, v text); INSERT INTO vacuumed SELECT g, 'v' || g FROM generate_series(1, 20) g"
);
$vacuuming->safe_psql('postgres', "SELECT doltgres_test_fold('vacuumed')");
$vacuuming->safe_psql('postgres', 'DELETE FROM vacuumed WHERE k <= 5');
$vacuuming->safe_psql('postgres', "SELECT doltgres_test_fold('vacuumed')");
$vacuuming->safe_psql('postgres', "INSERT INTO vacuumed VALUES (21, 'w')");
my $windowed = $vacuuming->background_psql('postgres', on_error_stop => 0);
$windowed->query_safe('SELECT injection_points_set_local()');
$windowed->query_safe(
	"SELECT injection_points_attach('doltgres-state-logging', 'wait')");
$windowed->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_fold('vacuumed');\n");
$vacuuming->wait_for_event('client backend', 'doltgres-state-logging');
$vacuuming->safe_psql('postgres',
	"SELECT doltgres_test_vacuum('vacuumed', 'on')");
$vacuuming->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-state-logging')");
$windowed->quit;
$vacuuming->safe_psql('postgres',
	"INSERT INTO vacuumed SELECT g, 'x' || g FROM generate_series(22, 30) g");
$vacuuming->safe_psql('postgres', "SELECT doltgres_test_fold('vacuumed')");
$vacuuming->wait_for_replay_catchup($vacuumed);
my $placed =
  "SELECT string_agg(ctid::text || ':' || k, ' ' ORDER BY ctid) FROM vacuumed";
is( $vacuumed->safe_psql('postgres', $placed),
	$vacuuming->safe_psql('postgres', $placed),
	"a standby replaying VACUUM's frees logged in a fold's window reads every row at its primary's tuple id"
);
$vacuumed->stop;
$vacuuming->stop;

$logged->stop;

# A save outside a checkpoint between two updates stores a storage's live
# tuple id counter in its base record, but a crash start replays from the
# redo point, so a start makes the state's counter at its base's fresh end
# and the WAL moves it on as live did: the replayed page leaves and fresh
# pages' takes read back every row updated, and no tuple id is handed out
# twice after the start (bugs row 396).
my $restarted = PostgreSQL::Test::Cluster->new('restarted');
$restarted->init;
$restarted->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off"
);
$restarted->start;
$restarted->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$restarted->safe_psql(
	'postgres', q{
BEGIN;
CREATE TABLE restarted (k int4, v text);
ALTER TABLE restarted ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO restarted SELECT g, repeat(md5(g::text), 100) FROM generate_series(1, 400) g;
DELETE FROM restarted WHERE k % 2 = 0;
SELECT doltgres_test_fold('restarted');
COMMIT;
});
$restarted->safe_psql('postgres',
	"UPDATE restarted SET v = v || 'y' WHERE k <= 200");
$restarted->safe_psql('postgres', 'SELECT doltgres_test_save()');
$restarted->safe_psql('postgres',
	"UPDATE restarted SET v = v || 'y' WHERE k > 200");
$restarted->stop('immediate');
$restarted->start;
is( $restarted->safe_psql(
		'postgres',
		q{SELECT count(*), sum(k), count(*) FILTER (WHERE v = repeat(md5(k::text), 100) || 'y') FROM restarted}
	),
	'200|40000|200',
	'after a save and a crash every row reads back updated');
$restarted->safe_psql('postgres',
	"INSERT INTO restarted SELECT g, 'n' FROM generate_series(401, 600) g");
is( $restarted->safe_psql(
		'postgres', 'SELECT count(*), count(DISTINCT ctid) FROM restarted'),
	'400|400',
	"and the rows inserted after the crash's start take tuple ids no row holds"
);
$restarted->stop;

# A state saved with rows in its delta keeps the live counter in its record,
# but once a fold empties its delta, a states' pool at its smallest reclaims
# it, since a backend then holds as many states as the pool has, and the next
# write makes it again with the counter at its base's fresh end: every
# committed row reads back and no tuple id is held twice (bugs row 396).
my $reclaimed = PostgreSQL::Test::Cluster->new('reclaimed');
$reclaimed->init;
$reclaimed->append_conf('postgresql.conf',
	"max_connections = 10\nmax_locks_per_transaction = 10\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$reclaimed->start;
$reclaimed->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$reclaimed->safe_psql('postgres', 'CREATE TABLE reclaimed (k int4, v text)');
$reclaimed->safe_psql('postgres',
	"INSERT INTO reclaimed SELECT g, repeat('v', 200) FROM generate_series(1, 20) g"
);
$reclaimed->safe_psql('postgres', "SELECT doltgres_test_fold('reclaimed')");
$reclaimed->safe_psql('postgres',
	"INSERT INTO reclaimed SELECT g, repeat('v', 200) FROM generate_series(21, 60) g"
);
$reclaimed->safe_psql('postgres', 'SELECT doltgres_test_save()');
$reclaimed->safe_psql('postgres', "SELECT doltgres_test_fold('reclaimed')");
my $pool =
  $reclaimed->safe_psql('postgres', 'SELECT doltgres_test_relation_locks()');
is( $reclaimed->safe_psql(
		'postgres', "SELECT doltgres_test_states(900001, $pool)"),
	$pool,
	"a backend holds as many states as the pool has, so the pool reclaimed the saved one"
);
$reclaimed->safe_psql('postgres',
	"INSERT INTO reclaimed SELECT g, repeat('v', 200) FROM generate_series(61, 80) g"
);
is( $reclaimed->safe_psql(
		'postgres',
		'SELECT count(*), sum(k), count(DISTINCT ctid) FROM reclaimed'),
	'80|3240|80',
	"a state made again after its reclaim reads every committed row back and holds no tuple id twice"
);
$reclaimed->stop;

# A save outside a checkpoint makes durable a fold's base that applied the
# takes of the writers committed or aborted before its point, each block's up
# to the first take of a writer still open then, while the redo point stays
# before them all, so a crash start's redo leaves out what the base applied,
# as Doltgrust's recovery does, and applies the rest: every row reads back and
# every free space entry is the live one (bugs row 401). Each form runs its
# steps on a node made after a checkpoint, with rows its count and sum.
sub crashed_around
{
	my ($name, $conf, $steps, $rows) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name =~ tr/ /_/r);
	$node->init;
	$node->append_conf('postgresql.conf',
		"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off\n$conf");
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
	$node->safe_psql('postgres', 'CREATE TABLE folded (k int4, v text)');
	$node->safe_psql('postgres', 'CHECKPOINT');
	$steps->($node);
	my $entries =
	  q{SELECT string_agg(b || ':' || doltgres_test_free_space('folded', b), ' ' ORDER BY b) FROM generate_series(0, 8) b};
	my $live = $node->safe_psql('postgres', $entries);
	$node->stop('immediate');
	$node->start;
	is($node->safe_psql('postgres', 'SELECT count(*), sum(k) FROM folded'),
		$rows, "after a crash past a save of a base folded $name every row reads back");
	is($node->safe_psql('postgres', $entries),
		$live, "after a crash past a save of a base folded $name every free space entry is the live one");
	$node->stop;
}

# inserted inserts the rows from one key to another, of 200 bytes each.
sub inserted
{
	my ($from, $to) = @_;
	return "INSERT INTO folded SELECT g, repeat('v', 200) FROM generate_series($from, $to) g";
}

# An open writer's takes follow the folded writer's on the fresh page and
# replay, while a writer whose commit ends past the point replays whole.
crashed_around(
	'around an open writer',
	'',
	sub {
		my ($node) = @_;
		$node->safe_psql('postgres', inserted(1, 100));
		my $open = $node->background_psql('postgres');
		$open->query_safe('BEGIN; ' . inserted(101, 200));
		$node->safe_psql('postgres', inserted(201, 300));
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded')");
		$node->safe_psql('postgres', 'SELECT doltgres_test_save()');
		$open->query_safe('COMMIT');
		$open->quit;
	},
	'300|45150');

# The open writer aborts after the save, its takes replayed as its abort
# leaves them.
crashed_around(
	'around a writer aborted after it',
	'',
	sub {
		my ($node) = @_;
		$node->safe_psql('postgres', inserted(1, 100));
		my $open = $node->background_psql('postgres');
		$open->query_safe('BEGIN; ' . inserted(101, 200));
		$node->safe_psql('postgres', inserted(201, 300));
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded')");
		$node->safe_psql('postgres', 'SELECT doltgres_test_save()');
		$open->query_safe('ROLLBACK');
		$open->quit;
	},
	'200|30100');

# A writer whose xid comes before an open writer's takes block 0's freed
# offsets after the open writer's take there and commits; with only the open
# writer running, the horizon doesn't keep it out, so the fold applies it
# while the block's run as the fold took it ends at the open writer's take,
# and its take there replays.
crashed_around(
	'past an open writer an earlier xid follows',
	'',
	sub {
		my ($node) = @_;
		$node->safe_psql('postgres', inserted(1, 100));
		$node->safe_psql('postgres', 'DELETE FROM folded WHERE k <= 34');
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded')");
		$node->safe_psql('postgres', 'VACUUM folded');
		my $early = $node->background_psql('postgres');
		$early->query_safe('BEGIN; SELECT pg_current_xact_id()');
		my $open = $node->background_psql('postgres');
		$open->query_safe('BEGIN; ' . inserted(101, 105));
		$early->query_safe(inserted(201, 205) . '; COMMIT');
		$early->quit;
		$node->safe_psql('postgres', 'CREATE TABLE later (k int4)');
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded')");
		$node->safe_psql('postgres', 'SELECT doltgres_test_save()');
		$open->query_safe('COMMIT');
		$open->quit;
	},
	'76|5985');

# A transaction prepared before the point and committed after it isn't one
# the base applied.
crashed_around(
	'around a prepared writer',
	'max_prepared_transactions = 2',
	sub {
		my ($node) = @_;
		$node->safe_psql('postgres', inserted(1, 100));
		$node->safe_psql('postgres',
			'BEGIN; ' . inserted(101, 200) . "; PREPARE TRANSACTION 'around'");
		$node->safe_psql('postgres', inserted(201, 300));
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded')");
		$node->safe_psql('postgres', 'SELECT doltgres_test_save()');
		$node->safe_psql('postgres', "COMMIT PREPARED 'around'");
	},
	'300|45150');

# A fold begun while a commit's record is in the WAL and its commit log entry
# isn't waits for the commit to leave that window, and its base keeps a point
# lowered below a commit the process array still names, so redo applies the
# writer whichever side of the point the fold put it.
if ($ENV{enable_injection_points} eq 'yes')
{
	crashed_around(
		'around a commit in its window',
		'',
		sub {
			my ($node) = @_;
			$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
			$node->safe_psql('postgres', inserted(1, 100));
			my $committing = $node->background_psql('postgres');
			$committing->query_safe('SELECT injection_points_set_local()');
			$committing->query_safe(
				"SELECT injection_points_attach('commit-before-clog-update', 'wait')"
			);
			$committing->query_safe('BEGIN; ' . inserted(101, 200));
			$committing->query_until(qr/committing/,
				"\\echo committing\nCOMMIT;\n");
			$node->wait_for_event('client backend',
				'commit-before-clog-update');
			my $folding = $node->background_psql('postgres');
			$folding->query_until(qr/folding/,
				"\\echo folding\nSELECT doltgres_test_fold('folded');\n");
			$node->wait_for_event('client backend', 'CheckpointDelayStart');
			$node->safe_psql('postgres',
				"SELECT injection_points_wakeup('commit-before-clog-update')");
			$committing->quit;
			$folding->quit;
			$node->safe_psql('postgres', 'SELECT doltgres_test_save()');
		},
		'200|20100');
}

# A base folded around a writer that then aborts, saved or with a checkpoint
# after it and a crash, leaves every later insert a tuple id of its own.
foreach my $form ('saved', 'crashed')
{
	my $node = PostgreSQL::Test::Cluster->new("aborted_$form");
	$node->init;
	$node->append_conf('postgresql.conf',
		"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
	$node->safe_psql('postgres', 'CREATE TABLE folded (k int4, v text)');
	$node->safe_psql('postgres', inserted(1, 100));
	my $open = $node->background_psql('postgres');
	$open->query_safe('BEGIN; ' . inserted(101, 200));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('folded')");
	$open->query_safe('ROLLBACK');
	$open->quit;
	if ($form eq 'saved')
	{
		$node->safe_psql('postgres', 'SELECT doltgres_test_save()');
	}
	else
	{
		$node->safe_psql('postgres', 'CHECKPOINT');
		$node->stop('immediate');
		$node->start;
	}
	$node->safe_psql('postgres', inserted(201, 600));
	is( $node->safe_psql('postgres',
			'SELECT count(*), count(DISTINCT ctid), sum(k) FROM folded'),
		'500|500|165250',
		"after a base folded around a writer that aborts is $form every insert takes a tuple id of its own"
	);
	$node->stop;
}

done_testing();
