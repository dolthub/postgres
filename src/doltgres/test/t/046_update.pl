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


# The fork's deletes, updates, and row locks ((4b)), through the test module:
# doltgres_test_write runs a statement with the fork's table access method
# routine standing as its table's own, so the executor changes the table's rows
# through it, or through heap's for a heap table. Each case runs the same steps over a heap table of the same rows
# and compares what each step gives, so heap's answers are the expectations.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('update');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# made makes a fork table and a heap table, each of a definition, and fills
# both with the rows a source query gives, the fork's through the fork's
# inserts.
sub made
{
	my ($name, $definition, $rows) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE ${name}_source AS $rows;
CREATE TABLE $name $definition;
CREATE TABLE heap_$name $definition;
INSERT INTO heap_$name SELECT * FROM ${name}_source;
SELECT doltgres_test_insert('$name', '${name}_source', false, 0);
});
}

# written returns the SQL that runs a statement over a table, with "%" in the
# statement standing for the table's name, through the fork's routine for a fork
# table and through heap's for a heap table, each giving the count of rows the
# statement processed; a statement with no "%" runs as it is.
sub written
{
	my ($table, $statement) = @_;
	return $statement if $statement !~ /%/;
	(my $sql = $statement) =~ s/%/$table/g;
	$sql =~ s/'/''/g;
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	return "SELECT doltgres_test_write('$table', $fork, '$sql')";
}

# reading returns the SQL that reads a table's rows as text in their order, with
# their ctids, and with their cmin and cmax when asked, through the fork's
# routine for a fork table.
sub reading
{
	my ($table, $commands) = @_;
	my $shown = $commands ? "ctid::text || ' ' || cmin || ' ' || cmax" : 'ctid::text';
	my $query = "SELECT $shown || ' ' || ${table}::text FROM $table ORDER BY ctid";
	return $table =~ /^heap_/
	  ? $query
	  : "SELECT doltgres_test_standing('$table', '" . ($query =~ s/'/''/gr) . "')";
}

# step runs a statement in a session and returns its output, or the first line
# of the error it raised.
sub step
{
	my ($session, $sql) = @_;
	my ($output) = $session->query($sql);
	my ($error) = $session->{stderr} =~ /(ERROR:[^\n]*)/;
	$session->{stderr} = '';
	return $error // $output;
}

# both runs steps over a fork table and over its heap table, each step a
# session's number and a statement over "%", or a read of "%" when the
# statement is "read", and checks that each gives what heap's gives. A
# statement after "start " is sent without waiting for its end, "blocked" waits
# until a session waits on a lock, and "finish" collects what the started
# statement gave. Its sessions read by scans alone, since the fork's index
# method isn't there yet and a heap index over a fork table holds none of its
# rows.
sub both
{
	my ($name, $case, @steps) = @_;
	my %outputs;
	for my $table ($name, "heap_$name")
	{
		my @sessions = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
		$_->query_safe('SET enable_indexscan = off; SET enable_bitmapscan = off') for @sessions;
		for my $step (@steps)
		{
			my ($session, $statement) = @$step;
			if ($statement =~ /^start (.*)/)
			{
				my $sql = written($table, $1);
				$sessions[$session]->query_until(qr/started/, "\\echo started\n$sql;\n");
				next;
			}
			if ($statement eq 'blocked')
			{
				$node->poll_query_until('postgres',
					'SELECT count(*) > 0 FROM pg_locks WHERE NOT granted')
				  or die 'no session waited';
				next;
			}
			my $sql =
				$statement eq 'read' ? reading($table)
			  : $statement eq 'commands' ? reading($table, 1)
			  : $statement eq 'finish' ? 'SELECT 1'
			  : written($table, $statement);
			push @{ $outputs{$table} }, step($sessions[$session], $sql) =~ s/heap_//gr;
		}
		$_->quit for @sessions;
	}
	is_deeply($outputs{$name}, $outputs{"heap_$name"}, $case);
}

made('deleted', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 3) k");
both(
	'deleted',
	'a row deleted is gone for a new snapshot and kept for an older one',
	[ 1, 'BEGIN ISOLATION LEVEL REPEATABLE READ' ],
	[ 1, 'read' ],
	[ 0, 'DELETE FROM % WHERE k = 2' ],
	[ 0, 'read' ],
	[ 1, 'read' ],
	[ 1, 'COMMIT' ],
	[ 1, 'read' ]);

made('copies', '(k int4, v text)', "SELECT 1 AS k, 'v' AS v FROM generate_series(1, 3)");
both(
	'copies',
	'a keyless table\'s delete of one copy leaves its other copies',
	[ 0, "DELETE FROM % WHERE ctid = '(0,2)'" ],
	[ 0, 'read' ]);

made('waited', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 3) k");
both(
	'waited',
	'a second deleter of a row waits for the first\'s transaction, then deletes nothing after its commit',
	[ 0, 'BEGIN' ],
	[ 0, 'DELETE FROM % WHERE k = 1' ],
	[ 1, 'SET lock_timeout = 200' ],
	[ 1, 'DELETE FROM % WHERE k = 1' ],
	[ 1, 'RESET lock_timeout' ],
	[ 0, 'COMMIT' ],
	[ 1, 'DELETE FROM % WHERE k = 1' ],
	[ 0, 'read' ]);

both(
	'waited',
	'a second deleter of a row deletes it after the first\'s abort',
	[ 0, 'BEGIN' ],
	[ 0, 'DELETE FROM % WHERE k = 2' ],
	[ 0, 'ROLLBACK' ],
	[ 1, 'DELETE FROM % WHERE k = 2' ],
	[ 0, 'read' ]);

made('updated', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 3) k");
both(
	'updated',
	'a row updated reads back with its new ctid, the old one kept for an older snapshot',
	[ 1, 'BEGIN ISOLATION LEVEL REPEATABLE READ' ],
	[ 1, 'read' ],
	[ 0, "UPDATE % SET v = 'w' WHERE k = 2" ],
	[ 0, 'read' ],
	[ 1, 'read' ],
	[ 1, 'COMMIT' ],
	[ 0, 'UPDATE % SET k = 5 WHERE k = 1' ],
	[ 0, 'read' ]);

both(
	'updated',
	'an insert of a live row\'s key is refused as heap\'s is',
	[ 0, 'UPDATE % SET k = 2 WHERE k = 3' ],
	[ 0, 'read' ]);

both(
	'updated',
	'a second updater of a row waits for the first\'s transaction',
	[ 0, 'BEGIN' ],
	[ 0, "UPDATE % SET v = 'x' WHERE k = 3" ],
	[ 1, 'SET lock_timeout = 200' ],
	[ 1, "UPDATE % SET v = 'y' WHERE k = 3" ],
	[ 1, 'RESET lock_timeout' ],
	[ 0, 'ROLLBACK' ],
	[ 1, "UPDATE % SET v = 'y' WHERE k = 3" ],
	[ 0, 'read' ]);

made('moved', '(k int4, v text)', "SELECT 1 AS k, 'v' || n AS v FROM generate_series(1, 3) n");
both(
	'moved',
	'a keyless table\'s update of one copy moves it and leaves its other copies',
	[ 0, "UPDATE % SET v = 'w' WHERE ctid = '(0,2)'" ],
	[ 0, 'read' ]);

made('locked', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 3) k");
both(
	'locked',
	'a second updater of a row follows the first\'s committed update to the row\'s new version',
	[ 0, 'BEGIN' ],
	[ 0, "UPDATE % SET v = 'x' WHERE k = 1" ],
	[ 1, "start UPDATE % SET v = v || 'y' WHERE k = 1" ],
	[ 0, 'blocked' ],
	[ 0, 'COMMIT' ],
	[ 1, 'finish' ],
	[ 0, 'read' ]);

both(
	'locked',
	'a NOWAIT locker of a locked row is refused as heap\'s is',
	[ 0, 'BEGIN' ],
	[ 0, 'SELECT * FROM % WHERE k = 2 FOR UPDATE' ],
	[ 1, 'SELECT * FROM % WHERE k = 2 FOR UPDATE NOWAIT' ],
	[ 1, 'SELECT * FROM % FOR UPDATE SKIP LOCKED' ],
	[ 0, 'SELECT * FROM % WHERE k = 2 FOR SHARE' ],
	[ 0, 'COMMIT' ],
	[ 1, 'SELECT * FROM % WHERE k = 2 FOR UPDATE NOWAIT' ]);

both(
	'locked',
	'a key-share lock lets a no-key update by another transaction through and holds a key update back',
	[ 0, 'BEGIN' ],
	[ 0, 'SELECT * FROM % WHERE k = 3 FOR KEY SHARE' ],
	[ 1, 'SET lock_timeout = 200' ],
	[ 1, "UPDATE % SET v = 'z' WHERE k = 3" ],
	[ 1, 'UPDATE % SET k = 9 WHERE k = 3' ],
	[ 1, 'RESET lock_timeout' ],
	[ 0, 'COMMIT' ],
	[ 0, 'read' ]);

made('refused', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 2) k");
both(
	'refused',
	"an insert refused for a live row's key spends its tuple id as heap's does",
	[ 0, "INSERT INTO % VALUES (1, 'again')" ],
	[ 0, "INSERT INTO % VALUES (3, 'v3')" ],
	[ 0, 'read' ]);

made('latest', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 3) k");
$node->safe_psql('postgres', "SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT doltgres_test_write('latest', true, 'UPDATE latest SET k = 5 WHERE k = 1')");
$node->safe_psql('postgres', "SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT doltgres_test_write('latest', true, 'UPDATE latest SET v = ''w'' WHERE k = 5')");
is($node->safe_psql('postgres', "SELECT doltgres_test_latest('latest', '(0,1)')"),
	'(0,5)', "a moved row's latest tid is its new version's");

made('serial', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 2) k");
both(
	'serial',
	'a repeatable read updater of a row another transaction updated since its snapshot fails with 40001',
	[ 0, 'BEGIN ISOLATION LEVEL REPEATABLE READ' ],
	[ 0, 'read' ],
	[ 1, "UPDATE % SET v = 'r' WHERE k = 1" ],
	[ 0, "UPDATE % SET v = 's' WHERE k = 1" ],
	[ 0, 'ROLLBACK' ],
	[ 0, 'read' ]);

both(
	'serial',
	"two updaters waiting on each other are broken by the deadlock detector as heap's are",
	[ 0, 'BEGIN' ],
	[ 1, 'BEGIN' ],
	[ 0, "UPDATE % SET v = 'a' WHERE k = 1" ],
	[ 1, "UPDATE % SET v = 'b' WHERE k = 2" ],
	[ 0, "start UPDATE % SET v = 'a' WHERE k = 2" ],
	[ 0, 'blocked' ],
	[ 1, "UPDATE % SET v = 'b' WHERE k = 1" ],
	[ 0, 'finish' ],
	[ 0, 'ROLLBACK' ],
	[ 1, 'COMMIT' ],
	[ 0, 'read' ]);

made('invisible', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 2) k");
my %invisible;
for my $table ('invisible', 'heap_invisible')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $inserter = $node->background_psql('postgres', on_error_stop => 0);
	$inserter->query_safe('BEGIN');
	$inserter->query_safe("SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (3, ''v3'')')");
	my $deleter = $node->background_psql('postgres', on_error_stop => 0);
	$invisible{$table} = step($deleter, "SELECT doltgres_test_delete('$table', $fork, '(0,3)')");
	$inserter->query_safe('ROLLBACK');
	$_->quit for $inserter, $deleter;
}
is($invisible{invisible}, $invisible{heap_invisible},
	"a delete of a row another running transaction inserted raises heap's error");

made('rekeyed', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 3) k");
both(
	'rekeyed',
	"a key change is the old key's row for an older snapshot and the new key's for a new one",
	[ 1, 'BEGIN ISOLATION LEVEL REPEATABLE READ' ],
	[ 1, 'read' ],
	[ 0, 'UPDATE % SET k = 7 WHERE k = 1' ],
	[ 0, 'read' ],
	[ 1, 'read' ],
	[ 1, 'COMMIT' ]);

both(
	'rekeyed',
	'a key changed and changed back takes a put on its first key after its move',
	[ 0, 'UPDATE % SET k = 8 WHERE k = 7' ],
	[ 0, 'UPDATE % SET k = 7 WHERE k = 8' ],
	[ 0, 'read' ]);

both(
	'rekeyed',
	"a key its own transaction's delete freed takes a row moved onto it, away, and back",
	[ 0, 'BEGIN' ],
	[ 0, 'DELETE FROM % WHERE k = 2' ],
	[ 0, 'UPDATE % SET k = 2 WHERE k = 7' ],
	[ 0, 'UPDATE % SET k = 7 WHERE k = 2' ],
	[ 0, 'UPDATE % SET k = 2 WHERE k = 7' ],
	[ 0, 'COMMIT' ],
	[ 0, 'read' ]);

both(
	'rekeyed',
	"a waiting updater of a row whose key moved follows it to its new key",
	[ 0, 'BEGIN' ],
	[ 0, 'UPDATE % SET k = 9 WHERE k = 3' ],
	[ 1, "start UPDATE % SET v = 'w' WHERE k = 3" ],
	[ 0, 'blocked' ],
	[ 0, 'COMMIT' ],
	[ 1, 'finish' ],
	[ 0, 'read' ]);

made('combo', '(k int4 PRIMARY KEY, v text)', "SELECT 0 AS k, 'v' AS v WHERE false");
both(
	'combo',
	"a row its own transaction inserted and then changed shows the combo command id as heap's does",
	[ 0, 'BEGIN' ],
	[ 0, "INSERT INTO % VALUES (1, 'a'), (2, 'b')" ],
	[ 0, 'DELETE FROM % WHERE k = 1' ],
	[ 0, 'SAVEPOINT s' ],
	[ 0, 'DELETE FROM % WHERE k = 2' ],
	[ 0, 'ROLLBACK TO s' ],
	[ 0, 'commands' ],
	[ 0, "UPDATE % SET v = 'c' WHERE k = 2" ],
	[ 0, 'commands' ],
	[ 0, 'COMMIT' ],
	[ 0, 'commands' ]);

$node->safe_psql(
	'postgres', q{
CREATE TABLE kept_source AS SELECT 1 AS k, string_agg(md5(n::text), '') AS v, 1 AS w FROM generate_series(1, 300) n;
CREATE TABLE kept (k int4 PRIMARY KEY, v text, w int4);
ALTER TABLE kept ALTER COLUMN v SET STORAGE EXTERNAL;
CREATE TABLE heap_kept (LIKE kept INCLUDING ALL);
INSERT INTO heap_kept SELECT * FROM kept_source;
SELECT doltgres_test_insert('kept', 'kept_source', false, 0);
});
my %kept;
for my $table ('kept', 'heap_kept')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $read = "SELECT pg_column_toast_chunk_id(v)::text || ' ' || length(v) FROM $table";
	my $id = sub {
		$node->safe_psql('postgres',
			$fork eq 'true' ? "SELECT doltgres_test_standing('$table', '" . ($read =~ s/'/''/gr) . "')" : $read);
	};
	my $before = $id->();
	$node->safe_psql('postgres',
		"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT doltgres_test_write('$table', $fork, 'UPDATE $table SET w = 2')");
	my $after = $id->();
	$kept{$table} = ($before eq $after ? 'kept ' : 'moved ') . ($after =~ s/^\d+ //r);
}
is($kept{kept}, $kept{heap_kept}, 'an update that keeps a value out of line keeps its id and counts it twice');
is($kept{kept}, 'kept 9600', 'the kept value reads back whole');

made('striped', '(k int4 PRIMARY KEY, v text)', "SELECT k, 'v' || k AS v FROM generate_series(1, 2) k");
{
	my ($first, $second) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
	$_->query_safe('SET enable_indexscan = off; SET enable_bitmapscan = off') for $first, $second;
	# The updater attaches the point itself after setting it local, as
	# Postgres' own tests of a point in a critical section do, so the test
	# module's state is set up before its update's critical window waits there.
	$first->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-insert-logged', 'wait')");
	$first->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_write('striped', true, 'UPDATE striped SET v = ''x'' WHERE k = 1');\n");
	$node->wait_for_event('client backend', 'doltgres-insert-logged');
	my $tuple_locks = $node->safe_psql('postgres', "SELECT count(*) FROM pg_locks WHERE locktype = 'tuple'");
	$second->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_write('striped', true, 'UPDATE striped SET v = ''y'' WHERE k = 1');\n");
	my $waited = $node->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'versioned key'");
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-insert-logged'); SELECT injection_points_wakeup('doltgres-insert-logged')");
	$first->query_safe('SELECT 1');
	$second->query_safe('SELECT 1');
	$_->quit for $first, $second;
	my $row = $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('striped', 'SELECT v FROM striped WHERE k = 1')");
	ok($tuple_locks eq '0' && $waited && $row eq 'y', "an update takes its key's stripe");
}

made('deferred', '(k int4 PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v text)',
	"SELECT k, 'v' || k AS v FROM generate_series(1, 2) k");
both(
	'deferred',
	"a deferrable key's rows go beside a live row",
	[ 0, 'BEGIN' ],
	[ 0, 'UPDATE % SET k = 2 WHERE k = 1' ],
	[ 0, 'read' ],
	[ 0, 'ROLLBACK' ],
	[ 0, 'read' ]);

$node->safe_psql('postgres',
	"CREATE COLLATION ignoring_case (provider = icu, locale = 'und-u-ks-level2', deterministic = false)");
made('collated', '(k text COLLATE ignoring_case PRIMARY KEY, v text)', "SELECT 'b'::text AS k, 'v' AS v");
both(
	'collated',
	'two writers of a text key under a nondeterministic collation, one waiting',
	[ 0, 'BEGIN' ],
	[ 0, "INSERT INTO % VALUES ('a', 'x')" ],
	[ 1, 'SET lock_timeout = 200' ],
	[ 1, "INSERT INTO % VALUES ('A', 'y')" ],
	[ 1, 'RESET lock_timeout' ],
	[ 0, 'ROLLBACK' ],
	[ 1, "INSERT INTO % VALUES ('A', 'y')" ],
	[ 1, "INSERT INTO % VALUES ('a', 'z')" ],
	[ 0, 'read' ]);

# (4b)'s speculative insert through the test module's direct calls, as
# ExecInsert makes them for INSERT ... ON CONFLICT (the SQL cases come with the
# fork's btree): each step's ctid and the rows after, the fork's and heap's.
made('speculated', '(k int4 PRIMARY KEY, v text)', "SELECT 1 AS k, 'v1' AS v");
my %speculated;
for my $table ('speculated', 'heap_speculated')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $speculate = sub {
		my ($k, $v, $succeeded) = @_;
		return $node->safe_psql('postgres',
			"SELECT doltgres_test_speculative('$table', $fork, $k, '$v', $succeeded, true)");
	};
	my $rows = sub { $node->safe_psql('postgres', reading($table)) =~ s/heap_//gr };
	$speculated{$table} = [
		$speculate->(2, 'v2', 'true'), $rows->(),
		$speculate->(3, 'v3', 'false'), $rows->(),
		$speculate->(1, 'again', 'false'), $rows->(),
		$speculate->(4, 'v4', 'true'), $rows->()
	];
}
my ($fork_steps, $heap_steps) = ($speculated{speculated}, $speculated{heap_speculated});
is_deeply([ @$fork_steps[ 0, 1 ] ], [ @$heap_steps[ 0, 1 ] ], 'a speculative insert confirmed reads back as an insert does');
is_deeply([ @$fork_steps[ 2, 3 ] ], [ @$heap_steps[ 2, 3 ] ],
	'a speculative insert completed as failed after it went live is killed as heap kills it');
is_deeply([ @$fork_steps[ 4 .. 7 ] ], [ @$heap_steps[ 4 .. 7 ] ],
	"a speculative insert onto a live key spends its place, and its completion kills nothing more");

{
	my ($speculator, $inserter) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
	$speculator->query_safe("BEGIN; SELECT doltgres_test_speculative('speculated', true, 5, 'v5', true, false)");
	$inserter->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_write('speculated', true, 'INSERT INTO speculated VALUES (5, ''w5'')');\n");
	my $waited = $node->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_locks WHERE locktype = 'spectoken' AND NOT granted");
	$speculator->query_safe('ROLLBACK');
	my $inserted = $inserter->query_safe('SELECT 1');
	$_->quit for $speculator, $inserter;
	ok($waited && $inserted =~ /^1/, "a resolve waits on a running speculative insert's token");
}

# (4b)'s index_delete_tuples through the test module's direct calls, as an
# index's simple and bottom-up deletions make them, over rows on three blocks:
# rows deleted before a held snapshot, rows deleted after it, a row a running
# transaction deletes, and an entry the caller knows deletable. The same
# transactions write both tables, so each table's marks and conflict horizon
# compare as they are.
made('pruned', '(k int4 PRIMARY KEY, v text)', "SELECT k, repeat('x', 1000) AS v FROM generate_series(1, 20) k");
my $scans = 'SET enable_indexscan = off; SET enable_bitmapscan = off;';
my %tids = map {
	my $query = "SELECT array_agg(ctid ORDER BY ctid) FROM $_";
	($_ => $node->safe_psql('postgres',
		/^heap_/ ? $query : "SELECT doltgres_test_standing('$_', '$query')"))
} ('pruned', 'heap_pruned');
is($tids{pruned}, $tids{heap_pruned}, "an index's deleted entries' rows sit where heap's sit");
my $deleted = sub {
	my ($keys) = @_;
	return "SELECT doltgres_test_write('pruned', true, 'DELETE FROM pruned WHERE k IN ($keys)');"
	  . " DELETE FROM heap_pruned WHERE k IN ($keys);";
};
$node->safe_psql('postgres', "$scans BEGIN; " . $deleted->('2, 5, 9, 16') . ' COMMIT;');
my $held = $node->background_psql('postgres');
$held->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;');
$node->safe_psql('postgres', "$scans BEGIN; " . $deleted->('1, 12') . ' COMMIT;');
my $running = $node->background_psql('postgres');
$running->query_safe("$scans BEGIN; " . $deleted->('7'));
my %marked;
for my $table ('pruned', 'heap_pruned')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $known = 'ARRAY[' . join(', ', map { $_ == 20 ? 'true' : 'false' } 1 .. 20) . ']';
	my $promising = 'ARRAY[' . join(', ', map { $_ >= 8 && $_ <= 14 ? 'true' : 'false' } 1 .. 20) . ']';
	my $none = 'ARRAY[' . join(', ', ('false') x 20) . ']';
	$marked{$table} = [
		$node->safe_psql('postgres',
			"SELECT doltgres_test_index_delete('$table', $fork, '$tids{$table}', $known, $none, -1)"),
		$node->safe_psql('postgres',
			"SELECT doltgres_test_index_delete('$table', $fork, '$tids{$table}', $none, $promising, 40)")
	];
}
$running->query_safe('COMMIT');
$running->quit;
$held->quit;
is($marked{pruned}[0], $marked{heap_pruned}[0],
	"an index's deletion of a fork table's dead entries marks those heap's marks");
is($marked{pruned}[1], $marked{heap_pruned}[1],
	"an index's bottom-up deletion visits and marks a fork table's blocks as heap's");

made('combo_updated', '(k int4 PRIMARY KEY, v text)', "SELECT 0 AS k, 'v' AS v WHERE false");
both(
	'combo_updated',
	"a row its own transaction's rolled-back update changed shows the update's combo command id as heap's does",
	[ 0, 'BEGIN' ],
	[ 0, "INSERT INTO % VALUES (1, 'a'), (2, 'b')" ],
	[ 0, 'SAVEPOINT s' ],
	[ 0, "UPDATE % SET v = 'x' WHERE k = 2" ],
	[ 0, 'ROLLBACK TO s' ],
	[ 0, 'commands' ],
	[ 0, 'SAVEPOINT t' ],
	[ 0, 'UPDATE % SET k = 5 WHERE k = 1' ],
	[ 0, 'ROLLBACK TO t' ],
	[ 0, 'commands' ],
	[ 0, 'COMMIT' ],
	[ 0, 'commands' ]);

made('combo_locked', '(k int4 PRIMARY KEY, v text)', "SELECT 0 AS k, 'v' AS v WHERE false");
both(
	'combo_locked',
	"a row its own transaction's rolled-back SELECT FOR UPDATE locked shows the command heap's does",
	[ 0, 'BEGIN' ],
	[ 0, "INSERT INTO % VALUES (1, 'a'), (2, 'b')" ],
	[ 0, 'SAVEPOINT s' ],
	[ 0, 'SELECT * FROM % WHERE k = 2 FOR UPDATE' ],
	[ 0, 'ROLLBACK TO s' ],
	[ 0, 'commands' ],
	[ 0, 'DELETE FROM % WHERE k = 1' ],
	[ 0, 'commands' ],
	[ 0, 'COMMIT' ],
	[ 0, 'commands' ]);

# (4b)'s resolve of a key its class compares by a function, whose equal values
# can have other bytes (citext): an insert equal to a running insert's key
# waits, one equal to a committed row's or to the base's is refused with 23505,
# as heap's btree answers.
$node->safe_psql('postgres', 'CREATE EXTENSION citext');
made('compared', '(k citext PRIMARY KEY, v text)', "SELECT 'b'::citext AS k, 'v' AS v");
both(
	'compared',
	'writers of keys equal under a compared class but other in bytes, one waiting, then refused as heap refuses them',
	[ 0, 'BEGIN' ],
	[ 0, "INSERT INTO % VALUES ('a', 'x')" ],
	[ 1, 'SET lock_timeout = 200' ],
	[ 1, "INSERT INTO % VALUES ('A', 'y')" ],
	[ 1, 'RESET lock_timeout' ],
	[ 0, 'ROLLBACK' ],
	[ 1, "INSERT INTO % VALUES ('A', 'y')" ],
	[ 1, "INSERT INTO % VALUES ('a', 'z')" ],
	[ 1, "INSERT INTO % VALUES ('B', 'z')" ],
	[ 0, 'read' ]);

# (4b)'s section 3: a write of a key equal to a resolve's key under its class
# but other in bytes, made between the resolve's read of its candidates and its
# stripe, moves the stripe's push count, so the resolve starts over and refuses
# its key, as heap's btree refuses the later of two such inserts.
made('raced', '(k citext PRIMARY KEY, v text)', "SELECT 'q'::citext AS k, 'v' AS v");
$node->safe_psql('postgres',
	"CREATE TABLE raced_upper AS SELECT 'A'::citext AS k, 'x'::text AS v; CREATE TABLE raced_lower AS SELECT 'a'::citext AS k, 'y'::text AS v"
);
{
	my $first = $node->background_psql('postgres', on_error_stop => 0);
	$first->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-insert-resolved', 'wait')");
	$first->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_insert('raced', 'raced_upper', false, 0);\n");
	$node->wait_for_event('client backend', 'doltgres-insert-resolved');
	$node->safe_psql('postgres', "SELECT doltgres_test_insert('raced', 'raced_lower', false, 0)");
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-insert-resolved'); SELECT injection_points_wakeup('doltgres-insert-resolved')"
	);
	my $raced = step($first, 'SELECT 1');
	$first->quit;
	$node->safe_psql('postgres', "INSERT INTO heap_raced VALUES ('a', 'y')");
	my ($code, $out, $heap) = $node->psql('postgres', "INSERT INTO heap_raced VALUES ('A', 'x')");
	($heap) = $heap =~ /(ERROR:[^\n]*)/;
	my $rows = $node->safe_psql('postgres', reading('raced'));
	my $heap_rows = $node->safe_psql('postgres', reading('heap_raced')) =~ s/heap_//gr;
	is_deeply([ $raced, $rows ], [ $heap =~ s/heap_//gr, $heap_rows ],
		"a resolve whose stripe moved since its read of a compared key's candidates starts over and refuses its key as heap does");
}

# (4b)'s lock of an updated row (heap_lock_updated_tuple): a key share lock
# taken while another transaction's update that keeps the key runs covers the
# row that update makes, so once the update commits, a delete of its row waits
# for the locker, as heap's lock of the update chain makes it; for a keyed
# table, a deferrable key's row-addressed rows, and a keyless table's.
for my $definition ('(k int4 PRIMARY KEY, v text)', '(k int4 PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v text)',
	'(k int4, v text)')
{
	my $name =
		$definition =~ /DEFERRABLE/ ? 'chained_deferred'
	  : $definition =~ /PRIMARY/ ? 'chained'
	  : 'chained_keyless';
	made($name, $definition, "SELECT k, 'v' || k AS v FROM generate_series(1, 2) k");
	both(
		$name,
		"a key share lock taken during another's update covers the updated row ($name)",
		[ 0, 'BEGIN' ],
		[ 0, "UPDATE % SET v = 'x' WHERE k = 1" ],
		[ 1, 'BEGIN' ],
		[ 1, 'SELECT k FROM % WHERE k = 1 FOR KEY SHARE' ],
		[ 0, 'COMMIT' ],
		[ 0, 'SET lock_timeout = 200' ],
		[ 0, 'DELETE FROM % WHERE k = 1' ],
		[ 1, 'COMMIT' ],
		[ 0, 'DELETE FROM % WHERE k = 1' ],
		[ 0, 'read' ]);
}

# (4b)'s section 3's hold-back flag: a resolve of a compared key, kept at each
# round's read by the test module's callback until the test unlocks that
# round's advisory lock, while another session writes a row to the same storage
# between its rounds, starts over three times and holds its stripe back at the
# third, so the next writer sleeps on the stripe's wait event until the resolve
# finishes; and a resolve cancelled or terminated while its flag is set lets
# its sleeper write.
$node->safe_psql('postgres',
	"CREATE TABLE held_r AS SELECT 'r'::citext AS k, 'v'::text AS v; "
	  . join(' ', map { "CREATE TABLE held_p$_ AS SELECT 'p$_'::citext AS k, 'v'::text AS v;" } 1 .. 4));
for my $ending ('finishes', 'cancelled', 'terminated')
{
	my $name = "held_$ending";
	made($name, '(k citext PRIMARY KEY, v text)', "SELECT 'q'::citext AS k, 'v' AS v");
	my $gate = $node->background_psql('postgres');
	$gate->query_safe('SELECT pg_advisory_lock(n) FROM generate_series(1, 4) n');
	my $resolver = $node->background_psql('postgres', on_error_stop => 0);
	my $pid = $resolver->query_safe(
		"SELECT injection_points_attach('doltgres-insert-resolved', 'doltgres_test', 'dg_round', int4send(pg_backend_pid())); SELECT pg_backend_pid()"
	) =~ s/\s+//gr;
	$resolver->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_insert('$name', 'held_r', false, 0);\n");
	my $at = sub {
		my ($round) = @_;
		$node->poll_query_until('postgres',
			"SELECT count(*) = 1 FROM pg_locks WHERE locktype = 'advisory' AND NOT granted AND objid = $round")
		  or die "the resolve never reached round $round";
	};
	for my $round (1 .. 3)
	{
		$at->($round);
		$node->safe_psql('postgres', "SELECT doltgres_test_insert('$name', 'held_p$round', false, 0)");
		$gate->query_safe("SELECT pg_advisory_unlock($round)");
	}
	$at->(4);
	my $pusher = $node->background_psql('postgres', on_error_stop => 0);
	$pusher->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_insert('$name', 'held_p4', false, 0);\n");
	my $slept = $node->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'versioned key'");
	$node->safe_psql('postgres', "SELECT pg_cancel_backend($pid)") if $ending eq 'cancelled';
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)") if $ending eq 'terminated';
	$gate->query_safe('SELECT pg_advisory_unlock(4)');
	$pusher->query_safe('SELECT 1');
	$resolver->query('SELECT 1') if $ending ne 'terminated';
	$_->quit for $gate, $pusher;
	$resolver->quit if $ending ne 'terminated';
	$node->safe_psql('postgres', "SELECT injection_points_detach('doltgres-insert-resolved')");
	my $rows = $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('$name', 'SELECT ctid::text || '' '' || k FROM $name ORDER BY ctid')");
	my $wanted =
	  $ending eq 'finishes'
	  ? "(0,1) q\n(0,2) p1\n(0,3) p2\n(0,4) p3\n(0,5) r\n(0,6) p4"
	  : "(0,1) q\n(0,2) p1\n(0,3) p2\n(0,4) p3\n(0,5) p4";
	my $case =
	  $ending eq 'finishes'
	  ? "a resolve held back by its stripe's start-overs finishes, its pusher asleep on the stripe"
	  : "a resolve $ending while its stripe's hold-back flag is set lets its sleeper write";
	ok($slept && $rows eq $wanted, $case) or diag($rows);
}

# (4b)'s heap record of the command ids of a row a change ends
# (log_heap_new_cid): under wal_level logical, updates and a delete of a
# user_catalog_table's rows, one of them the transaction's own row, log heap's
# NEW_CID records, with the combo id where heap gives one.
$node->append_conf('postgresql.conf', 'wal_level = logical');
$node->restart;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pg_walinspect;
CREATE TABLE cids_source AS SELECT k, 'v' || k AS v FROM generate_series(1, 2) k;
CREATE TABLE cids (k int4 PRIMARY KEY, v text) WITH (user_catalog_table = true);
CREATE TABLE heap_cids (k int4 PRIMARY KEY, v text) WITH (user_catalog_table = true);
INSERT INTO heap_cids SELECT * FROM cids_source;
SELECT doltgres_test_insert('cids', 'cids_source', false, 0);
});
my %cids;
for my $table ('cids', 'heap_cids')
{
	my @statements = (
		"INSERT INTO % VALUES (3, 'v3')",
		"UPDATE % SET v = 'x' WHERE k = 1",
		'DELETE FROM % WHERE k = 3',
		'UPDATE % SET k = 20 WHERE k = 2');
	my $writes = join("\n", map { written($table, $_) . ';' } @statements);
	$cids{$table} = $node->safe_psql(
		'postgres', qq{
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT pg_current_wal_insert_lsn() AS before \\gset
BEGIN;
$writes
COMMIT;
SELECT replace(description, '/' || pg_relation_filenode('$table') || ',', '/written,')
  FROM pg_get_wal_records_info(:'before', pg_current_wal_flush_lsn())
  WHERE resource_manager = 'Heap2' AND record_type = 'NEW_CID';
});
}
is($cids{cids}, $cids{heap_cids},
	"a user_catalog_table's updates and delete log heap's NEW_CID records of the rows they end");
cmp_ok(scalar(split /\n/, $cids{heap_cids}), '>=', 4, "heap logs a NEW_CID record of each row the changes write or end");

# (4b)'s combo ids taken from the backend's one combo table in heap's order,
# interleaved with a heap table's in the same transaction: a fork table and a
# heap table written in turn show the cmin and cmax two heap tables written in
# the same turns show.
$node->safe_psql('postgres',
	'CREATE TABLE combined (k int4 PRIMARY KEY, v text); CREATE TABLE heap_combined (LIKE combined INCLUDING ALL); '
	  . 'CREATE TABLE other_combined (LIKE combined INCLUDING ALL); CREATE TABLE heap_other_combined (LIKE combined INCLUDING ALL)'
);
my %combined;
for my $pair ([ 'combined', 'other_combined' ], [ 'heap_combined', 'heap_other_combined' ])
{
	my ($first, $second) = @$pair;
	my @steps = (
		[ $first, "INSERT INTO % VALUES (1, 'a'), (2, 'b')" ],
		[ $second, "INSERT INTO % VALUES (1, 'a'), (2, 'b')" ],
		[ $first, "UPDATE % SET v = 'c' WHERE k = 1" ],
		[ $second, "UPDATE % SET v = 'c' WHERE k = 2" ],
		[ $first, 'DELETE FROM % WHERE k = 2' ],
		[ $second, "UPDATE % SET v = 'd' WHERE k = 2" ]);
	my $writes = join("\n", map { written($_->[0], $_->[1]) . ';' } @steps);
	my $out = $node->safe_psql(
		'postgres', qq{
SET enable_indexscan = off;
SET enable_bitmapscan = off;
BEGIN;
$writes
@{[ reading($first, 1) ]};
@{[ reading($second, 1) ]};
COMMIT;
});
	$combined{$first} = $out =~ s/(heap_|other_)//gr;
}
is($combined{combined}, $combined{heap_combined},
	"a fork table's combo ids interleave with a heap table's in one transaction as two heap tables' do");

# (4b)'s update's block: the replaced row's block while it has room for the
# new row, else the block an insert takes, as heap's RelationGetBufferForTuple
# prefers the old tuple's page. A snapshot held across the updates keeps heap
# from pruning their old rows, whose space the fork frees only at the fold.
made('placed', '(k int4 PRIMARY KEY, v text)',
	"SELECT k, repeat('x', 1000) AS v FROM generate_series(1, 14) k");
both(
	'placed',
	"an update's new row goes on the replaced row's block when it has room and on an insert's block when it hasn't",
	[ 1, 'BEGIN ISOLATION LEVEL REPEATABLE READ' ],
	[ 1, 'SELECT 1' ],
	[ 0, "UPDATE % SET v = repeat('y', 1000) WHERE k = 1" ],
	[ 0, "UPDATE % SET v = repeat('z', 700) WHERE k = 9" ],
	[ 0, "UPDATE % SET v = repeat('w', 1000) WHERE k = 3" ],
	[ 1, 'COMMIT' ],
	[ 0, 'read' ]);

# Bugs row 76: an insert prefers the block its own backend's last insert into
# the relation took, heap's target block, kept per backend: one backend fills
# block 0 nearly, another's wide row goes to block 1, and the first backend's
# small row then goes back on block 0, as heap's does.
made('targeted', '(k int4 PRIMARY KEY, v text)', "SELECT 0 AS k, 'v' AS v WHERE false");
both(
	'targeted',
	"each backend's insert prefers the block its own last insert took, as heap's target block",
	[ 1, "INSERT INTO % SELECT k, (SELECT string_agg(md5((k * 100 + i)::text), '') FROM generate_series(1, 31) i) FROM generate_series(1, 7) k" ],
	[ 0, "INSERT INTO % VALUES (100, (SELECT string_agg(md5(i::text), '') FROM generate_series(1, 62) i))" ],
	[ 1, "INSERT INTO % VALUES (200, 'small')" ],
	[ 0, 'read' ]);

# Bugs row 77: an update keeps its new row on the replaced row's block whenever
# the row fits there, the fillfactor's reserve aside, as heap_update does:
# fillfactor 50 holds three 1000-byte rows a page, and an update of a row on
# block 0 goes to (0,4), as heap's does.
made('filled', '(k int4 PRIMARY KEY, v text) WITH (fillfactor = 50)',
	"SELECT k, repeat('x', 1000) AS v FROM generate_series(1, 6) k");
both(
	'filled',
	"an update keeps its row on the replaced row's block when it fits there, the fillfactor's reserve aside",
	[ 0, "UPDATE % SET v = repeat('y', 1000) WHERE k = 2" ],
	[ 0, 'read' ]);

# Bugs row 86: an insert of a compared key whose first target is refused takes
# its next target in the same round. One session inserts a row, which leaves its
# block as the relation's target block, then another fills that block until the
# fresh page moves past it, so the first session's next insert tries the full
# block first and is refused there. Its refused turn pushed nothing under the
# storage's stripe, so the stripe's push count is as its resolve read it, and the
# fresh page takes the row within the statement's time.
$node->safe_psql('postgres', 'CREATE TABLE refused_target (k citext PRIMARY KEY, v text STORAGE PLAIN)');
my $first = $node->background_psql('postgres');
$first->query_safe(written('refused_target', q{INSERT INTO % VALUES ('a', repeat('x', 1300))}));
$node->safe_psql('postgres',
	written('refused_target', q{INSERT INTO % SELECT 'f' || n, repeat('x', 1300) FROM generate_series(1, 12) n}));
my ($inserted) = $first->query(
	"SET statement_timeout = '30s'; " . written('refused_target', q{INSERT INTO % VALUES ('b', repeat('x', 1300))}));
is($inserted, '1', "an insert of a compared key whose target block is refused takes the fresh page");
$first->quit;
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('refused_target', 'SELECT count(*) FROM refused_target')"),
	'14',
	'the refused insert leaves every row once');

# Bugs row 141: an update toasts as heap's does when its row or the row it
# replaces holds a value out of line, not only past the threshold: at a
# toast_tuple_target of 128, a 300-byte value set beside a kept 3,000-byte one
# moves out of line, and so does one set in a row whose old version held values
# out of line and whose new one holds none.
$node->safe_psql(
	'postgres', q{
CREATE TABLE low_source (k int4, a text STORAGE EXTERNAL, b text STORAGE EXTERNAL);
INSERT INTO low_source VALUES (1, repeat('a', 3000), repeat('b', 50));
CREATE TABLE low (k int4 PRIMARY KEY, a text STORAGE EXTERNAL, b text STORAGE EXTERNAL) WITH (toast_tuple_target = 128);
CREATE TABLE heap_low (LIKE low INCLUDING ALL) WITH (toast_tuple_target = 128);
INSERT INTO heap_low SELECT * FROM low_source;
SELECT doltgres_test_insert('low', 'low_source', false, 0);
});
my %low;
for my $table ('low', 'heap_low')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $read =
	  "SELECT (pg_column_toast_chunk_id(a) IS NOT NULL) || ' ' || (pg_column_toast_chunk_id(b) IS NOT NULL) FROM $table";
	my $outside = sub {
		$node->safe_psql('postgres',
			$fork eq 'true'
			? "SELECT doltgres_test_standing('$table', '"
			  . ($read =~ s/'/''/gr) . "')"
			: $read);
	};
	for my $set (q{b = repeat('z', 300)}, q{a = 'x', b = repeat('y', 300)})
	{
		$node->safe_psql('postgres',
			"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT doltgres_test_write('$table', $fork, 'UPDATE $table SET "
			  . ($set =~ s/'/''/gr)
			  . "')");
		push @{ $low{$table} }, $outside->();
	}
}
is($low{low}[0], $low{heap_low}[0],
	"an update beside a kept value toasts the new one to the table's target as heap does"
);
is($low{low}[1], $low{heap_low}[1],
	'an update of a row whose old version held a value out of line toasts the new one as heap does'
);
is( join(',', @{ $low{low} }),
	'true true,false true',
	"the fork's updated rows hold heap's values out of line");

# Bugs row 134 (orev-30 Z1): one statement changes a row through its row's
# slice of its key's chain, then in a subtransaction twice, the second change
# finding the first as its own write, and the subtransaction rolls back; the
# statement's next change passes over the rolled-back writes, as the whole
# chain's read does, for a keyed and a keyless table, as on heap's twin.
my $subrolled =
  q{DO $$ BEGIN UPDATE % SET v = v || '1' WHERE k = 1; BEGIN UPDATE % SET v = v || '2' WHERE k = 1; UPDATE % SET v = v || '3' WHERE k = 1; RAISE EXCEPTION 'undone'; EXCEPTION WHEN raise_exception THEN NULL; END; UPDATE % SET v = v || '4' WHERE k = 1; END $$};
made(
	'subrolled',
	'(k int4 PRIMARY KEY, v text)',
	"SELECT k, 'v' || k AS v FROM generate_series(1, 2) k");
both(
	'subrolled',
	"a statement's change after its rolled-back subtransaction's changes of the row reads past them",
	[ 0, $subrolled ],
	[ 0, 'read' ]);
made(
	'subrolled_copies',
	'(k int4, v text)',
	"SELECT 1 AS k, 'v' AS v FROM generate_series(1, 2)");
both(
	'subrolled_copies',
	"a keyless table's change after its rolled-back subtransaction's changes of its copies reads past them",
	[ 0, $subrolled ],
	[ 0, 'read' ]);

# A fold that installs a base holding the row an update changes and drops the
# row's versions from the delta, between the update's read of the table's base
# and its read of the row's key and versions, leaves the row in the new base, where the
# update finds and changes it, rather than calling it invisible (the switch's
# partial-index blocker).
made('refolded', '(id int4, v int4)', 'SELECT g AS id, 0 AS v FROM generate_series(1, 20) g');
{
	my $updater = $node->background_psql('postgres', on_error_stop => 0);
	$updater->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-update-reading', 'wait')");
	$updater->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_write('refolded', true, 'UPDATE refolded SET v = 1 WHERE id = 9');\n");
	$node->wait_for_event('client backend', 'doltgres-update-reading');
	is($node->safe_psql('postgres', "SELECT doltgres_test_fold('refolded')"),
		't', "a fold applies the rows between the update's reads");
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('doltgres-update-reading')");
	is(step($updater, 'SELECT 1'), "1\n1", 'the update changed one row with no error');
	$updater->quit;
	is( $node->safe_psql(
			'postgres', "SELECT doltgres_test_standing('refolded', 'SELECT sum(v) FROM refolded WHERE id = 9')"),
		'1',
		"the update changed the row the fold moved into the base");
}

# A row lock that follows a committed update's chain passes over the versions
# its own transaction updated, as SnapshotDirty takes them as dead, to the
# newest: an update whose CTE updates the row after waiting on another's
# update, and whose function updates it again in a later command, finds that
# version past its command and refuses as eval-plan-qual's updwctefail does.
made('own_chain', '(accountid text PRIMARY KEY, balance numeric NOT NULL)',
	"SELECT * FROM (VALUES ('checking', 600::numeric), ('savings', 600::numeric)) AS v(accountid, balance)");
for my $table ('own_chain', 'heap_own_chain')
{
	$node->safe_psql('postgres',
		"CREATE FUNCTION ${table}_checking(int) RETURNS bool LANGUAGE sql AS \$\$ UPDATE $table SET balance = balance + 1 WHERE accountid = 'checking'; SELECT true; \$\$"
	);
}
both(
	'own_chain',
	"a lock following another's committed update passes its own transaction's updates and refuses a version past its command",
	[ 0, 'BEGIN' ],
	[ 0, "UPDATE % SET balance = balance - 200 WHERE accountid = 'checking'" ],
	[
		1,
		"start WITH doup AS (UPDATE % SET balance = balance + 1100 WHERE accountid = 'checking' RETURNING *, %_checking(999)) UPDATE % a SET balance = doup.balance + 100 FROM doup"
	],
	[ 0, 'blocked' ],
	[ 0, 'COMMIT' ],
	[ 1, 'finish' ],
	[ 0, 'read' ]);

# A row a committed update moved to another partition is refused, not passed
# over, by an update that waited on it, as heap's lock refuses a tuple its
# delete marked moved (eval-plan-qual's complexpartupdate_route_err1).
my %part_moved;
for my $table ('part_moved', 'heap_part_moved')
{
	my $using = $table eq 'part_moved' ? ' USING heap' : '';
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $table (a int4, b int4, c int4) PARTITION BY LIST (a)$using;
CREATE TABLE ${table}_1 PARTITION OF $table FOR VALUES IN (1);
CREATE TABLE ${table}_2 PARTITION OF $table FOR VALUES IN (2);
INSERT INTO $table VALUES (1, 1, 1), (2, 2, 2);
});
	my $mover = $node->background_psql('postgres');
	my $waiter = $node->background_psql('postgres', on_error_stop => 0);
	$waiter->query_safe('\set VERBOSITY verbose');
	$mover->query_safe("BEGIN; UPDATE $table SET a = 2 WHERE c = 1");
	$waiter->query_until(qr/started/, "\\echo started\nUPDATE $table SET b = 3 WHERE c = 1;\n");
	$node->poll_query_until('postgres', 'SELECT count(*) > 0 FROM pg_locks WHERE NOT granted')
	  or die 'no session waited';
	$mover->query_safe('COMMIT');
	$part_moved{$table} = [
		step($waiter, 'SELECT 1'),
		$node->safe_psql('postgres', "SELECT tableoid::regclass, a, b, c FROM $table ORDER BY c") =~ s/heap_//gr
	];
	$_->quit for $mover, $waiter;
}
is_deeply($part_moved{part_moved}, $part_moved{heap_part_moved},
	"an update waiting on a row another's update moved to another partition is refused as heap refuses it");
like($part_moved{part_moved}[0], qr/ERROR:  40001: tuple to be locked was already moved to another partition due to concurrent update/,
	"and the refusal is the reference's");

done_testing();
