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

# A checkpoint window whose work fails with the fork's own error (bugs row
# 392): each window that runs a pause inside it, failed there by the pause's
# "-fails" injection point, stops the server with that error, at PANIC with
# its own SQLSTATE and message, as Postgres raises an error inside a critical
# section, its context naming the window; the server comes back, and the rows
# committed before read back. Before, the window's drop raised only "checkpoint
# delay ended before its change was made visible". The windows with no pause
# inside them (a blob's write, an identity change, a rewrite's carry and its
# free space, and a whole build's node records) have no case here, nor has a
# truncate's emptying of a table its transaction made, whose window loads the
# memtables' clearing and unlisting pauses that its retire of the memtables
# doesn't reach.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('failing');
$node->init;
$node->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off\ncheckpoint_timeout = 1h\nrestart_after_crash = on\nlog_error_verbosity = verbose"
);
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION doltgres_test;
CREATE TABLE t (k int4, v text);
INSERT INTO t SELECT g, 'r' || g FROM generate_series(1, 10) g;
CREATE TABLE i (k int4, v text);
CREATE INDEX i_v ON i (v);
INSERT INTO i SELECT g, 'r' || g FROM generate_series(1, 10) g;
CREATE TABLE v (k int4, v text);
ALTER TABLE v ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO v SELECT g, repeat(md5(g::text), 100) FROM generate_series(1, 10) g;
DELETE FROM v WHERE k <= 5;
SELECT doltgres_test_fold('v');
CREATE TABLE carried (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50);
INSERT INTO carried SELECT g, repeat('c', 20) FROM generate_series(1, 30) g;
SELECT doltgres_test_fold('carried');
});

my $read =
  q{SELECT (SELECT count(*) FROM t) || ' ' || (SELECT count(*) FROM i) || ' ' || (SELECT count(*) FROM v)};

# failed attaches a pause's failing point, runs the statement that reaches
# the pause inside its window, and checks that the server stopped with the
# injected error and came back with the rows the read gives.
sub failed
{
	my ($pause, $statement, $rows, $what) = @_;
	my $offset = -s $node->logfile;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$pause-fails', 'notice')");
	$node->psql('postgres', $statement);
	$node->wait_for_log(qr/database system is ready to accept connections/,
		$offset);
	like(
		slurp_file($node->logfile, $offset),
		qr/PANIC:  XX000: injected failure at \Q$pause\E\n[^\n]*CONTEXT:  checkpoint delay ended before its change was made visible/,
		"$what fails in its window, stopping the server with its own error (row 392)"
	);
	is($node->safe_psql('postgres', $read),
		$rows, "$what: the server comes back and its rows read back (row 392)");
}

failed('doltgres-insert-logged', "INSERT INTO t VALUES (11, 'r11')",
	'10 10 5', "an insert");
failed('doltgres-change-logged', 'DELETE FROM t WHERE k = 1',
	'10 10 5', "a delete");
failed('doltgres-entry-logged', "INSERT INTO i VALUES (11, 'r11')",
	'10 10 5', "an index entry");
failed('doltgres-vacuum-logged', "SELECT doltgres_test_vacuum('v', 'auto')",
	'10 10 5', "a VACUUM's free");
$node->safe_psql('postgres', "INSERT INTO t VALUES (11, 'r11')");
failed('doltgres-state-logging', "SELECT doltgres_test_fold('t')",
	'11 10 5', "a fold's install");
# A carry whose count's add fails takes its files back out in the unlist's
# window (t/124's form): three writers' recently dead versions, an older
# snapshot keeping them.
my $holder = $node->background_psql('postgres', on_error_stop => 0);
$holder->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
for my $n (0 .. 2)
{
	$node->safe_psql('postgres',
		"UPDATE carried SET v = v || 'u' WHERE mod(k, 3) = $n");
}
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-carry-counting', 'error')");
failed('doltgres-carry-unlisting', 'VACUUM FULL carried',
	'11 10 5', "a carry's unlist");
eval { $holder->quit };
is($node->safe_psql('postgres', 'SELECT count(*) FROM carried'),
	'30', "the carried table's rows read back (row 392)");

$node->stop;
done_testing();
