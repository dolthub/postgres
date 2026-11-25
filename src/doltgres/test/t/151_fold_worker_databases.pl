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

# A fold worker connected to a database ends for CREATE DATABASE from it and
# for DROP DATABASE of it, as stock ends an autovacuum worker, where it made
# them refuse the database as being accessed by other users (bugs row 420).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('working');
$node->init;
$node->append_conf('postgresql.conf', "fold_threshold = 16kB\nfold_timeout = 3600s");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE DATABASE working');
$node->safe_psql('working', 'CREATE EXTENSION doltgres_test; CREATE TABLE w (k int4, v text)');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-relation', 'wait')");

# held writes past the fold threshold in database working and waits until a
# fold worker is held there in its fold.
sub held
{
	$node->safe_psql('working',
		"INSERT INTO w SELECT n, repeat('h', 200) FROM generate_series(1, 200) n");
	$node->wait_for_event('fold worker', 'doltgres-fold-relation');
}

held();
my ($ret, $out, $err) =
  $node->psql('postgres', 'CREATE DATABASE working_copy TEMPLATE working');
is($ret . $err, '0',
	'CREATE DATABASE from a database a fold worker is connected to ends the worker');

# The ended worker's fold left nothing half-installed: the rows read back
# whole, and a later fold of the table succeeds.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-relation')");
is($node->safe_psql('working', 'SELECT count(*), sum(length(v)) FROM w'),
	'200|40000', "the ended worker's table reads back whole");
$node->safe_psql('working', "SELECT doltgres_test_fold('w')");
is($node->safe_psql('working', 'SELECT count(*), sum(length(v)) FROM w'),
	'200|40000', "a fold of the ended worker's table succeeds and keeps its rows");
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-relation', 'wait')");
held();
($ret, $out, $err) = $node->psql('postgres', 'DROP DATABASE working');
is($ret . $err, '0',
	'DROP DATABASE of a database a fold worker is connected to ends the worker');

$node->stop;

# A fold worker started on an ask that ends before it connects to its
# database leaves the ask owed, so a worker started after it serves the ask
# and folds the database's storage, where the ended worker's start served the
# ask and nothing folded the storage while neither of the fold's thresholds
# was met (bugs row 421). A checkpoint asks for the folds.
my $asked = PostgreSQL::Test::Cluster->new('asked');
$asked->init;
$asked->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h");
$asked->start;
$asked->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION doltgres_test;
CREATE TABLE asked (k int4, v text);
INSERT INTO asked SELECT n, repeat('a', 100) FROM generate_series(1, 100) n;
SELECT injection_points_attach('doltgres-fold-worker-connect', 'error');
});
my $offset = -s $asked->logfile;
$asked->safe_psql('postgres', 'CHECKPOINT');
$asked->wait_for_log(
	qr/error triggered for injection point doltgres-fold-worker-connect/,
	$offset);
$asked->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-worker-connect')");
ok( $asked->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('asked'), ' ', 2) = '100'"),
	'an ask whose fold worker ended before connecting is served by a later worker'
);
$asked->stop;

# A fold worker connects to a database that takes no new connections, as
# stock's autovacuum worker does, so a table a session connected before
# ALLOW_CONNECTIONS false writes past fold_threshold is folded, where the
# worker's connect was refused and the table stayed unfolded (bugs row 422).
my $closed = PostgreSQL::Test::Cluster->new('closed');
$closed->init;
$closed->append_conf('postgresql.conf',
	"fold_threshold = 16kB\nfold_timeout = 3600s");
$closed->start;
$closed->safe_psql('postgres', 'CREATE DATABASE closed');
$closed->safe_psql('closed',
	'CREATE EXTENSION doltgres_test; CREATE TABLE c (k int4, v text)');
my $session = $closed->background_psql('closed');
$session->set_query_timer_restart;
$closed->safe_psql('postgres', 'ALTER DATABASE closed ALLOW_CONNECTIONS false');
$session->query_safe(
	"INSERT INTO c SELECT n, repeat('c', 200) FROM generate_series(1, 200) n");
my $folded = '';
for (1 .. $PostgreSQL::Test::Utils::timeout_default)
{
	$folded = $session->query_safe(
		"SELECT split_part(doltgres_test_base_sizes('c'), ' ', 2)");
	last if $folded eq '200';
	sleep 1;
}
$session->quit;
is($folded, '200',
	'a fold worker folds a table of a database that takes no new connections');
$closed->stop;

# The fold launcher starts no fold worker for an invalid database, one a
# DROP DATABASE left partly dropped, as stock's autovacuum skips it, where it
# started one at each check that could not connect (bugs row 423). The
# invalid database's table, written by a session connected before the mark,
# is due at a check the launcher finishes before another database's table is
# written; with one fold worker, that table is folded only once no worker for
# the invalid database runs, so one started for it has logged its refused
# connection by then.
my $dropping = PostgreSQL::Test::Cluster->new('dropping');
$dropping->init;
$dropping->append_conf('postgresql.conf',
	"fold_threshold = 16kB\nfold_timeout = 3600s\nfold_workers = 1\nlog_min_messages = notice"
);
$dropping->start;
$dropping->safe_psql('postgres', 'CREATE EXTENSION injection_points');
for my $database ('invalid', 'valid')
{
	$dropping->safe_psql('postgres', "CREATE DATABASE $database");
	$dropping->safe_psql($database,
		'CREATE EXTENSION doltgres_test; CREATE TABLE d (k int4, v text)');
}
my $marked = $dropping->background_psql('invalid');
$dropping->safe_psql('postgres',
	"UPDATE pg_database SET datconnlimit = -2 WHERE datname = 'invalid'");
$marked->query_safe(
	"INSERT INTO d SELECT n, repeat('d', 200) FROM generate_series(1, 200) n");
$dropping->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-launch', 'notice')");
my $checks = -s $dropping->logfile;
for (1 .. 2)
{
	$dropping->wait_for_log(
		qr/notice triggered for injection point doltgres-fold-launch/, $checks);
	$checks = -s $dropping->logfile;
}
$dropping->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-launch')");
$dropping->safe_psql('valid',
	"INSERT INTO d SELECT n, repeat('d', 200) FROM generate_series(1, 200) n");
$dropping->poll_query_until('valid',
	"SELECT split_part(doltgres_test_base_sizes('d'), ' ', 2) = '200'")
  or die 'the valid database was never folded';
$marked->quit;
ok( !$dropping->log_contains(qr/cannot connect to invalid database/),
	'the fold launcher starts no fold worker for an invalid database');
$dropping->stop;
done_testing();
