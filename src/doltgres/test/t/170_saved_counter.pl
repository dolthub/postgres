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

# A primary whose fold a held snapshot keeps below a committed batch, after a
# checkpoint's save, and its standby folding the same point keep the same base:
# its sizes, fresh end, and tuple id counter (bugs row 483). Before, the save
# wrote the live counter, which counts the batch's tuple ids, into the
# primary's newest base, and the standby's base kept its own.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $settings = "fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off";
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $settings);
$primary->start;
$primary->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE TABLE t (id int4, v text)');
$primary->backup('saved');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'saved', has_streaming => 1);
$standby->append_conf('postgresql.conf', $settings);
$standby->start;

$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat('x', 30) FROM generate_series(1, 2000) g");
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
my $held = $primary->background_psql('postgres');
$held->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t');
$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat('y', 30) FROM generate_series(2001, 5000) g");
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->safe_psql('postgres', 'SELECT txid_current()');
$primary->wait_for_replay_catchup($standby);
my $sizes = q{SELECT doltgres_test_base_sizes('t')};
is( $standby->safe_psql(
		'postgres', "$sizes FROM (SELECT doltgres_test_fold('t')) f"),
	$primary->safe_psql('postgres', $sizes),
	"a standby folding the point its primary's held fold stopped at keeps the primary's base, counter included"
);
$held->query_safe('COMMIT');
$held->quit;

# After a crash, the primary's base is still its standby's, and the counter
# its checkpoint cut keeps every tuple id a row holds from a new insert.
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $sizes),
	$standby->safe_psql('postgres', $sizes),
	"after a crash the primary's base is still its standby's");
$primary->safe_psql('postgres', "INSERT INTO t VALUES (5001, 'after')");
is( $primary->safe_psql(
		'postgres', 'SELECT count(*) - count(DISTINCT ctid) FROM t'),
	'0',
	'an insert after the crash takes a tuple id no row holds');

done_testing();
