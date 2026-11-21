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

# A carry whose count's add fails takes its files back out (bugs row 291's
# review R2): a VACUUM FULL carries three writers' recently dead versions,
# which an older snapshot keeps, into spill files under each writer, and the
# add of the second file's count fails; the carry takes its files back out and
# gives back the first add, so every writer's count is where it was, and a
# restart replays the carry's records and its unlist to nothing. A failure
# inside the unlist's window stops the server, and the restart's recount
# leaves every count at the versions its storages hold.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('carrier');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nrestart_after_crash = on\ncheckpoint_timeout = 1h"
);
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION doltgres_test;
CREATE TABLE carried (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50);
INSERT INTO carried SELECT g, repeat('c', 20) FROM generate_series(1, 30) g;
SELECT doltgres_test_fold('carried');
});

# Every server takes no checkpoint or restartpoint by time: a standby's
# restartpoint moves the writers' counts while the rounds compare them.
# counts returns each writer's entry's count on a server, the primary when
# none is given, -1 for one with none.
sub counts
{
	my ($writers, $on) = @_;
	$on //= $node;
	return join ' ', map {
		$on->safe_psql('postgres',
			"SELECT doltgres_test_status_table(1, $_, 0)")
	} @$writers;
}

# updated runs three updates, each its own transaction, while an older
# snapshot holds their older versions, and returns the writers' xids.
sub updated
{
	my ($holder) = @_;
	my @writers;
	for my $n (1 .. 3)
	{
		push @writers,
		  $node->safe_psql('postgres',
				"BEGIN; UPDATE carried SET v = v || 'u' WHERE mod(k, 3) = "
			  . ($n - 1)
			  . "; SELECT txid_current(); COMMIT;");
	}
	return \@writers;
}

my $holder = $node->background_psql('postgres');
$holder->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
my $writers = updated($holder);
my $before = counts($writers);

$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-carry-counting', 'wait')");
my $rewriter = $node->background_psql('postgres', on_error_stop => 0);
$rewriter->query_until(qr/rewriting/,
	"\\echo rewriting\nVACUUM FULL carried;\n");
$node->wait_for_event('client backend', 'doltgres-carry-counting');
$node->safe_psql(
	'postgres', q{
SELECT injection_points_detach('doltgres-carry-counting');
SELECT injection_points_attach('doltgres-carry-counting', 'error');
SELECT injection_points_wakeup('doltgres-carry-counting');
});
$rewriter->quit;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-carry-counting')");
is(counts($writers), $before,
	"a carry whose second count's add fails gives back the first, every writer's count where it was"
);
$holder->query_safe('COMMIT');
$holder->quit;

$node->restart;
is(counts($writers), $before,
	"a restart replays the carry's records and its unlist to nothing");
is($node->safe_psql('postgres', 'SELECT count(*) FROM carried'),
	'30',
	"the table's rows read back after the failed carry and the restart");

# A concurrent build's late entries (bugs row 291's review R2): CREATE INDEX
# CONCURRENTLY carries the entries of rows whose writers are past its horizon,
# which an older snapshot holds, into spill files under each writer, in an
# index whose storage an earlier transaction made, so the build's abort
# drops nothing. With the second count's add failed, the build takes its
# files back out and logs the unlist, and a standby that spilled the carried
# entries, too large for its delta_memory, before the unlist arrived takes
# them out of its spill files too: its delta of the index holds its primary's
# entries, and every writer's count is where it was.
$node->backup('late');
my $standby = PostgreSQL::Test::Cluster->new('late_standby');
$standby->init_from_backup($node, 'late', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$standby->start;
$node->safe_psql('postgres', 'CREATE TABLE late (k int4, v text)');
$holder = $node->background_psql('postgres');
$holder->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
my @late_writers;

for my $n (0 .. 2)
{
	my $from = 3000 * $n + 1;
	push @late_writers,
	  $node->safe_psql('postgres',
		"BEGIN; INSERT INTO late SELECT g, md5(g::text) || repeat('l', 200) FROM generate_series($from, $from + 2999) g; SELECT txid_current(); COMMIT;"
	  );
}
$before = counts(\@late_writers);
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-carry-counting', 'wait')");
my $builder = $node->background_psql('postgres', on_error_stop => 0);
$builder->query_until(qr/building/,
	"\\echo building\nCREATE INDEX CONCURRENTLY late_v ON late (v COLLATE \"C\");\n"
);
$node->wait_for_event('client backend', 'doltgres-carry-counting');
$node->safe_psql(
	'postgres', q{
SELECT injection_points_detach('doltgres-carry-counting');
SELECT injection_points_attach('doltgres-carry-counting', 'error');
SELECT injection_points_wakeup('doltgres-carry-counting');
});
$builder->quit;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-carry-counting')");
$holder->query_safe('COMMIT');
$holder->quit;
is(counts(\@late_writers), $before,
	"a concurrent build whose second count's add fails gives back the first, every writer's count where it was"
);
$node->wait_for_replay_catchup($standby);
is(counts(\@late_writers, $standby),
	$before,
	"the standby replays the unlist, every writer's count where it was");
is( $standby->safe_psql(
		'postgres', "SELECT md5(doltgres_test_index_entries('late_v'))"),
	$node->safe_psql(
		'postgres', "SELECT md5(doltgres_test_index_entries('late_v'))"),
	"the standby's delta of the index holds its primary's entries, the carried ones out"
);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM late'),
	'9000', "the standby reads the table whole");
$standby->stop;

# A plain rewrite's failed carry on a standby (bugs row 291's review G1): a
# VACUUM FULL carries three writers' recently dead rows, with their tuple ids
# and their puts' takes, which an older snapshot holds, and the add of the
# second file's count fails; a standby that replayed the carried rows takes
# them back out at the unlist, so every writer's count there is where it was.
# The new storage itself goes with the rewrite's abort, on the standby too.
# The standby has the default delta_memory: at 1MB its replay of so wide a
# carry stalls at the budget (bugs rows 332 and 304).
$node->backup('wide');
my $wide_standby = PostgreSQL::Test::Cluster->new('wide_standby');
$wide_standby->init_from_backup($node, 'wide', has_streaming => 1);
$wide_standby->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nallow_in_place_tablespaces = on\ncheckpoint_timeout = 1h"
);
$wide_standby->start;
$node->safe_psql(
	'postgres', q{
CREATE TABLE carried_wide (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50);
INSERT INTO carried_wide SELECT g, repeat('w', 600) FROM generate_series(1, 3000) g;
SELECT doltgres_test_fold('carried_wide');
});
$node->wait_for_replay_catchup($wide_standby);
$holder = $node->background_psql('postgres');
$holder->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
my @wide_writers;

for my $n (0 .. 2)
{
	push @wide_writers,
	  $node->safe_psql('postgres',
		"BEGIN; UPDATE carried_wide SET v = v || 'u' WHERE mod(k, 3) = $n; SELECT txid_current(); COMMIT;"
	  );
}
$before = counts(\@wide_writers);
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-carry-counting', 'wait')");
$rewriter = $node->background_psql('postgres', on_error_stop => 0);
$rewriter->query_until(qr/widening/,
	"\\echo widening\nVACUUM FULL carried_wide;\n");
$node->wait_for_event('client backend', 'doltgres-carry-counting');
$node->safe_psql(
	'postgres', q{
SELECT injection_points_detach('doltgres-carry-counting');
SELECT injection_points_attach('doltgres-carry-counting', 'error');
SELECT injection_points_wakeup('doltgres-carry-counting');
});
$rewriter->quit;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-carry-counting')");
$holder->query_safe('COMMIT');
$holder->quit;
is(counts(\@wide_writers), $before,
	"a wide table's carry whose second count's add fails gives back the first, every writer's count where it was"
);
$node->wait_for_replay_catchup($wide_standby);
is( counts(\@wide_writers, $wide_standby),
	$before,
	"a standby that replayed the carried rows takes them out at the unlist, every writer's count where it was"
);
is( $wide_standby->safe_psql(
		'postgres', 'SELECT count(*), sum(length(v)) FROM carried_wide'),
	$node->safe_psql(
		'postgres', 'SELECT count(*), sum(length(v)) FROM carried_wide'),
	"the standby reads the wide table as its primary does");

# A move's failed carry on a standby (bugs row 291's review G2): ALTER TABLE
# SET TABLESPACE carries its old storage's versions, with the free space map
# versions three writers' updates made, which an older snapshot holds, and
# the add of the second file's count fails; the standby takes out each
# carried map version by its record's end, as well as the carried rows'
# takes, so every writer's count there is where it was.
$node->safe_psql(
	'postgres', q{
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE moved_space LOCATION '';
CREATE TABLE carried_moved (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50);
INSERT INTO carried_moved SELECT g, repeat('m', 600) FROM generate_series(1, 3000) g;
SELECT doltgres_test_fold('carried_moved');
});
$holder = $node->background_psql('postgres');
$holder->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
my @moved_writers;
for my $n (0 .. 2)
{
	push @moved_writers,
	  $node->safe_psql('postgres',
		"BEGIN; UPDATE carried_moved SET v = v || 'u' WHERE mod(k, 3) = $n; SELECT txid_current(); COMMIT;"
	  );
}
$before = counts(\@moved_writers);
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-carry-counting', 'wait')");
$rewriter = $node->background_psql('postgres', on_error_stop => 0);
$rewriter->query_until(qr/moving/,
	"\\echo moving\nALTER TABLE carried_moved SET TABLESPACE moved_space;\n");
$node->wait_for_event('client backend', 'doltgres-carry-counting');
$node->safe_psql(
	'postgres', q{
SELECT injection_points_detach('doltgres-carry-counting');
SELECT injection_points_attach('doltgres-carry-counting', 'error');
SELECT injection_points_wakeup('doltgres-carry-counting');
});
$rewriter->quit;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-carry-counting')");
$holder->query_safe('COMMIT');
$holder->quit;
is(counts(\@moved_writers), $before,
	"a move's carry whose second count's add fails gives back the first, every writer's count where it was"
);
$node->wait_for_replay_catchup($wide_standby);
is( counts(\@moved_writers, $wide_standby),
	$before,
	"a standby that replayed the move's carried map versions takes them out at the unlist, every writer's count where it was"
);
$wide_standby->stop;

# A failure inside the unlist's window, Postgres' critical section, stops the
# server after the unlist's record is flushed, and its restart recounts what
# its storages hold.
$holder = $node->background_psql('postgres', on_error_stop => 0);
$holder->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
$writers = updated($holder);
$before = counts($writers);
$node->safe_psql(
	'postgres', q{
SELECT injection_points_attach('doltgres-carry-counting', 'error');
SELECT injection_points_attach('doltgres-carry-unlisting', 'error');
});
my ($ret, $stdout, $stderr) = $node->psql('postgres', 'VACUUM FULL carried');
like(
	$stderr,
	qr/server closed the connection unexpectedly|terminating connection/,
	"a failure inside the unlist's window stops the server, as a PANIC does");
ok($node->poll_query_until('postgres', 'SELECT 1', '1'),
	"the server restarts after the PANIC");
# The stopped rewrite's new storage is an orphan its crashed transaction left,
# as Postgres leaves an orphaned relation file, whose drop with its counts is
# bugs row 322's; the recount is what the storages list, so a second restart
# recounts the same.
my $recounted = counts($writers);
$node->restart;
is(counts($writers), $recounted,
	"the restart after a stop inside the window recounts the versions its storages list, as a second restart does"
);
is($node->safe_psql('postgres', 'SELECT count(*) FROM carried'),
	'30', "the table's rows read back after the stop and the restart");

done_testing();
