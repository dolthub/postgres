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

# A free space take whose block's entry a spill moved after its caller read
# it finishes on the entry the spill left, not on the one under the memtable
# it read before (bugs row 342), which lost the spilled takes: a VACUUM whose
# block's running writer spills, which checks the entry under its block's
# range, kept locked through its window, before it logs its record and reads
# again outside the window when the entry moved; and a standby's replayed
# leave of the fresh page whose prepare the full budget refuses, the
# startup's relief spilling the replayed writer's takes of that page.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $spilling = PostgreSQL::Test::Cluster->new('vacuum_spilling');
$spilling->init;
$spilling->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
$spilling->start;
$spilling->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test');

# freed makes a table whose block 0 holds a folded delete VACUUM frees.
sub freed
{
	my ($table) = @_;
	$spilling->safe_psql(
		'postgres', qq{
CREATE TABLE $table (k int4, v text) WITH (fillfactor = 50);
INSERT INTO $table VALUES (1, repeat('v', 600)), (2, repeat('v', 600));
DELETE FROM $table WHERE k = 1;
SELECT doltgres_test_fold('$table');
});
}
freed('spilled_block');

# The identity versions the tables' creation committed are saved first, so
# the writer's relief below has only its own trees to spill, not a save of
# them (bugs row 399).
$spilling->safe_psql('postgres', 'SELECT doltgres_test_save()');
my $spilling_writer = $spilling->background_psql('postgres');
$spilling_writer->query_safe(
	"BEGIN; INSERT INTO spilled_block VALUES (3, repeat('v', 600))");
my $spilled_vacuum =
  $spilling->background_psql('postgres', on_error_stop => 0);
$spilled_vacuum->query_safe('SELECT injection_points_set_local()');
$spilled_vacuum->query_safe(
	"SELECT injection_points_attach('doltgres-vacuum-read', 'wait')");
$spilled_vacuum->query_until(qr/started/,
	"\\echo started\nSELECT doltgres_test_vacuum('spilled_block', 'auto');\n"
);
$spilling->wait_for_event('client backend', 'doltgres-vacuum-read');
is( join(
		' ',
		map {
			$spilling_writer->query_safe(
				'SELECT doltgres_test_delta(61, 0, 0)')
		} 1 .. 3),
	'1 1 0',
	"the block's writer spills its rows and then its free space takes while VACUUM waits after its read"
);
$spilling->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-vacuum-read'); SELECT injection_points_wakeup('doltgres-vacuum-read')"
);
$spilled_vacuum->query_safe('SELECT 1');
$spilled_vacuum->quit;
$spilling_writer->query_safe('COMMIT');
$spilling_writer->quit;
freed('unspilled_block');
$spilling->safe_psql(
	'postgres', q{
INSERT INTO unspilled_block VALUES (3, repeat('v', 600));
SELECT doltgres_test_vacuum('unspilled_block', 'auto');
});
my $spilled_entry = q{SELECT doltgres_test_free_space('%s', 0)};
is( $spilling->safe_psql(
		'postgres', sprintf($spilled_entry, 'spilled_block')),
	$spilling->safe_psql(
		'postgres', sprintf($spilled_entry, 'unspilled_block')),
	"VACUUM's give-back after a spill moved its block's takes leaves the entry an unspilled VACUUM leaves"
);

# The entry's move found before VACUUM's window sends it to read the block
# again outside the window, where an error is an ordinary one that fails the
# VACUUM alone, and a later VACUUM gives the place back as an unspilled one.
freed('erred_block');
$spilling_writer = $spilling->background_psql('postgres');
$spilling_writer->query_safe(
	"BEGIN; INSERT INTO erred_block VALUES (3, repeat('v', 600))");
$spilled_vacuum = $spilling->background_psql('postgres', on_error_stop => 0);
$spilled_vacuum->query_safe('SELECT injection_points_set_local()');
$spilled_vacuum->query_safe(
	"SELECT injection_points_attach('doltgres-vacuum-read', 'wait')");
$spilled_vacuum->query_until(qr/started/,
	"\\echo started\nSELECT doltgres_test_vacuum('erred_block', 'auto');\n");
$spilling->wait_for_event('client backend', 'doltgres-vacuum-read');
$spilling_writer->query_safe('SELECT doltgres_test_delta(61, 0, 0)')
  for 1 .. 3;
$spilling->safe_psql(
	'postgres', q{
SELECT injection_points_detach('doltgres-vacuum-read');
SELECT injection_points_attach('doltgres-vacuum-read', 'error');
SELECT injection_points_wakeup('doltgres-vacuum-read');
});
$spilled_vacuum->query('SELECT 1');
$spilling->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-vacuum-read')");
like(
	$spilled_vacuum->{stderr},
	qr/ERROR:  error triggered for injection point doltgres-vacuum-read/,
	"an error at VACUUM's read again after its block's entry moved fails the VACUUM as an ordinary error"
);
$spilled_vacuum->quit;
$spilling_writer->query_safe('COMMIT');
$spilling_writer->quit;
$spilling->safe_psql('postgres',
	"SELECT doltgres_test_vacuum('erred_block', 'auto')");
is( $spilling->safe_psql('postgres', sprintf($spilled_entry, 'erred_block')),
	$spilling->safe_psql(
		'postgres', sprintf($spilled_entry, 'unspilled_block')),
	"a VACUUM after the failed one leaves the entry an unspilled VACUUM leaves"
);

# VACUUM keeps its block's range locked from before its window to its last
# take there, so a spill's take-out of the block's free space takes that
# begins while VACUUM is in its window waits on that lock, which VACUUM
# releases waiting on nothing, and both finish.
# toasted makes a table whose block 0 holds a folded delete of a row with a
# TOAST value, whose take-down VACUUM pauses in its window before.
sub toasted
{
	my ($table) = @_;
	$spilling->safe_psql(
		'postgres', qq{
CREATE TABLE $table (k int4, v text);
ALTER TABLE $table ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO $table VALUES (1, repeat('t', 3000)), (2, repeat('t', 3000));
DELETE FROM $table WHERE k = 1;
SELECT doltgres_test_fold('$table');
});
}
toasted('locked_block');
$spilling_writer = $spilling->background_psql('postgres');
$spilling_writer->query_safe(
	"BEGIN; INSERT INTO locked_block VALUES (3, 'short')");
$spilling_writer->query_safe('SELECT doltgres_test_delta(61, 0, 0)');
$spilled_vacuum = $spilling->background_psql('postgres');
$spilled_vacuum->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-vacuum-logged', 'wait')"
);
$spilled_vacuum->query_until(qr/started/,
	"\\echo started\nSELECT doltgres_test_vacuum('locked_block', 'auto');\n");
$spilling->wait_for_event('client backend', 'doltgres-vacuum-logged');
my $writer_pid = $spilling_writer->query_safe('SELECT pg_backend_pid()');
$spilling_writer->query_until(qr/spilling/,
	"\\echo spilling\nSELECT doltgres_test_delta(61, 0, 0);\n");
ok( $spilling->poll_query_until(
		'postgres',
		"SELECT wait_event_type = 'LWLock' FROM pg_stat_activity WHERE pid = $writer_pid"
	),
	"a spill's take-out of the block's takes waits on the range VACUUM keeps through its window"
);
$spilling->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-vacuum-logged'); SELECT injection_points_wakeup('doltgres-vacuum-logged')"
);
like($spilled_vacuum->query_safe('SELECT 1'),
	qr/^1\z/m, "VACUUM finishes its window");
is($spilling_writer->query_safe('SELECT 1'),
	"1\n1", "and then the spill finishes, having spilled");
$spilled_vacuum->quit;
$spilling_writer->query_safe('COMMIT');
$spilling_writer->quit;
toasted('unlocked_block');
$spilling->safe_psql(
	'postgres', q{
INSERT INTO unlocked_block VALUES (3, 'short');
SELECT doltgres_test_vacuum('unlocked_block', 'auto');
});
is( $spilling->safe_psql('postgres', sprintf($spilled_entry, 'locked_block')),
	$spilling->safe_psql(
		'postgres', sprintf($spilled_entry, 'unlocked_block')),
	"VACUUM and the spill that waited on its range leave the entry an unspilled VACUUM leaves"
);
$spilling->stop;


# A 1MB standby's replay of the row that leaves the fresh page holds its jump
# after it read the page's entry, the budget filled, so the jump's refused
# prepare relieves by spilling the replayed writer's free space takes of the
# page; the jump reads the entry the spill left, and the page's entry is its
# primary's.
my $PAGE = (8 << 32) | (1024 - 64);
my $jumping = PostgreSQL::Test::Cluster->new('jumping');
$jumping->init(allows_streaming => 1);
$jumping->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off\ncheckpoint_timeout = 1h"
);
$jumping->start;
$jumping->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test');
$jumping->backup('jumping_backup');
my $jumping_standby = PostgreSQL::Test::Cluster->new('jumping_standby');
$jumping_standby->init_from_backup($jumping, 'jumping_backup',
	has_streaming => 1);
$jumping_standby->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h"
);
$jumping_standby->start;
$jumping->safe_psql('postgres', 'CREATE TABLE jumped (k int4)');
$jumping->wait_for_catchup($jumping_standby);
$jumping_standby->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-insert-jump-seen', 'wait')");
$jumping->safe_psql('postgres',
	'INSERT INTO jumped SELECT g FROM generate_series(1, 300) g');
my $jumped =
  $jumping->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$jumping_standby->wait_for_event('startup', 'doltgres-insert-jump-seen');
my $full = $jumping_standby->safe_psql('postgres',
	'SELECT doltgres_test_shared(0, 0, 0)');
$jumping_standby->safe_psql('postgres',
	"SELECT doltgres_test_delta(20, $full, $PAGE)");
$jumping_standby->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-insert-jump-seen'); SELECT injection_points_wakeup('doltgres-insert-jump-seen')"
);
ok( $jumping_standby->poll_query_until(
		'postgres',
		"SELECT pg_last_wal_replay_lsn() >= '$jumped'::pg_lsn OR (SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'startup' AND wait_event = 'versioned delta budget')"
	),
	"the standby's startup goes past its refused jump, or waits at its full budget with its relief spent"
);
$jumping_standby->safe_psql('postgres',
	"SELECT doltgres_test_delta(21, $full, 0)");
$jumping->wait_for_catchup($jumping_standby);
my $jumped_entries =
  q{SELECT string_agg(b || ':' || doltgres_test_free_space('jumped', b), ' ' ORDER BY b) FROM generate_series(0, (pg_relation_size('jumped') / 8192 - 1)::int4) b};
is( $jumping_standby->safe_psql('postgres', $jumped_entries),
	$jumping->safe_psql('postgres', $jumped_entries),
	"a 1MB standby whose replayed leave's read a spill at its refused prepare moved holds its primary's free space entries"
);
$jumping_standby->stop;
$jumping->stop;

done_testing();
