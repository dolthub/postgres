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

# The preload entry's allocation failure (design item 17's cases), through the
# developer setting debug_fail_preload_allocation: when the postmaster's
# preload entry fails to allocate, the postmaster fails to start with Postgres'
# out of memory FATAL, as its own startup errors do, rather than aborting; in an
# EXEC_BACKEND build each child's preload entry runs again, and a failure there
# ends that child the same way. Windows builds are EXEC_BACKEND, so they cover
# the child form natively; it is untested there until the Windows machines are
# ready.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('preload');
$node->init;
$node->start;
is( $node->safe_psql(
		'postgres',
		"SELECT category, vartype, context, enumvals, boot_val, setting FROM pg_settings WHERE name = 'debug_fail_preload_allocation'"
	),
	'Developer Options|enum|postmaster|{off,postmaster,child}|off|off',
	'the setting is a developer enum, off by default');
my $exec_backend =
  $node->safe_psql('postgres', 'SHOW debug_exec_backend') eq 'on';
$node->stop;

# The postmaster's preload entry fails to allocate.
$node->append_conf('postgresql.conf',
	'debug_fail_preload_allocation = postmaster');
my $offset = -s $node->logfile;
ok(!$node->start(fail_ok => 1),
	'the postmaster fails to start when its preload entry fails to allocate');
my $log = slurp_file($node->logfile, $offset);
like(
	$log,
	qr/FATAL:  out of memory\n.*DETAIL:  Failed on request of size \d+\./,
	'it reports Postgres\' out of memory FATAL');
unlike($log, qr/PANIC|terminated by signal|core dumped/,
	'it exits as a startup error, without aborting');

SKIP:
{
	skip 'the child form needs an EXEC_BACKEND build', 2 unless $exec_backend;

	# Each child's preload entry fails to allocate, the startup process's first.
	$node->adjust_conf('postgresql.conf', 'debug_fail_preload_allocation',
		'child');
	$offset = -s $node->logfile;
	ok(!$node->start(fail_ok => 1),
		'the server fails to start when a child\'s preload entry fails to allocate'
	);
	$log = slurp_file($node->logfile, $offset);
	like(
		$log,
		qr/FATAL:  out of memory\n.*DETAIL:  Failed on request of size \d+\..*exited with exit code 1/s,
		'the child exits with Postgres\' out of memory FATAL, without aborting'
	);
}

done_testing();
