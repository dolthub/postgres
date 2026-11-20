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

# A backend's owned records (design item 13) in the server, through the test
# module's doltgres_test_owned: a record one call makes is lent to the next by
# its handle; the resource owner it was made under releases it when its end
# never came, at the transaction's commit with Postgres' leak warning and at
# an ERROR's abort raising nothing; its end forgets it; and a handle that
# names no live record, stale,
# duplicated, or forged, is refused with an internal error. A call that comes
# back into the fork for a record its caller holds is the simulation's case,
# since no call of the server's comes back into it yet.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('owned');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# refused runs a statement that must fail and returns its error.
sub refused
{
	my ($sql) = @_;
	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', "\\set VERBOSITY verbose\n$sql");
	return $stderr;
}

my ($ret, $stdout, $stderr) = $node->psql('postgres',
	'SELECT doltgres_test_owned(0, 7), doltgres_test_owned(1, 0)');
is($stdout, '0|7',
	'a record outlives the call that made it and is lent to the next by its handle');
like(
	$stderr,
	qr/WARNING:  resource was not closed: versioned owned record/,
	"a record whose end never came by its transaction's commit is released with Postgres' leak warning");

# The commit released it, and the next statement's read of its handle is
# refused.
like(
	refused('SELECT doltgres_test_owned(1, 0)'),
	qr/ERROR:  XX000: a handle names no live record of its kind/,
	"a statement's end releases a record it made, and its handle is refused after");

# Each backend counts its own drops, so the counts are read in one session.
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_owned(3, 0), doltgres_test_owned(0, 5), doltgres_test_owned(2, 0), doltgres_test_owned(3, 0)'
	),
	'0|0|0|1',
	"a record's end forgets it, and its handle is refused after");
like(
	refused(
		'SELECT doltgres_test_owned(0, 5), doltgres_test_owned(2, 0), doltgres_test_owned(1, 0)'),
	qr/ERROR:  XX000: a handle names no live record of its kind/,
	'a stale handle is refused');
like(
	refused(
		'SELECT doltgres_test_owned(0, 5), doltgres_test_owned(2, 0), doltgres_test_owned(2, 0)'),
	qr/ERROR:  XX000: a handle names no live record of its kind/,
	'a duplicated handle ends no record twice');
like(
	refused('SELECT doltgres_test_owned(0, 5), doltgres_test_owned(4, 0)'),
	qr/ERROR:  XX000: a handle names no live record of its kind/,
	'a forged handle is refused');

# An ERROR's abort releases a record whose end never came, once, and raises
# nothing: the session goes on, with only the ERROR said. The divisor is
# volatile, so the ERROR comes after the record is made, not at planning.
($ret, $stdout, $stderr) = $node->psql(
	'postgres', q{
SELECT doltgres_test_owned(3, 0);
BEGIN;
SELECT doltgres_test_owned(0, 9), 1 / (random() * 0)::int;
ROLLBACK;
SELECT doltgres_test_owned(3, 0);
},
	on_error_stop => 0);
my ($before, $after) = split /\n/, $stdout;
ok( $after == $before + 1
	  && $stderr =~ /ERROR:  division by zero/
	  && $stderr !~ /WARNING|PANIC|FATAL/,
	"an ERROR's release drops a record whose end never came, raising nothing");

# Design item 13's Order 2a, the server's case: a call into Postgres that comes
# back into the fork for the record its caller holds is refused. Step 5 lends
# the backend's test record, which the same statement made, to a comparison
# through a btree comparison function, a SQL function that reads the same
# record.
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION owned_cmp(int4, int4) RETURNS int4 LANGUAGE sql
AS $$ SELECT btint4cmp($1, $2) + 0 * doltgres_test_owned(1, 0)::int4 $$;
});
($stdout, $stderr) = ('', '');
$node->psql(
	'postgres', q{
\set VERBOSITY verbose
SELECT doltgres_test_owned(0, 7),
  doltgres_test_owned(5, 'owned_cmp(int4, int4)'::regprocedure::oid::int8);
},
	stdout => \$stdout,
	stderr => \$stderr,
	on_error_stop => 0);
like(
	$stderr,
	qr/ERROR:  XX000: a record of the fork's is in use by a call that comes back into it/,
	"a call into Postgres that comes back into the fork for the record its caller holds is refused");

$node->stop;
done_testing();
