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

# A block's free whose prepare takes more than one group (bugs row 384): with
# the delta budget held by test pages, a VACUUM of a block of folded deletes
# of rows with out-of-line values gives its parts back group after group,
# each group's part in a record of its own. A later group of value
# take-downs once named its takes against the first group's and stopped the
# server; a later group of dead tuple ids' markers, and a later group after a
# refused prepare's spill moved what the block's read saw, once had the block
# tried again from its first part, giving parts back twice. Each free names
# each tuple id once and leaves the free space entry a VACUUM on a free
# budget leaves, and the standby that replays each group's record reads what
# its primary does.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 40;
my $BLOCK = 64 * 1024;
my $BUDGET = 1024 * 1024;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off\ncheckpoint_timeout = 1h"
);
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION pg_walinspect; CREATE EXTENSION doltgres_test');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'delta_memory = 64MB');
$standby->start;

# deleted makes a table of rows with out-of-line values, with an index when
# asked, deletes every other row, and folds the deletes.
sub deleted
{
	my ($table, $indexed) = @_;
	$primary->safe_psql(
		'postgres', qq{
CREATE TABLE $table (k int4, v text);
ALTER TABLE $table ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO $table SELECT g, repeat(md5(g::text), 100) FROM generate_series(1, $rows) g;
});
	$primary->safe_psql('postgres', "CREATE INDEX ${table}_k ON $table (k)")
	  if $indexed;
	$primary->safe_psql('postgres', "DELETE FROM $table WHERE k % 2 = 0");
	$primary->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
}

# The test record whose chain of test pages holds the delta budget.
my $holder = $primary->safe_psql('postgres', 'SELECT doltgres_test_shared(0, 0, 0)');

# linked links a chain of test pages under the holder, the count given of
# 4 KiB, and returns how many the delta gave.
sub linked
{
	my ($pages) = @_;
	return 0 if $pages == 0;
	return $primary->safe_psql('postgres',
		"SELECT doltgres_test_delta(29, $holder, $pages)");
}

# used returns the delta's bytes in use.
sub used
{
	return $primary->safe_psql('postgres', 'SELECT doltgres_test_delta(0, 0, 0)');
}

# held links the fewest test pages that leave no more than the bytes given
# free in the delta, found by trying chains it gives back, and returns how
# many it linked.
sub held
{
	my ($free) = @_;
	my ($low, $high) = (0, $BUDGET / 4096);
	while ($low < $high)
	{
		my $pages = int(($low + $high) / 2);
		my $gave = linked($pages);
		my $reached = used() >= $BUDGET - $free;
		$primary->safe_psql('postgres',
			"SELECT doltgres_test_delta(21, $holder, 0)")
		  if $gave > 0;
		if ($reached)
		{
			$high = $pages;
		}
		else
		{
			$low = $pages + 1;
		}
	}
	return linked($low);
}

# vacuumed runs a VACUUM of a table with the delta held to the free bytes
# given, gives the holding pages back once the VACUUM finishes or waits at
# the budget, and returns its frees' records' count, tuple ids, and dead
# tuple ids.
sub vacuumed
{
	my ($table, $cleanup, $free) = @_;
	my $linked = held($free);
	my $start =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	my $vacuum = $primary->background_psql('postgres');
	my $pid = $vacuum->query_safe('SELECT pg_backend_pid()');
	$vacuum->query_until(qr/vacuuming/,
		"\\echo vacuuming\nSELECT doltgres_test_vacuum('$table', '$cleanup');\n"
	);
	$primary->poll_query_until(
		'postgres', qq{
SELECT state = 'idle' AND query LIKE '%doltgres_test_vacuum%' OR wait_event = 'versioned delta budget'
FROM pg_stat_activity WHERE pid = $pid});
	$primary->safe_psql('postgres', "SELECT doltgres_test_delta(21, $holder, 0)")
	  if $linked > 0;
	$vacuum->query_safe('SELECT 1');
	$vacuum->quit;
	$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
	return '0||'
	  if $primary->safe_psql('postgres',
		"SELECT pg_current_wal_flush_lsn() <= '$start'") eq 't';
	return $primary->safe_psql(
		'postgres', qq{
SELECT count(*) || '|' || sum(m[1]::int8) || '|' || sum(m[2]::int8)
FROM pg_get_wal_records_info('$start', pg_current_wal_flush_lsn()),
	regexp_match(description, '; ([0-9]+) tids; ([0-9]+) dead') AS m
WHERE record_type = 'TidsFreed'});
}

my $entry = q{SELECT doltgres_test_free_space('%s', 0)};
my $read = q{SELECT count(*), sum(length(v)) FROM %s};

# Bugs row 384's cases. With the whole budget held, a later group's prepare
# is refused and spills the table's versions, which moves what the block's
# read saw before the group after it: the free goes on from that group after
# reading again, so it gives the freed runs back once, and inserts after it,
# made last, get distinct tuple ids. The table is the only one whose versions
# the spill can take, so its twin is made after it.
deleted('spilled_split');
my ($count, $tids, $dead) = split /\|/, vacuumed('spilled_split', 'auto', 0);
deleted('spilled_whole');
vacuumed('spilled_whole', 'auto', $BUDGET);
is($tids, $rows / 2,
	'a free across a spill gives back each tuple id once (row 384)');
is( $primary->safe_psql('postgres', sprintf($entry, 'spilled_split')),
	$primary->safe_psql('postgres', sprintf($entry, 'spilled_whole')),
	'a free across a spill leaves the entry a free budget leaves (row 384)');
is( $primary->safe_psql('postgres', sprintf($read, 'spilled_split')),
	($rows / 2) . '|' . ($rows / 2 * 3200),
	'the rows a free across a spill left read back (row 384)');

# With one block of the budget free, each group takes one part: the freed
# runs' take, then each value's take-down, each later group's record holding
# only take-downs.
deleted($_) for qw(freed_whole freed_split);
vacuumed('freed_whole', 'auto', $BUDGET);
($count, $tids, $dead) = split /\|/, vacuumed('freed_split', 'auto', $BLOCK);
cmp_ok($count, '>', 2, 'the free takes more than two groups');
is($tids, $rows / 2, 'the free gives back each tuple id once (row 384)');
is( $primary->safe_psql('postgres', sprintf($entry, 'freed_split')),
	$primary->safe_psql('postgres', sprintf($entry, 'freed_whole')),
	'a free of later groups of take-downs leaves the entry a free budget leaves (row 384)'
);
is( $primary->safe_psql('postgres', sprintf($read, 'freed_split')),
	($rows / 2) . '|' . ($rows / 2 * 3200),
	'the rows the free left read back (row 384)');

# A dead block whose markers span groups: a VACUUM that leaves the index
# makes each dead tuple id's marker, a later group's after the first group's
# took the block's entry, and a VACUUM that cleans the index frees them.
deleted($_, 1) for qw(dead_whole dead_split);
vacuumed('dead_whole', 'off', $BUDGET);
vacuumed('dead_whole', 'on', $BUDGET);
($count, $tids, $dead) = split /\|/, vacuumed('dead_split', 'off', $BLOCK);
cmp_ok($count, '>', 1, "the dead block's markers span groups");
is($dead, $rows / 2, 'the markers name each dead tuple id once (row 384)');
vacuumed('dead_split', 'on', $BUDGET);
is( $primary->safe_psql('postgres', sprintf($entry, 'dead_split')),
	$primary->safe_psql('postgres', sprintf($entry, 'dead_whole')),
	'markers spanning groups leave the entry a free budget leaves (row 384)');

# The standby, whose larger budget takes each record's redo in one group,
# replays each group's record, its entries and rows its primary's.
$primary->wait_for_catchup($standby);
for my $table (qw(freed_split dead_split spilled_split))
{
	is( $standby->safe_psql('postgres', sprintf($entry, $table)),
		$primary->safe_psql('postgres', sprintf($entry, $table)),
		"the standby's entry of $table is its primary's (row 384)");
	is( $standby->safe_psql('postgres', sprintf($read, $table)),
		$primary->safe_psql('postgres', sprintf($read, $table)),
		"the standby reads ${table}'s rows (row 384)");
}

$primary->safe_psql('postgres',
	"INSERT INTO spilled_split SELECT g, 'x' FROM generate_series(1, $rows) g"
);
is( $primary->safe_psql(
		'postgres', 'SELECT count(DISTINCT ctid) = count(*) FROM spilled_split'),
	't',
	'inserts after a free across a spill get distinct tuple ids (row 384)');

$standby->stop;
$primary->stop;
done_testing();
