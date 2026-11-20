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

# The WAL a checkpoint keeps for a manifest's fallback (the repository design
# text, section 7): with only initdb's repositories, a checkpoint recycles WAL as Postgres
# does, keeping nothing before its redo point's segment; and the check of the
# kept WAL warns of a segment cut short, naming it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('kept_wal');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# With only initdb's repositories, the oldest segment a checkpoint leaves is its own redo
# point's, as Postgres' KeepLogSeg keeps it with no slot and no wal_keep_size.
$node->safe_psql('postgres', 'CREATE TABLE written (i int)');
for my $i (1 .. 4)
{
	$node->safe_psql('postgres',
		"INSERT INTO written VALUES ($i); SELECT pg_switch_wal()");
}
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'CHECKPOINT');
is( $node->safe_psql(
		'postgres',
		"SELECT min(name) = (SELECT redo_wal_file FROM pg_control_checkpoint()) FROM pg_ls_waldir() WHERE name ~ '^[0-9A-F]{24}\$'"
	),
	't',
	"a checkpoint with only initdb's repositories recycles WAL as Postgres does");

# A segment of the kept range cut short is warned of, naming the segment.
$node->safe_psql('postgres', "ALTER SYSTEM SET wal_keep_size = '1GB'");
$node->reload;
my $from = $node->safe_psql('postgres',
	"SELECT (pg_current_wal_insert_lsn() - '0/0')::int8");
my @names;
for my $i (1 .. 3)
{
	push @names,
	  $node->safe_psql('postgres',
		"INSERT INTO written VALUES ($i); SELECT pg_walfile_name(pg_current_wal_insert_lsn())"
	  );
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
}
my $to = $node->safe_psql('postgres',
	"SELECT (pg_current_wal_insert_lsn() - '0/0')::int8");
my $cut = $node->data_dir . "/pg_wal/$names[1]";
truncate($cut, 4096) or die "could not cut $cut short: $!";
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', "SELECT doltgres_test_kept_wal($from, $to)");
like(
	$stderr,
	qr/WARNING:  could not read the WAL kept for a manifest's fallback in segment "$names[1]"/,
	'a kept segment cut short warns, naming it');
is($ret, 0, 'the check of the kept WAL raises nothing');

# A report at WARNING of an error, the fork's own or one Postgres raised, keeps
# its SQLSTATE and puts its context before its message, as a checkpoint warns
# of a database whose manifest it can't write once more.
for my $case ([ 5, 'XX001', 'a manifest the test damaged' ],
	[ 6, '58P01', 'could not open file' ])
{
	my ($which, $code, $text) = @$case;
	($ret, $stdout, $stderr) = $node->psql('postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_host($which)");
	like(
		$stderr,
		qr/WARNING:  $code: the test's report: $text/,
		'a report at WARNING of an error keeps its SQLSTATE and puts its context first');
}

# A range whose last record ends on a page boundary ends with that record: the
# check reads nothing at the range's end, past the next page's header, where
# the WAL may hold no record yet (bugs row 58). A message sized to fill its
# page is flushed, and the range ends at the insert position after it.
my $filled = 0;
for my $try (1 .. 50)
{
	$from = $node->safe_psql('postgres',
		"SELECT (pg_current_wal_insert_lsn() - '0/0')::int8");
	my $end = $node->safe_psql('postgres',
		"SELECT (pg_logical_emit_message(false, '', repeat('x', greatest(8192 - (pg_current_wal_insert_lsn() - '0/0')::int8 % 8192 - 54, 231)::int), true) - '0/0')::int8"
	);
	$to = $node->safe_psql('postgres',
		"SELECT (pg_current_wal_insert_lsn() - '0/0')::int8");
	next if $end % 8192 != 0 || ($to - $end != 24 && $to - $end != 40);
	$filled = 1;
	last;
}
ok($filled, "a record ends on a page boundary with nothing after it");
($ret, $stdout, $stderr) =
  $node->psql('postgres', "SELECT doltgres_test_kept_wal($from, $to)");
is($stderr, '',
	'a range whose last record ends on a page boundary warns of nothing');

$node->stop;
done_testing();
