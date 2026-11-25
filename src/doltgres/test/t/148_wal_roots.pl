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

# A collection keeps what the WAL past the redo point names, which recovery
# after a crash reads (bugs row 406): an index built after a checkpoint, whose
# base a fold then replaces, keeps its build's pack through a full collection,
# so the start after a crash replays the build's record and then the fold's.
# Under wal_level minimal the build logs no Chunk record and its pack is the
# only copy of its base, which a collection once retired, so the start stopped
# at the build's record; a streaming primary logs the build's nodes; and a
# standby that replayed both records and collects keeps them past its crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# superseded builds an index after a checkpoint, inserts a row, and folds the
# index, so a fold's base replaces the build's past the redo point.
sub superseded
{
	my ($node) = @_;
	$node->safe_psql('postgres', 'CREATE INDEX built_k ON built (k)');
	$node->safe_psql('postgres', 'INSERT INTO built VALUES (1, 10)');
	$node->safe_psql('postgres', "SELECT doltgres_test_index_fold('built_k')");
}

# restarted crashes a node and starts it, answering whether it started and
# then whether the index reads the row.
sub restarted
{
	my ($node, $form) = @_;
	$node->stop('immediate');
	my $started = $node->start(fail_ok => 1);
	ok($started, "$form: the start after a crash replays the build and the fold past the redo point (row 406)");
	return unless $started;
	is( $node->safe_psql('postgres',
			'SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM built WHERE k = 10'),
		'1',
		"$form: the index reads the row after the start (row 406)");
}

my $setup =
  'CREATE EXTENSION doltgres_test; CREATE TABLE built (id int PRIMARY KEY, k int); CHECKPOINT';
my $settings = "fold_threshold = 1GB\nfold_timeout = 3600s";

my $minimal = PostgreSQL::Test::Cluster->new('minimal');
$minimal->init;
$minimal->append_conf('postgresql.conf', $settings);
$minimal->start;
$minimal->safe_psql('postgres', $setup);
superseded($minimal);
like($minimal->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	qr/^\d+ \d+ \d+$/, 'minimal: a full collection runs past the redo point');
restarted($minimal, 'minimal');
$minimal->stop;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $settings);
$primary->start;
$primary->safe_psql('postgres', $setup);
$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', $settings);
$standby->start;
superseded($primary);
$primary->wait_for_catchup($standby);
like($primary->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	qr/^\d+ \d+ \d+$/, 'streaming: a full collection runs past the redo point');
like($standby->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	qr/^\d+ \d+ \d+$/, "standby: a full collection runs past the standby's restartpoint");
restarted($standby, 'standby');
restarted($primary, 'streaming');
$standby->stop;
$primary->stop;

done_testing();
