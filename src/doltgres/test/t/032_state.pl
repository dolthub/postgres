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

# The storage states' pool (design item 6's count) in the server's shared
# memory, through the test module's doltgres_test_states and
# doltgres_test_relation_locks: under non-default max_prepared_transactions
# and max_connections, and max_locks_per_transaction on each side of a power
# of two, one backend holds a state for each relation lock Postgres can grant
# at once, as that backend's own lock globals count them after its init, and
# one state more is the pool's error. The fork sizes the pool before Postgres
# computes the fast-path groups in a child, so the count it copies is checked
# against Postgres' own.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('state');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
max_prepared_transactions = 3
max_connections = 5
autovacuum_worker_slots = 3
max_worker_processes = 2
});

my $repository = 900000;
foreach my $locks (100, 300)
{
	$node->append_conf('postgresql.conf',
		"max_locks_per_transaction = $locks");
	$node->start;
	$node->safe_psql('postgres',
		'CREATE EXTENSION IF NOT EXISTS doltgres_test; CALL doltgres_test_twins()');
	my $count =
	  $node->safe_psql('postgres', 'SELECT doltgres_test_relation_locks()');

	$repository++;
	is( $node->safe_psql(
			'postgres', "SELECT doltgres_test_states($repository, $count)"),
		$count,
		"the storage states' pool holds a state for each relation lock a backend's own globals count"
	);

	$repository++;
	my ($ret, $stdout, $stderr) = $node->psql('postgres',
		"SELECT doltgres_test_states($repository, $count + 1)");
	like(
		$stderr,
		qr/every storage state record is in use/,
		"one state more than the relation locks is the pool's error");
	$node->stop;
}

done_testing();
