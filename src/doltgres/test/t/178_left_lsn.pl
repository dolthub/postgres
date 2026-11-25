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

# A standby's replay of a fresh page's leave gives the page's free space
# version its record's end, as the primary's leave does, so the page's entry
# read as of a point before the leave is the entry before it on both
# servers, before and after the standby restarts and replays past it (bugs
# row 456). Before, the replayed version carried LSN 0, which every point
# reads.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $settings = "fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off";
my $primary = PostgreSQL::Test::Cluster->new('left');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $settings);
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE EXTENSION pg_walinspect; CREATE TABLE t (id int4, v text)'
);
$primary->backup('left_backup');
my $standby = PostgreSQL::Test::Cluster->new('left_standby');
$standby->init_from_backup($primary, 'left_backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', $settings);
$standby->start;

my $before = $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat('v', 200) FROM generate_series(1, 60) g");
my ($start, $end) = split /\|/, $primary->safe_psql(
	'postgres', qq{
SELECT start_lsn, end_lsn FROM pg_get_wal_records_info('$before', pg_current_wal_flush_lsn())
  WHERE resource_manager = 'Versioned' AND record_type = 'PageLeft' ORDER BY start_lsn LIMIT 1});
ok(defined $end, 'the inserts leave the first fresh page');
$primary->wait_for_replay_catchup($standby);

my $at_start = "SELECT doltgres_test_free_space('t', 0, '$start')";
my $at_end = "SELECT doltgres_test_free_space('t', 0, '$end')";
my $primary_start = $primary->safe_psql('postgres', $at_start);
my $primary_end = $primary->safe_psql('postgres', $at_end);
isnt($primary_start, $primary_end,
	"the primary's leave changes the page's entry at its record's end");
for my $when ('streaming', 'restarted')
{
	is($standby->safe_psql('postgres', $at_start),
		$primary_start,
		"$when: the standby reads the page's entry before the leave as its primary does");
	is($standby->safe_psql('postgres', $at_end),
		$primary_end,
		"$when: the standby reads the page's entry after the leave as its primary does");
	$standby->restart;
}

done_testing();
