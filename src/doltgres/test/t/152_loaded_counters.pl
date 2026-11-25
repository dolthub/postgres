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

# A start's load of a storage's runs moves its tuple id counter once its
# spilled files are linked, before its other runs push, so a block whose free
# space entry a spill holds keeps it (bugs row 349). Before, the counter's
# move took the block's entry before the spill was linked and wrote its
# removal over it, and a standby loading a base backup's runs folded the
# entry away where its primary kept it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 100000;
my $settings =
  "delta_memory = 4MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off";

# A table of narrow rows loaded past a 4 MB delta, whose first block's free
# space versions a spill holds, keeps that block's entry across a crash.
my $node = PostgreSQL::Test::Cluster->new('crashed');
$node->init;
$node->append_conf('postgresql.conf', $settings);
$node->start;
$node->safe_psql(
	'postgres', qq{
CREATE EXTENSION doltgres_test;
CREATE TABLE spilled (v text);
INSERT INTO spilled SELECT 'r' || i FROM generate_series(1, $rows) i;
});
my $block = q{SELECT doltgres_test_free_space('spilled', 0)};
my $before = $node->safe_psql('postgres', $block);
isnt($before, 'none', 'the first block of a table loaded past the delta has a free space entry');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $block),
	$before, 'a crash keeps the free space entry of a block whose versions a spill holds');
$node->stop;

# A standby made from a base backup taken after such a load replays its
# primary's fold of the table, which a later load's pressure starts, to the
# primary's free space tree.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"delta_memory = 4MB\nfold_threshold = 1GB\nfold_timeout = 3600s");
$primary->start;
$primary->safe_psql(
	'postgres', qq{
CREATE EXTENSION doltgres_test;
CREATE TABLE loaded (v text);
INSERT INTO loaded SELECT 'r' || i FROM generate_series(1, $rows) i;
CREATE TABLE pressing (v text);
});
$primary->backup('loaded');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'loaded', has_streaming => 1);
$standby->append_conf('postgresql.conf', "delta_memory = 4MB");
$standby->start;
$primary->safe_psql('postgres',
	"INSERT INTO pressing SELECT 'p' || i FROM generate_series(1, $rows) i");
$primary->poll_query_until('postgres',
	q{SELECT split_part(doltgres_test_base_sizes('loaded'), ' ', 2) <> '0'})
  or die 'the primary never folded the loaded table';
my $caught = eval { $primary->wait_for_replay_catchup($standby); 1 };
ok( $caught && !$standby->log_contains(qr/does not match its primary's/),
	"a standby loaded from a base backup replays its primary's fold of a table a spill held");
$standby->stop('immediate', fail_ok => 1);
$primary->stop;

# entries returns a table's free space entries of block 0 and of a block.
sub entries
{
	my ($cluster, $table, $block) = @_;
	return $cluster->safe_psql('postgres',
		"SELECT doltgres_test_free_space('$table', 0) || '|' || doltgres_test_free_space('$table', $block)"
	);
}

# A crash after a fold of such a table past its checkpoint's redo point makes
# its state at its first spilled file with an empty counter, which its free
# space run's move then sets to the run's cut: the table's next row takes the
# cut's tuple id, and block 0's and the counter block's entries are the live
# ones.
my $folded = PostgreSQL::Test::Cluster->new('folded');
$folded->init;
$folded->append_conf('postgresql.conf', $settings);
$folded->start;
$folded->safe_psql(
	'postgres', qq{
CREATE EXTENSION doltgres_test;
CREATE TABLE folded (v text);
INSERT INTO folded SELECT 'r' || i FROM generate_series(1, $rows) i;
CHECKPOINT;
SELECT doltgres_test_fold('folded');
});
my $cut = (split / /, $folded->safe_psql('postgres', q{SELECT doltgres_test_base_sizes('folded')}))[3];
my ($cut_block) = $cut =~ /^\((\d+),/;
my $live = entries($folded, 'folded', $cut_block);
$folded->stop('immediate');
$folded->start;
is(entries($folded, 'folded', $cut_block),
	$live, "a crash past a fold keeps block 0's and the counter block's free space entries");
is($folded->safe_psql('postgres', q{INSERT INTO folded VALUES ('n') RETURNING ctid}),
	$cut, "a crash past a fold starts the table's next row at its free space run's cut");
$folded->stop;

# A standby restarted after replaying such a fold past its restartpoint's
# redo point loads its runs to its primary's free space entries.
my $source = PostgreSQL::Test::Cluster->new('source');
$source->init(allows_streaming => 1);
$source->append_conf('postgresql.conf', $settings);
$source->start;
$source->safe_psql(
	'postgres', qq{
CREATE EXTENSION doltgres_test;
CREATE TABLE replayed (v text);
INSERT INTO replayed SELECT 'r' || i FROM generate_series(1, $rows) i;
});
$source->backup('replayed');
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->init_from_backup($source, 'replayed', has_streaming => 1);
$replica->append_conf('postgresql.conf', "delta_memory = 4MB");
$replica->start;
$source->safe_psql('postgres', 'CHECKPOINT');
$source->wait_for_replay_catchup($replica);
$replica->safe_psql('postgres', 'CHECKPOINT');
$source->safe_psql('postgres', q{SELECT doltgres_test_fold('replayed')});
$source->wait_for_replay_catchup($replica);
my $replayed = (split / /, $source->safe_psql('postgres', q{SELECT doltgres_test_base_sizes('replayed')}))[3];
my ($replayed_block) = $replayed =~ /^\((\d+),/;
$replica->stop('immediate');
$replica->start;
$source->wait_for_replay_catchup($replica);
is(entries($replica, 'replayed', $replayed_block),
	entries($source, 'replayed', $replayed_block),
	"a standby restarted past a replayed fold loads its primary's free space entries");
$replica->stop;
$source->stop;

done_testing();
