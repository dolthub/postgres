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


# The fork's serializable reads (PLAN item 7's serializable rule; step (5)),
# through the test module: two serializable transactions each insert a row
# and then read the other's table, each read examining the other's running
# insert, which conflicts out with it, so one of them fails with Postgres'
# 40001, as the same steps over heap tables fail. Each of the fork's read
# paths is one case: a seq scan, a tid range scan, a parallel scan, a fetch by
# tid, and an index fetch by row key; each heap case reads its tables the same
# way heap's executor would.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('serializable');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_a (a int4, b text);
INSERT INTO source_a VALUES (1, 'a');
CREATE TABLE source_b (a int4, b text);
INSERT INTO source_b VALUES (2, 'b');
});

# step runs a statement in a session and returns "ok", or the first line of
# the error it raised.
sub step
{
	my ($session, $sql) = @_;
	my ($output) = $session->query($sql);
	my ($error) = $session->{stderr} =~ /(ERROR:[^\n]*)/;
	$session->{stderr} = '';
	return $error // 'ok';
}

# skew runs a write skew over two tables made by a statement: each of two
# serializable transactions inserts a row into its own table by a statement
# that returns the row's ctid, then reads the other's table by a statement of
# that table and the other row's ctid, then each commits; it returns each
# step's outcome in order.
sub skew
{
	my ($tag, $make, $insert, $read) = @_;
	my ($table_a, $table_b) = ("${tag}_a", "${tag}_b");
	$node->safe_psql('postgres', $make->($table_a) . $make->($table_b));
	my $first = $node->background_psql('postgres', on_error_stop => 0);
	my $second = $node->background_psql('postgres', on_error_stop => 0);
	my @steps;
	push @steps, step($first, 'BEGIN ISOLATION LEVEL SERIALIZABLE');
	push @steps, step($second, 'BEGIN ISOLATION LEVEL SERIALIZABLE');
	my ($tid_a) = $first->query($insert->($table_a, 'source_a'));
	my ($tid_b) = $second->query($insert->($table_b, 'source_b'));
	push @steps, step($first, $read->($table_b, $tid_b, 'source_b'));
	push @steps, step($second, $read->($table_a, $tid_a, 'source_a'));
	push @steps, step($first, 'COMMIT');
	push @steps, step($second, 'COMMIT');
	$first->quit;
	$second->quit;
	return join("\n", @steps);
}

my $heap_make = sub { "CREATE TABLE $_[0] (a int4 PRIMARY KEY, b text);" };
my $heap_insert =
  sub { "INSERT INTO $_[0] SELECT * FROM $_[1] RETURNING ctid" };
my $fork_make = $heap_make;
my $fork_insert = sub {
	"SELECT doltgres_test_insert('$_[0]', '$_[1]', false, 0)";
};

my %cases = (
	'seq' => [
		sub { "SELECT count(*) FROM $_[0]" },
		sub { "SELECT count(*) FROM doltgres_test_scan('$_[0]', false, -1)" },
	],
	'tidrange' => [
		sub {
			"SELECT count(*) FROM $_[0] WHERE ctid >= '(0,1)' AND ctid <= '(0,10)'";
		},
		sub {
			"SELECT count(*) FROM doltgres_test_tidrange('$_[0]', '(0,1)', '(0,10)', false)";
		},
	],
	'parallel' => [
		sub { "SELECT count(*) FROM $_[0]" },
		sub {
			"SELECT count(*) FROM doltgres_test_parallel('$_[0]', 2, false, 0, NULL, NULL)";
		},
	],
	'fetch' => [
		sub { "SELECT count(*) FROM $_[0] WHERE ctid = '$_[1]'" },
		sub { "SELECT count(doltgres_test_fetch('$_[0]', '$_[1]'))" },
	],
	'index' => [
		sub {
			"SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM $_[0] WHERE a = (SELECT a FROM $_[2]); RESET enable_seqscan; RESET enable_bitmapscan";
		},
		sub {
			"SELECT count(*) FROM doltgres_test_index_fetch_key('$_[0]', '$_[2]', 0) r WHERE r <> 'none'";
		},
	],);

foreach my $case (sort keys %cases)
{
	my ($heap_read, $fork_read) = @{ $cases{$case} };
	my $heap = skew("heap_$case", $heap_make, $heap_insert, $heap_read);
	like(
		$heap,
		qr/ERROR:  could not serialize access/,
		"heap's $case case is a write skew it refuses");
	note "the $case reads";
	is( skew("fork_$case", $fork_make, $fork_insert, $fork_read),
		$heap,
		"a write skew between two serializable transactions over the fork's reads is refused with 40001, as heap's is"
	);
}

# (4b)'s section 7: two serializable transactions each read one table and
# delete the other's rows, a write skew through deletes, which heap refuses with
# 40001; the fork's deletes check their conflicts in on the rows' tuples, so it
# refuses it too. Each step is "ok" or its error's first line.
sub deleting
{
	my ($tag, $fork) = @_;
	my ($table_a, $table_b) = ("${tag}_a", "${tag}_b");
	my $fill =
	  $fork
	  ? "SELECT doltgres_test_insert('$table_a', 'source_a', false, 0); SELECT doltgres_test_insert('$table_b', 'source_b', false, 0);"
	  : "INSERT INTO $table_a SELECT * FROM source_a; INSERT INTO $table_b SELECT * FROM source_b;";
	$node->safe_psql('postgres', $heap_make->($table_a) . $heap_make->($table_b) . $fill);
	my $read = sub {
		$fork ? "SELECT count(*) FROM doltgres_test_scan('$_[0]', false, -1)" : "SELECT count(*) FROM $_[0]";
	};
	my $delete = sub {
		$fork ? "SELECT doltgres_test_write('$_[0]', true, 'DELETE FROM $_[0]')" : "DELETE FROM $_[0]";
	};
	my ($first, $second) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
	$_->query_safe('SET enable_indexscan = off; SET enable_bitmapscan = off') for $first, $second;
	my @steps;
	push @steps, step($first, 'BEGIN ISOLATION LEVEL SERIALIZABLE');
	push @steps, step($second, 'BEGIN ISOLATION LEVEL SERIALIZABLE');
	push @steps, step($first, $read->($table_a));
	push @steps, step($second, $read->($table_b));
	push @steps, step($first, $delete->($table_b));
	push @steps, step($second, $delete->($table_a));
	push @steps, step($first, 'COMMIT');
	push @steps, step($second, 'COMMIT');
	$_->quit for $first, $second;
	return join("\n", @steps);
}

my $heap_deleting = deleting('heap_deleting', 0);
like($heap_deleting, qr/ERROR:  could not serialize access/, "heap's deletes are a write skew it refuses");
is(deleting('fork_deleting', 1),
	$heap_deleting, "a delete of a row a serializable reader read is refused with 40001, as heap's is");

$node->stop;

done_testing();
