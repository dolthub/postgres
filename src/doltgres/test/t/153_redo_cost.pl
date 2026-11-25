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

# A crash start's redo of keyed, indexed tables' row changes takes at most a
# few times the updates that wrote them, while two hundred more indexed tables
# keep their states in the pool, since each replayed record finds its
# storage's state through the states' pool's index, not a sweep of the pool's
# slots (bugs row 405).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

my $node = PostgreSQL::Test::Cluster->new('redo_cost');
$node->init;
$node->append_conf('postgresql.conf',
	"max_connections = 100\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h\nmax_wal_size = 4GB\nautovacuum = off"
);
$node->start;
$node->safe_psql(
	'postgres',
	join(
		' ',
		map {
			"CREATE TABLE loaded_$_ (k int4 PRIMARY KEY, v text); CREATE INDEX ON loaded_$_ (v);"
		} 1 .. 10));
$node->safe_psql('postgres',
	"INSERT INTO loaded_$_ SELECT g, md5(g::text) FROM generate_series(1, 2000) g"
) for 1 .. 10;
$node->safe_psql(
	'postgres',
	join(
		' ',
		map {
			"CREATE TABLE other_$_ (k int4, v text); CREATE INDEX ON other_$_ (k); CREATE INDEX ON other_$_ (v); INSERT INTO other_$_ VALUES (1, 'v');"
		} 1 .. 200));
$node->safe_psql('postgres', 'CHECKPOINT');
my $started = time();
$node->safe_psql('postgres', "UPDATE loaded_$_ SET v = v || 'u'") for 1 .. 10;
my $load = time() - $started;
$node->stop('immediate');
$node->start;
my ($redo) =
  slurp_file($node->logfile) =~ /redo done at .* elapsed: ([0-9.]+) s/;
cmp_ok(
	$redo, '<=',
	3 * $load + 1,
	sprintf(
		"a crash start's redo of 20,000 updated rows of indexed tables takes at most three times their update (redo %.2f s, update %.2f s)",
		$redo, $load));
is( $node->safe_psql(
		'postgres',
		"SELECT count(*), count(*) FILTER (WHERE v LIKE '%u') FROM ("
		  . join(' UNION ALL ', map { "SELECT v FROM loaded_$_" } 1 .. 10)
		  . ') updated'),
	'20000|20000',
	"and every row redo replayed reads back updated");
$node->stop;

done_testing();
