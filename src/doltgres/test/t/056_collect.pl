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

# The collector (design item 6's collection, the collector text): a base backup
# and a collection meet so that no collection changes the pack lists, the
# forwards, or the files while a backup copies them. Every collection runs in
# the collector worker, which doltgres_test_collect asks.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('collect');
$node->init(allows_streaming => 1);
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins(); CREATE EXTENSION injection_points');

# packs returns the names of the pack files in the postgres database's own
# directory of versioned files, in order.
my $database = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'postgres'");
sub packs
{
	return join(' ', sort map { s{.*/}{}r } glob($node->data_dir . "/pg_versioned/$database/*.pack"));
}

# The collector text's §3.3: a base backup started while a marking runs (an
# injection point holding the marking at its first batch) waits for the
# collection, which stops there at its next safe point, retiring and copying
# nothing, so every pack it found is still there (the backup's own checkpoint may
# write another); the backup then starts, and the rows still read.
$node->safe_psql('postgres', 'CREATE TABLE kept (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('kept', true, 'INSERT INTO kept SELECT g, repeat(''k'', g) FROM generate_series(1, 50) g')");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('kept')");
my $before = packs();
my $collector = $node->background_psql('postgres');
$collector->query_safe(
	"SELECT injection_points_attach('doltgres-collector-batch', 'wait')");
$collector->query_until(qr/start/,
	"\\echo start\nCREATE TABLE collected AS SELECT doltgres_test_collect() AS done;\n");
$node->wait_for_event('versioned collector', 'doltgres-collector-batch');
my $backup = $node->background_psql('postgres');
$backup->query_until(qr/start/, "\\echo start\nSELECT pg_backup_start('collect', true);\n");
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'versioned collection'")
  or die 'the backup never waited for the collection';
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-collector-batch')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-collector-batch')");
is($collector->query_safe('SELECT done FROM collected'),
	'0 0 0', 'the collection that a backup waited for stopped at its safe point, retiring and copying nothing');
$collector->quit;
$backup->query_safe('SET client_min_messages = warning; SELECT pg_backup_stop()');
$backup->quit;
my %after = map { $_ => 1 } split / /, packs();
is(join(' ', grep { !$after{$_} } split / /, $before), '',
	'a base backup started while a marking runs waits for its collection, which changes no list');
is($node->safe_psql('postgres',
	"SELECT doltgres_test_standing('kept', 'SELECT count(*) FROM kept')"), '50',
	'the rows read after the collection the backup stopped');
like($node->safe_psql('postgres', 'SELECT doltgres_test_collect()'), qr/^\d+ \d+ \d+$/,
	'a collection runs once the backup has ended');

# The free space the collector's schedule compares with its floor (PLAN's one C
# function for free disk space): some bytes free, and no more than the volume
# holds.
my ($free, $size) = split / /, $node->safe_psql('postgres', 'SELECT doltgres_test_disk_space()');
ok($free > 0 && $free <= $size, "the data directory's volume reports free bytes below its size");

# The collector's worker (the collector text's §8.1) runs once the server is
# consistent.
is($node->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'versioned collector'"), '1',
	'the collector worker runs');

# The collector text's §9: a hot standby runs the collector worker and
# collects its own packs, writing no WAL, which a standby can't, and its rows
# read the same after; a standby without hot standby starts no collector until
# it is promoted (BgWorkerStart_ConsistentState).
$node->backup('collected');
my $hot = PostgreSQL::Test::Cluster->new('hot');
$hot->init_from_backup($node, 'collected', has_streaming => 1);
$hot->start;
my $cold = PostgreSQL::Test::Cluster->new('cold');
$cold->init_from_backup($node, 'collected', has_streaming => 1);
$cold->append_conf('postgresql.conf', "hot_standby = off\nlog_min_messages = debug1");
$cold->start;
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('kept', true, 'DELETE FROM kept WHERE k <= 25')");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('kept')");
$node->wait_for_replay_catchup($hot);
like($hot->safe_psql('postgres', 'SELECT doltgres_test_collect()'), qr/^\d+ \d+ \d+$/,
	'a hot standby collects its own packs');
is($hot->safe_psql('postgres',
	"SELECT doltgres_test_standing('kept', 'SELECT count(*) FROM kept')"), '25',
	'a hot standby reads its rows after its own collection');
$hot->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'versioned collector'")
  or die 'the hot standby never started its collector';
$cold->wait_for_log(qr/consistent recovery state reached/);
ok(!$cold->log_contains('starting background worker process "versioned collector"'),
	'a standby without hot standby starts no collector');
$cold->promote;
$cold->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'versioned collector'")
  or die 'the promoted standby never started its collector';
ok($cold->log_contains('starting background worker process "versioned collector"'),
	'a standby without hot standby starts its collector once promoted');
$hot->stop;
$cold->stop;

# The collector text's §3.2, its capture order: a fold that installs between
# the read of the registered roots and the read of the states, and a save (a
# checkpoint's) that writes its manifest between the read of the states and the
# read of the manifests, each held there by an injection point, lose no chunk:
# the rows read after that collection, after the next, and after a restart.
$node->safe_psql('postgres', 'CREATE TABLE raced (k int4 PRIMARY KEY, v text)');
my $raced = 0;
foreach my $point ('doltgres-collector-registered', 'doltgres-collector-states')
{
	$raced += 20;
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('raced', true, 'INSERT INTO raced SELECT g, repeat(''r'', g) FROM generate_series($raced - 19, $raced) g')");
	my $racing = $node->background_psql('postgres');
	$racing->query_safe("SELECT injection_points_attach('$point', 'wait')");
	$racing->query_until(qr/start/,
		"\\echo start\nCREATE TABLE racing AS SELECT doltgres_test_collect() AS done;\n");
	$node->wait_for_event('versioned collector', $point);
	if ($point eq 'doltgres-collector-registered')
	{
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('raced')");
	}
	else
	{
		$node->safe_psql('postgres', 'CHECKPOINT');
	}
	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point')");
	like($racing->query_safe('SELECT done FROM racing'), qr/^\d+ \d+ \d+$/,
		"a collection raced at $point ends");
	$racing->query_safe('DROP TABLE racing');
	$racing->quit;
	is($node->safe_psql('postgres',
		"SELECT doltgres_test_standing('raced', 'SELECT count(*) || '' '' || sum(length(v)) FROM raced')"),
		"$raced " . ($raced * ($raced + 1) / 2),
		"the rows read after a collection raced at $point");
	$node->safe_psql('postgres', 'SELECT doltgres_test_collect()');
	is($node->safe_psql('postgres',
		"SELECT doltgres_test_standing('raced', 'SELECT count(*) || '' '' || sum(length(v)) FROM raced')"),
		"$raced " . ($raced * ($raced + 1) / 2),
		"the rows read after the collection after one raced at $point");
}
$node->restart;
is($node->safe_psql('postgres',
	"SELECT doltgres_test_standing('raced', 'SELECT count(*) || '' '' || sum(length(v)) FROM raced')"),
	'40 820', 'the raced rows read after a restart');

# The collector's settings (the collector text's §8.3), in Postgres' form: their
# defaults, units, and contexts.
is($node->safe_psql('postgres',
	"SELECT string_agg(name || ' ' || setting || ' ' || coalesce(unit, '-') || ' ' || context, ', ' ORDER BY name) FROM pg_settings WHERE name LIKE 'collector\\_%'"),
	'collector_first_packs 64 - sighup, collector_full_interval 86400 s sighup, '
	  . 'collector_io_limit 0 kB sighup, collector_min_free_disk -1 MB sighup, '
	  . 'collector_naptime 1000 ms sighup, collector_target_percent 50 - sighup, '
	  . 'collector_young_packs 16 - sighup, collector_young_size 262144 kB sighup',
	"the collector's settings have Postgres' units, defaults, and contexts");

# The collector worker collects every database's repository on its own once
# its collection is due, rereading its settings at a reload: in another
# database, a base whose rows were all deleted and that two later folds left
# behind, which no manifest named, leaves its pack mostly dead, which the worker
# copies out and retires once collector_first_packs falls to 1.
$node->safe_psql('postgres', 'CREATE DATABASE other');
my $other = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'other'");
$node->safe_psql('other', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('other', 'CREATE TABLE gone (k int4 PRIMARY KEY, v text)');
$node->safe_psql('other',
	"SELECT doltgres_test_write('gone', true, 'INSERT INTO gone SELECT g, repeat(md5(g::text), 20) FROM generate_series(1, 500) g')");
$node->safe_psql('other', "SELECT doltgres_test_fold('gone')");
my @written = glob($node->data_dir . "/pg_versioned/$other/*.pack");
$node->safe_psql('other',
	"SELECT doltgres_test_write('gone', true, 'DELETE FROM gone')");
$node->safe_psql('other', "SELECT doltgres_test_fold('gone')");
$node->safe_psql('other',
	"SELECT doltgres_test_write('gone', true, 'INSERT INTO gone VALUES (1, ''kept'')')");
$node->safe_psql('other', "SELECT doltgres_test_fold('gone')");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'ALTER SYSTEM SET collector_first_packs = 1');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
my $waited = 0;
while (!grep { !-e $_ } @written)
{
	die 'the collector worker never collected the other database'
	  if $waited++ > 10 * $PostgreSQL::Test::Utils::timeout_default;
	Time::HiRes::usleep(100_000);
}
ok(1, "the collector worker collects every database's repository on its own");
is($node->safe_psql('other',
	"SELECT doltgres_test_standing('gone', 'SELECT k || '' '' || v FROM gone')"), '1 kept',
	'the row the worker\'s collection kept reads');
$node->safe_psql('postgres', 'ALTER SYSTEM RESET collector_first_packs');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');

# dead_base writes a table of 500 rows of poorly compressible text and folds it,
# returning the packs then listed, and then leaves that base behind with a
# delete of every row and a fold, keeping the one written next.
sub dead_base
{
	my ($table, $before_delete) = @_;
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 PRIMARY KEY, v text)");
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', true, 'INSERT INTO $table SELECT g, repeat(md5(g::text), 20) FROM generate_series(1, 500) g')");
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	my @listed = glob($node->data_dir . "/pg_versioned/$database/*.pack");
	$before_delete->() if $before_delete;
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', true, 'DELETE FROM $table')");
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	return @listed;
}

# removed waits until one of the files given is gone.
sub removed
{
	my @files = @_;
	my $waited = 0;
	while (!grep { !-e $_ } @files)
	{
		return 0 if $waited++ > 10 * $PostgreSQL::Test::Utils::timeout_default;
		$node->safe_psql('postgres', 'SELECT doltgres_test_collect()');
		Time::HiRes::usleep(100_000);
	}
	return 1;
}

# The collector text's §10: a REPEATABLE READ snapshot taken before a fold holds
# the base it replaced, the state's predecessor, whose chunks a collection keeps
# while the snapshot reads them; once the snapshot ends and a later fold leaves
# that base behind, a collection takes them.
my $snapshot = $node->background_psql('postgres');
my @held = dead_base('held',
	sub {
		$snapshot->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
		$snapshot->query_safe("SELECT doltgres_test_standing('held', 'SELECT count(*) FROM held')");
	});
like($node->safe_psql('postgres', 'SELECT doltgres_test_collect()'), qr/^\d+ \d+ \d+$/,
	'a collection runs while a snapshot holds a predecessor base');
is($snapshot->query_safe(
		"SELECT doltgres_test_standing('held', 'SELECT count(*) || '' '' || sum(length(v)) FROM held')"),
	'500 320000',
	'a snapshot reads the predecessor base after a collection');
$snapshot->query_safe('COMMIT');
$snapshot->quit;
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('held', true, 'INSERT INTO held VALUES (1, ''kept'')')");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('held')");
$node->safe_psql('postgres', 'CHECKPOINT');
ok(removed(@held), 'a collection takes a predecessor base once its snapshot ends and a fold leaves it behind');
is($node->safe_psql('postgres', "SELECT doltgres_test_standing('held', 'SELECT k || '' '' || v FROM held')"),
	'1 kept',
	'the row after the predecessor base reads');

# The collector text's §10: a delta version naming an out-of-line value, not yet
# folded, keeps the value through a collection.
$node->safe_psql('postgres', 'CREATE TABLE valued (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres', 'ALTER TABLE valued ALTER COLUMN v SET STORAGE EXTERNAL');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('valued', true, 'INSERT INTO valued SELECT 1, string_agg(md5(g::text), '''') FROM generate_series(1, 2000) g')");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'SELECT doltgres_test_collect()');
is($node->safe_psql('postgres',
		"SELECT doltgres_test_standing('valued', 'SELECT length(v) || '' '' || md5(v) FROM valued')"),
	$node->safe_psql('postgres', "SELECT '64000 ' || md5(string_agg(md5(g::text), '')) FROM generate_series(1, 2000) g"),
	'an out-of-line value of an unfolded row reads after a collection');

# The collector text's §6: a crash between a collection's retirement and the
# queue of its files (an injection point and an immediate stop) leaves the
# files listed nowhere; the next collection lists them again, retires what
# nothing reaches, and every row reads.
my @crashed = dead_base('crashed');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('crashed', true, 'INSERT INTO crashed VALUES (1, ''kept'')')");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('crashed')");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-collector-retired', 'wait')");
my $asker = $node->background_psql('postgres', on_error_stop => 0);
$asker->query_until(qr/start/, "\\echo start\nSELECT doltgres_test_collect();\n");
$node->wait_for_event('versioned collector', 'doltgres-collector-retired');
$node->stop('immediate');
$asker->quit;
$node->start;
ok(removed(@crashed), 'the collection after a crash between retirement and queue removes the dead packs');
is($node->safe_psql('postgres', "SELECT doltgres_test_standing('crashed', 'SELECT k || '' '' || v FROM crashed')"),
	'1 kept',
	'the rows read after a crash between retirement and queue');
is($node->safe_psql('postgres',
	"SELECT doltgres_test_standing('raced', 'SELECT count(*) || '' '' || sum(length(v)) FROM raced')"),
	'40 820', 'the other tables read after a crash between retirement and queue');

# The collector text's §8.2: an ask canceled while the worker collects for it
# is taken back, the collection going on, and a later ask is answered.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-collector-batch', 'wait')");
my $canceled = $node->background_psql('postgres', on_error_stop => 0);
$canceled->query_until(qr/start/, "\\echo start\nSELECT doltgres_test_collect();\n");
$node->wait_for_event('versioned collector', 'doltgres-collector-batch');
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'versioned collection ask'")
  or die 'the ask never waited for the worker';
$node->safe_psql('postgres',
	"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'versioned collection ask'");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'versioned collection ask'")
  or die 'the canceled ask never ended';
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-collector-batch')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-collector-batch')");
$canceled->quit;
like($node->safe_psql('postgres', 'SELECT doltgres_test_collect()'), qr/^\d+ \d+ \d+$/,
	'a canceled ask is taken back and a later ask is answered');

# PRevPF 8's K2: a value folded into its TOAST storage's base, its map's delta
# drained by the folds after, is named by the base's address table, so a full
# collection keeps its blob and the value reads.
my $toast = $node->safe_psql('postgres',
	"SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'valued'");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('valued')");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('$toast')") for 1 .. 3;
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'SELECT doltgres_test_collect()');
is($node->safe_psql('postgres',
		"SELECT doltgres_test_standing('valued', 'SELECT length(v) || '' '' || md5(v) FROM valued')"),
	$node->safe_psql('postgres', "SELECT '64000 ' || md5(string_agg(md5(g::text), '')) FROM generate_series(1, 2000) g"),
	'a value folded into its TOAST storage reads after a full collection');

$node->stop;

# The collector's worker takes a slot reserved for the fork, beside
# max_worker_processes: with one worker process and no logical replication
# launcher, the collector runs and a parallel query still launches the one
# worker stock Postgres gives it.
my $few = PostgreSQL::Test::Cluster->new('few');
$few->init;
$few->append_conf('postgresql.conf',
	"max_worker_processes = 1\nmax_logical_replication_workers = 0");
$few->start;
$few->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'versioned collector'")
  or die 'the collector never started at one worker process';
like($few->safe_psql('postgres',
		"SET debug_parallel_query = on; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM pg_class"),
	qr/Workers Launched: 1/,
	'at one worker process the collector runs and a parallel query launches its worker');
$few->stop;

# The collector registers before shared_preload_libraries' workers, in its own
# slot: an extension registering max_worker_processes static workers starts
# them all beside the collector, and one more is refused with stock's message.
my $spi = PostgreSQL::Test::Cluster->new('spi');
$spi->init;
$spi->append_conf('postgresql.conf',
	"max_worker_processes = 2\nmax_logical_replication_workers = 0\n"
	  . "shared_preload_libraries = 'worker_spi'\nworker_spi.total_workers = 2\nlog_min_messages = debug1");
$spi->start;
$spi->wait_for_log(qr/starting background worker process "worker_spi worker 2"/);
$spi->wait_for_log(qr/starting background worker process "versioned collector"/);
ok(!$spi->log_contains('too many background workers'),
	'an extension registering max_worker_processes static workers starts them all beside the collector');
$spi->stop;
$spi->append_conf('postgresql.conf', 'worker_spi.total_workers = 3');
my $offset = -s $spi->logfile;
$spi->start;
$spi->wait_for_log(qr/starting background worker process "versioned collector"/, $offset);
$spi->wait_for_log(qr/starting background worker process "worker_spi worker 2"/, $offset);
ok( $spi->log_contains(
		'Up to 2 background workers can be registered with the current settings.', $offset)
	  && !$spi->log_contains('starting background worker process "worker_spi worker 3"', $offset),
	"an extension's static worker past max_worker_processes is refused with stock's message, the collector started");
$spi->stop;

# Bugs row 119: tables of the same rows under other stamps hold records of one
# address that differ in their stamps, so a marking that names such a chunk by
# its address alone marks every record of it; a full collection completes, and
# every table reads as its heap twin afterward.
my $identical = PostgreSQL::Test::Cluster->new('identical');
$identical->init;
$identical->start;
$identical->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
my $rows = "INSERT INTO %s SELECT g, repeat('i', g * 20) FROM generate_series(1, 300) g";
for my $table ('same_a', 'same_b', 'same_c')
{
	$identical->safe_psql('postgres',
		"CREATE TABLE $table (k int4 PRIMARY KEY, v text); CREATE TABLE heap_$table (k int4 PRIMARY KEY, v text)");
	$identical->safe_psql('postgres', "SELECT doltgres_test_write('$table', true, \$\$" . sprintf($rows, $table) . "\$\$)");
	$identical->safe_psql('postgres', sprintf($rows, "heap_$table"));
	$identical->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	$identical->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', true, \$\$UPDATE $table SET v = v WHERE k % 7 = 0\$\$)");
	$identical->safe_psql('postgres', "UPDATE heap_$table SET v = v WHERE k % 7 = 0");
	$identical->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
}
$identical->safe_psql('postgres', 'CHECKPOINT');
like($identical->safe_psql('postgres', 'SELECT doltgres_test_collect()'), qr/^\d+ \d+ \d+$/,
	'a full collection completes on a repository of tables of the same rows under other stamps');
my $read = "SELECT count(*) || ' ' || md5(string_agg(k || ':' || v, ',' ORDER BY k)) FROM %s";
for my $table ('same_a', 'same_b', 'same_c')
{
	is( $identical->safe_psql('postgres',
			"SELECT doltgres_test_standing('$table', \$\$" . sprintf($read, $table) . "\$\$)"),
		$identical->safe_psql('postgres', sprintf($read, "heap_$table")),
		"$table reads as its heap twin after the collection");
}
$identical->stop;

# CommPF 8's ruling on IdxPF 1's Windows stall: an immediate stop ends the
# server at once while the collector worker fails a marking, at the error and
# again as the postmaster starts the worker anew, as Postgres' own workers die
# at once under it. Each round folds a row first, so its marking has a root to
# reach and fails at its first batch, after a crash restart too; and after the
# restarts a full collection keeps every row.
my $stopping = PostgreSQL::Test::Cluster->new('stopping');
$stopping->init;
$stopping->append_conf('postgresql.conf', 'log_min_messages = debug1');
$stopping->start;
$stopping->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CREATE EXTENSION injection_points');
$stopping->safe_psql('postgres', 'CREATE TABLE stopped (k int4 PRIMARY KEY, v text)');
$stopping->safe_psql('postgres',
	"SELECT doltgres_test_write('stopped', true, 'INSERT INTO stopped SELECT g, g::text FROM generate_series(1, 50) g')");
my $round = 0;
for my $moment ('at its marking error', 'as the worker starts again')
{
	$round++;
	$stopping->safe_psql('postgres',
		"SELECT doltgres_test_write('stopped', true, 'INSERT INTO stopped VALUES (100 + $round, ''r'')')");
	$stopping->safe_psql('postgres', "SELECT doltgres_test_fold('stopped')");
	$stopping->safe_psql('postgres', "SELECT injection_points_attach('doltgres-collector-batch', 'error')");
	my $offset = -s $stopping->logfile;
	my ($failed) = $stopping->psql('postgres', 'SELECT doltgres_test_collect()');
	isnt($failed, 0, "the ask's marking fails at its first batch $moment");
	$stopping->wait_for_log(qr/background worker "versioned collector" \(PID \d+\) exited with exit code 1/, $offset);
	$stopping->wait_for_log(qr/starting background worker process "versioned collector"/, -s $stopping->logfile)
	  if $moment eq 'as the worker starts again';
	my $started = [Time::HiRes::gettimeofday()];
	$stopping->stop('immediate');
	cmp_ok(Time::HiRes::tv_interval($started), '<', 4, "an immediate stop ends the server at once $moment");
	$stopping->start;
}
like($stopping->safe_psql('postgres', 'SELECT doltgres_test_collect()'), qr/^\d+ \d+ \d+$/,
	'a full collection after the crash restarts completes');
is($stopping->safe_psql('postgres', "SELECT doltgres_test_standing('stopped', 'SELECT count(*) FROM stopped')"),
	'52', 'every row reads after a full collection that follows a crash restart');
$stopping->stop;

# The anomaly CommPF 8 held the collector for: a fold's base record is logged
# after its point's flush, and a fold's transaction commits with no xid, so a
# crash before the WAL writer flushes the record may lose it. Recovery then
# either replays the fold, when its records reached disk, into a base that may
# name its packs, or finds it gone and replays the rows into the delta, leaving
# the fold's packs named by nothing. Either outcome holds the same invariants: a
# full collection retires what nothing names and keeps what the manifest names,
# the identity tree a checkpoint before the fold saved; a second full
# collection finds nothing to retire or copy; the fold's packs are gone unless a
# replayed base names them; and every row reads back, after a restart too.
# Recovery settles the outcome before the server accepts connections, and
# fold_threshold and fold_timeout keep the fold workers from folding the table
# again between the collections.
my $unflushed = PostgreSQL::Test::Cluster->new('unflushed');
$unflushed->init;
$unflushed->append_conf('postgresql.conf',
	"wal_writer_delay = 10000\nfold_threshold = 1GB\nfold_timeout = 3600s");
$unflushed->start;
$unflushed->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CREATE EXTENSION injection_points');
$unflushed->safe_psql('postgres', 'CREATE TABLE unflushed (k int4 PRIMARY KEY, v text)');
$unflushed->safe_psql('postgres',
	"SELECT doltgres_test_write('unflushed', true, 'INSERT INTO unflushed SELECT g, g::text FROM generate_series(1, 50) g')");
my $unflushed_database =
  $unflushed->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $unflushed_packs = sub {
	return map { s{.*/}{}r } glob($unflushed->data_dir . "/pg_versioned/$unflushed_database/*.pack");
};
$unflushed->safe_psql('postgres', 'CHECKPOINT');
my %before_fold = map { $_ => 1 } $unflushed_packs->();
is( $unflushed->safe_psql('postgres',
		"SELECT doltgres_test_fold('unflushed'); SELECT pg_current_wal_flush_lsn() < pg_current_wal_insert_lsn()"),
	"t\nt", "the fold's base record is not yet flushed");
my @fold_packs = grep { !$before_fold{$_} } $unflushed_packs->();
my $count = "SELECT doltgres_test_standing('unflushed', 'SELECT count(*) FROM unflushed')";
is($unflushed->safe_psql('postgres', $count), '50', 'every row reads before the crash');
$unflushed->stop('immediate');
$unflushed->start;
my ($retired, $copied, $live) = split / /, $unflushed->safe_psql('postgres', 'SELECT doltgres_test_collect()');
is($copied, '0', "a full collection after a crash that may have lost a fold's base record copies nothing");
is($unflushed->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	"0 0 $live", 'a second full collection finds nothing to retire or copy, so nothing leaks');
$unflushed->restart;
my %left = map { $_ => 1 } $unflushed_packs->();
my $based = $unflushed->safe_psql('postgres', "SELECT doltgres_test_base_sizes('unflushed')") !~ /^0 /;
ok(!(grep { $left{$_} } @fold_packs) || $based,
	"the fold's original packs are gone after the restart unless recovery replayed the fold into a base that names them"
);
$unflushed->safe_psql('postgres', 'CHECKPOINT');
is($unflushed->safe_psql('postgres', q{SELECT doltgres_test_identity(2, 'unflushed'::regclass::oid, 0) <> 'none'}),
	't', 'the identity tree the manifest names survives the collection');
is($unflushed->safe_psql('postgres', $count), '50', 'every row reads after the collection and a restart');
$unflushed->stop;

# ALTER DATABASE SET TABLESPACE and DROP DATABASE while the collector worker
# marks the database's repository under the database's lock: each command
# waits for that lock, the marking, released from its injection point, stops at
# the batch for the waiter, the worker closes the repository and gives the lock
# back, and the command runs with the repository closed: the moved database
# reads its rows, and the dropped one's repository is gone at once with its
# tablespace's directory of repositories, so the tablespace drops right after,
# as in Postgres.
my $dropping = PostgreSQL::Test::Cluster->new('dropping');
$dropping->init;
$dropping->start;
$dropping->safe_psql('postgres', 'CREATE EXTENSION injection_points');
my ($far, $near) = (PostgreSQL::Test::Utils::tempdir_short(), PostgreSQL::Test::Utils::tempdir_short());
$dropping->safe_psql('postgres', "CREATE TABLESPACE far LOCATION '$far'; CREATE TABLESPACE near LOCATION '$near'");
$dropping->safe_psql('postgres', 'CREATE DATABASE marked TABLESPACE near');
$dropping->safe_psql('marked', 'CREATE EXTENSION doltgres_test; CREATE TABLE kept (k int4 PRIMARY KEY, v text)');
$dropping->safe_psql('marked',
	"SELECT doltgres_test_write('kept', true, 'INSERT INTO kept SELECT g, g::text FROM generate_series(1, 50) g')");
$dropping->safe_psql('marked', "SELECT doltgres_test_fold('kept')");

# paused runs a statement in the postgres database while the worker's marking
# of the database's repository waits at its first batch, and returns once the
# statement, having waited for the database's lock, has run.
sub paused
{
	my ($statement) = @_;
	$dropping->safe_psql('postgres', "SELECT injection_points_attach('doltgres-collector-batch', 'wait')");
	my $asker = $dropping->background_psql('marked', on_error_stop => 0);
	$asker->query_until(qr/start/, "\\echo start\nSELECT doltgres_test_collect();\n");
	$dropping->wait_for_event('versioned collector', 'doltgres-collector-batch');
	$dropping->safe_psql('postgres',
		"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'versioned collection ask'");
	$asker->quit;
	$dropping->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE datname = 'marked'")
	  or die 'a session of the database stayed';
	my $runner = $dropping->background_psql('postgres');
	$runner->query_until(qr/start/, "\\echo start\n$statement;\n");
	$dropping->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE query = '$statement;' AND wait_event_type = 'Lock'")
	  or die "$statement never waited for the database's lock";
	$dropping->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-collector-batch'); SELECT injection_points_wakeup('doltgres-collector-batch')"
	);
	$runner->quit;
}

# PRevPF 8's L1: a worker ended while its marking holds the database's lock
# gives the lock back as it exits, so the server goes on with no crash and the
# move and the drop below don't wait for it.
$dropping->safe_psql('postgres', "SELECT injection_points_attach('doltgres-collector-batch', 'wait')");
my $held = $dropping->background_psql('marked', on_error_stop => 0);
$held->query_until(qr/start/, "\\echo start\nSELECT doltgres_test_collect();\n");
$dropping->wait_for_event('versioned collector', 'doltgres-collector-batch');
$dropping->safe_psql('postgres',
	"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'versioned collection ask'");
$held->quit;
my $ended_offset = -s $dropping->logfile;
$dropping->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-collector-batch'); SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'versioned collector'"
);
$dropping->wait_for_log(qr/background worker "versioned collector" \(PID \d+\) exited with exit code 1/, $ended_offset);
ok( !$dropping->log_contains('terminated by signal', $ended_offset)
	  && !$dropping->log_contains('TRAP', $ended_offset),
	"a worker ended while its marking holds a database's lock exits without a crash");

my $moved_offset = -s $dropping->logfile;
paused('ALTER DATABASE marked SET TABLESPACE far');
is($dropping->safe_psql('marked', "SELECT doltgres_test_standing('kept', 'SELECT count(*) FROM kept')"),
	'50', "a database moved while the collector marks its repository reads its rows");
ok(!$dropping->log_contains('could not open file', $moved_offset), 'and the worker never opens the moving repository');
paused('DROP DATABASE marked');
my ($far_version) = glob("$far/PG_*");
ok(!-e "$far_version/pg_versioned",
	"a database dropped while the collector marks its repository has it removed at once");
$dropping->safe_psql('postgres', 'DROP TABLESPACE far');
pass('its tablespace drops right after');
$dropping->stop;

# The replay of a database's drop takes the database's lock in the startup
# process, as the redo of Postgres' own drop does, so a hot standby's collector
# worker, paused mid-marking with that lock held, makes the replay wait, stops
# its marking for the waiter once released, and the replay then removes the
# repository with nothing open on it, so a database made after it replays.
my $upstream = PostgreSQL::Test::Cluster->new('upstream');
$upstream->init(allows_streaming => 1);
$upstream->append_conf('postgresql.conf', 'allow_in_place_tablespaces = on');
$upstream->start;
$upstream->safe_psql('postgres', 'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test');
$upstream->safe_psql('postgres', "CREATE TABLESPACE first_place LOCATION ''");
for my $database ('replay_dropped')
{
	$upstream->safe_psql('postgres', "CREATE DATABASE $database TABLESPACE first_place");
	$upstream->safe_psql($database, 'CREATE EXTENSION doltgres_test; CREATE TABLE kept (k int4 PRIMARY KEY, v text)');
	$upstream->safe_psql($database,
		"SELECT doltgres_test_write('kept', true, 'INSERT INTO kept SELECT g, g::text FROM generate_series(1, 50) g')");
	$upstream->safe_psql($database, "SELECT doltgres_test_fold('kept')");
}
$upstream->backup('upstream_backup');
my $downstream = PostgreSQL::Test::Cluster->new('downstream');
$downstream->init_from_backup($upstream, 'upstream_backup', has_streaming => 1);
$downstream->append_conf('postgresql.conf', 'max_standby_streaming_delay = -1');
$downstream->start;
$upstream->wait_for_replay_catchup($downstream);

# replayed runs a statement on the primary while the standby's worker marks a
# database's repository with its lock held, and returns once the standby's
# replay, having waited for that lock, has caught up.
sub replayed
{
	my ($database, $statement) = @_;
	$downstream->safe_psql('postgres', "SELECT injection_points_attach('doltgres-collector-batch', 'wait')");
	my $asker = $downstream->background_psql($database, on_error_stop => 0);
	$asker->query_until(qr/start/, "\\echo start\nSELECT doltgres_test_collect();\n");
	$downstream->wait_for_event('versioned collector', 'doltgres-collector-batch');
	$downstream->safe_psql('postgres',
		"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'versioned collection ask'");
	$asker->quit;
	$upstream->safe_psql('postgres', $statement);
	$downstream->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'startup' AND wait_event_type = 'Lock'")
	  or die "the replay of $statement never waited for the database's lock";
	$downstream->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-collector-batch'); SELECT injection_points_wakeup('doltgres-collector-batch')"
	);
	$upstream->wait_for_replay_catchup($downstream);
}

my $replay_offset = -s $downstream->logfile;
replayed('replay_dropped', 'DROP DATABASE replay_dropped');
ok(!$downstream->log_contains('FATAL', $replay_offset),
	"a standby replays a database's drop while its collector marks the repository, with no FATAL");
$upstream->safe_psql('postgres', 'CREATE DATABASE replay_again TABLESPACE first_place');
$upstream->wait_for_replay_catchup($downstream);
ok(!$downstream->log_contains('opened after the repository was dropped', $replay_offset),
	"and a database made after it replays");

# The replay of a move's copy takes the database's lock before it copies: with a
# standby session holding the lock the worker takes, as a check would, the
# replay of an empty database's move waits with no copy made, and once the lock
# is given back the move replays and the repository stands in its new
# tablespace alone.
$upstream->safe_psql('postgres', "CREATE TABLESPACE second_place LOCATION ''");
$upstream->safe_psql('postgres', 'CREATE DATABASE moved_empty TABLESPACE first_place');
$upstream->wait_for_replay_catchup($downstream);
my $empty_oid = $upstream->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = 'moved_empty'");
my %place_oid = map {
	$_ => $upstream->safe_psql('postgres', "SELECT oid FROM pg_tablespace WHERE spcname = '$_'")
} ('first_place', 'second_place');
my $holder = $downstream->background_psql('postgres');
is($holder->query_safe("SELECT doltgres_test_database_lock(${empty_oid}::oid, true)"),
	't', "a standby session holds the database's lock the collector worker takes");
$upstream->safe_psql('postgres', 'ALTER DATABASE moved_empty SET TABLESPACE second_place');
$downstream->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'startup' AND wait_event_type = 'Lock'")
  or die "the replay of the move never waited for the database's lock";
my %place = map { $_ => (glob($downstream->data_dir . "/pg_tblspc/$place_oid{$_}/PG_*"))[0] } keys %place_oid;
ok(!-e "$place{second_place}/pg_versioned/$empty_oid.moving" && !-e "$place{second_place}/pg_versioned/$empty_oid",
	"a standby's replay of a move's copy waits for the database's lock before it copies");
$holder->query_safe("SELECT doltgres_test_database_lock(${empty_oid}::oid, false)");
$holder->quit;
$upstream->wait_for_replay_catchup($downstream);
ok(-f "$place{second_place}/pg_versioned/$empty_oid/table" && !-e "$place{first_place}/pg_versioned/$empty_oid",
	"and once the lock is given back the move replays into the new tablespace");
$downstream->stop;
$upstream->stop;

# The collector worker's resource owner: a collection's capture of a storage's
# roots reads each delta key's versions, which consults a committed spill's
# filter through the node cache, whose pins a resource owner names, so a
# collection over a storage with a spill completes and its rows read back.
my $spilled = PostgreSQL::Test::Cluster->new('spilled');
$spilled->init;
$spilled->append_conf('postgresql.conf', "fold_threshold = 1GB\nfold_timeout = 3600s");
$spilled->start;
$spilled->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE source_some (v text);
INSERT INTO source_some SELECT repeat(md5(i::text), 31) FROM generate_series(1, 6000) i;
CREATE TABLE source_one (v text);
INSERT INTO source_one VALUES ('one');
CREATE TABLE spilled (v text);
SELECT count(*) FROM doltgres_test_insert('spilled', 'source_some', true, 0);
SELECT count(*) FROM doltgres_test_insert('spilled', 'source_one', true, 0);
});
like($spilled->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	qr/^\d+ \d+ \d+$/, "a collection over a storage with a committed spill completes");
is($spilled->safe_psql('postgres', q{SELECT count(*) FROM doltgres_test_scan('spilled', false, -1)}),
	'6001', 'and every row reads back after it');
$spilled->stop;

done_testing();
