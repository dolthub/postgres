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

# A save between a checkpoint and a crash (bugs row 395): rows inserted onto a
# fresh page, a save, and more rows, on that page or past it, then a crash.
# The restart's redo replays the rows from the checkpoint's redo point with a
# tuple id counter no record past it set, so the counter moves past every
# row, and the checkpoint after it cuts its free space run past them; the
# next crash's start loads that checkpoint's runs, every row reads back, and
# rows inserted after get tuple ids no row holds. Before, redo started from
# the save's counter and refused the takes at and past it on its page: the
# checkpoint's cut fell below rows its runs held, so the start refused the
# database, or, with the rows past the page, an insert after found the
# page's entry corrupt. Each case has a database of its own, since a start
# refuses a database for the server's life.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('saved');
$node->init;
$node->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off\ncheckpoint_timeout = 1h"
);
$node->start;

# sql runs a statement in a database and returns its output, or its error.
sub sql
{
	my ($database, $statement) = @_;
	my ($code, $out, $err) = $node->psql($database, $statement);
	return $code == 0 ? $out : $err;
}

# crashed stops the server at once and starts it again.
sub crashed
{
	$node->stop('immediate');
	$node->start;
}

# saved makes a database whose table's base holds block 0's freed offsets, so
# the replayed inserts that reuse them take from an entry and the storage's
# checkpoint writes a free space run; inserts rows onto a fresh page; saves;
# and inserts the rows given after the save.
sub saved
{
	my ($database, $after) = @_;
	$node->safe_psql('postgres', "CREATE DATABASE $database");
	$node->safe_psql($database, 'CREATE EXTENSION doltgres_test');
	$node->safe_psql($database, 'CHECKPOINT');
	$node->safe_psql($database, 'CREATE TABLE t (k int4, v text)');
	$node->safe_psql($database,
		"INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 40) g"
	);
	$node->safe_psql($database, "SELECT doltgres_test_fold('t')");
	$node->safe_psql($database, 'DELETE FROM t WHERE k <= 5');
	$node->safe_psql($database, "SELECT doltgres_test_fold('t')");
	$node->safe_psql($database, "SELECT doltgres_test_vacuum('t', 'auto')");
	$node->safe_psql($database, "SELECT doltgres_test_fold('t')");
	$node->safe_psql($database,
		"INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(41, 80) g"
	);
	$node->safe_psql($database, 'SELECT doltgres_test_save()');
	$node->safe_psql($database,
		"INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(81, 80 + $after) g"
	);
}

my $read = 'SELECT count(*), sum(k) FROM t';

# saved_read checks a database after a crash, a CHECKPOINT, and a second
# crash: its rows read back, and rows inserted after get distinct tuple ids.
sub saved_read
{
	my ($database, $what) = @_;
	my $rows = $node->safe_psql($database, $read);
	crashed();
	is(sql($database, $read), $rows,
		"$what: the rows read back after a crash (row 395)");
	sql($database, 'CHECKPOINT');
	crashed();
	is(sql($database, $read), $rows,
		"$what: the checkpoint's runs load after a second crash, every row read back (row 395)"
	);
	sql($database,
		"INSERT INTO t SELECT g, repeat('y', 200) FROM generate_series(201, 240) g"
	);
	is(sql($database, 'SELECT count(DISTINCT ctid) = count(*) FROM t'),
		't', "$what: rows inserted after get tuple ids no row holds (row 395)");
}

# Bugs row 395's cases: the rows after the save stay on the save's page, and
# they go on past it to the next page.
saved('on_page', 5);
is(sql('on_page', 'SELECT max(ctid) < \'(3,1)\' FROM t'),
	't', 'the rows after the save stay on its page');
saved_read('on_page', 'rows on the save\'s page');
saved('past_page', 40);
is(sql('past_page', 'SELECT max(ctid) >= \'(3,1)\' FROM t'),
	't', 'the rows after the save go on past its page');
saved_read('past_page', 'rows past the save\'s page');

# A checkpoint's runs hold rows past the base's fresh end whose records come
# before its redo point, so redo never replays them: the start's counter
# moves on to the runs' cut as they load, and inserts after it take tuple ids
# past them.
$node->safe_psql('postgres', 'CREATE DATABASE loaded');
$node->safe_psql('loaded', 'CREATE EXTENSION doltgres_test');
$node->safe_psql('loaded', 'CREATE TABLE t (k int4, v text)');
$node->safe_psql('loaded',
	"INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 40) g");
$node->safe_psql('loaded', "SELECT doltgres_test_fold('t')");
$node->safe_psql('loaded',
	"INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(41, 80) g"
);
$node->safe_psql('loaded', 'CHECKPOINT');
crashed();
sql('loaded',
	"INSERT INTO t SELECT g, repeat('y', 200) FROM generate_series(81, 120) g"
);
is( sql('loaded', 'SELECT count(*), count(DISTINCT ctid), sum(k) FROM t'),
	'120|120|7260',
	"rows inserted after a start that loaded a checkpoint's runs get tuple ids past the runs' rows (row 395)"
);

$node->stop;
done_testing();
