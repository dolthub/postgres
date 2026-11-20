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

# The delta's drain (bugs row 74, §III.3's spill of a running transaction's own
# versions): a statement whose own versions pass delta_memory completes at the
# default setting, its rows read back by its own session and by another, and
# its spill survives a crash after its commit, with or without a checkpoint
# between, and is gone after its abort.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('spill');
$node->init;
$node->append_conf('postgresql.conf',
	"max_prepared_transactions = 2\nfold_threshold = 1GB\nfold_timeout = 3600s"
);
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION doltgres_test;
CREATE TABLE source_wide (v text);
INSERT INTO source_wide SELECT repeat(md5(i::text), 250) FROM generate_series(1, 4200) i;
CREATE TABLE source_some (v text);
INSERT INTO source_some SELECT repeat(md5(i::text), 31) FROM generate_series(1, 6000) i;
});

# counted returns the count of a fork table's rows a scan reads.
sub counted
{
	my ($table) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM doltgres_test_scan('$table', false, -1)");
}

# folded folds tables, so the delta lets go of their committed versions and
# their spills (the fold's drop). The fold launcher is held off by
# fold_threshold, since these cases test the drain, not folds under load, so
# each case's tables are folded here once the case is done, so the later cases
# fit in the default delta_memory.
sub folded
{
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$_')") for @_;
}

# A statement inserting 4,200 rows of 8,000 bytes stored in line, past the
# default 32 MB of delta_memory in its own versions alone, completes by
# spilling them, and its rows read back. The rows are wide and few because an
# insert's cost per row grows with the memtable (bugs row 83), whose fix lets
# this case go back to narrow rows.
$node->safe_psql(
	'postgres', q{
CREATE TABLE wide (v text STORAGE PLAIN);
SELECT count(*) FROM doltgres_test_insert('wide', 'source_wide', true, 0);
});
is(counted('wide'), '4200',
	'a statement writing more than delta_memory completes at the default setting'
);

# Another session reads the committed spill's rows before any fold.
my $other = $node->background_psql('postgres');
is( $other->query_safe(
		q{SELECT count(*) FROM doltgres_test_scan('wide', false, -1)}),
	'4200',
	"another session reads a committed transaction's spilled rows");
$other->quit;

# A spill, a commit, and a crash with no checkpoint since: every row is back,
# through redo's row changes. The 6,000 rows of 992 bytes pass a spill's
# threshold, an eighth of delta_memory, and so spill.
$node->safe_psql(
	'postgres', q{
CHECKPOINT;
CREATE TABLE crashed (v text);
SELECT count(*) FROM doltgres_test_insert('crashed', 'source_some', true, 0);
});
$node->stop('immediate');
$node->start;
is(counted('crashed'), '6000',
	'a committed spill with no checkpoint since is back after a crash');
folded('crashed');

# D1: a spill, a checkpoint whose redo point passes the spilled rows' records,
# a commit, and a crash before the next checkpoint: every row is back, the
# checkpoint having named the running transaction's spill.
$node->safe_psql('postgres', 'CREATE TABLE named (v text)');
my $writer = $node->background_psql('postgres');
$writer->query_safe('BEGIN');
$writer->query_safe(
	q{SELECT count(*) FROM doltgres_test_insert('named', 'source_some', true, 0)}
);
$node->safe_psql('postgres', 'CHECKPOINT');
$writer->query_safe('COMMIT');
$writer->quit;
$node->stop('immediate');
$node->start;
is(counted('named'), '6000',
	"a spill a checkpoint named, committed before a crash, is back after it");
folded('named');

# D1's abort twin: a spill, a checkpoint, an abort, and a crash: no row comes
# back.
$node->safe_psql('postgres', 'CREATE TABLE rolled (v text)');
$writer = $node->background_psql('postgres');
$writer->query_safe('BEGIN');
$writer->query_safe(
	q{SELECT count(*) FROM doltgres_test_insert('rolled', 'source_some', true, 0)}
);
$node->safe_psql('postgres', 'CHECKPOINT');
$writer->query_safe('ROLLBACK');
$writer->quit;
$node->stop('immediate');
$node->start;
is(counted('rolled'), '0',
	'a spill a checkpoint named, rolled back before a crash, brings no row back'
);
folded('rolled');

# Bugs row 89: a checkpoint while the transaction of the backend in the proc
# array's first slot holds a spill. The checkpointer, outside the proc array,
# keeps that slot's offset as its own, which Postgres' in-progress check skips;
# the checkpoint still names the spill of the running transaction, whose rows
# read back after its commit and after a crash. The restart makes the writer
# the first backend, so its proc takes the first slot, and a later transaction
# commits before the checkpoint, so the check of the writer's xid reads the
# proc array rather than answering from the newest completed xid.
$node->safe_psql('postgres', 'CREATE TABLE first_slot (v text)');
$node->restart;
$writer = $node->background_psql('postgres');
$writer->query_safe('BEGIN');
$writer->query_safe(
	q{SELECT count(*) FROM doltgres_test_insert('first_slot', 'source_some', true, 0)}
);
$node->safe_psql('postgres', 'SELECT pg_current_xact_id()');
$node->safe_psql('postgres', 'CHECKPOINT');
$writer->query_safe('COMMIT');
$writer->quit;
is(counted('first_slot'), '6000',
	"a checkpoint names the spill of the first proc array slot's running transaction"
);
$node->stop('immediate');
$node->start;
is(counted('first_slot'), '6000',
	"the spill of the first slot's transaction is back after a crash");
folded('first_slot');

# keyed returns the statement that writes a keyed fork table through the fork's
# access method.
sub keyed
{
	my ($table, $sql) = @_;
	$sql =~ s/'/''/g;
	return "SELECT doltgres_test_write('$table', true, '$sql')";
}

# spill_files returns the count of the spill files of a transaction's xid in
# the postgres database's repository.
sub spill_files
{
	my ($xid) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_ls_dir('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = 'postgres') || '/delta/spill', true, false) f WHERE f LIKE '%-$xid-%'"
	);
}

# Another writer's insert of a key a running transaction spilled waits for it,
# and is refused with 23505 once it commits, as heap's btree answers.
$node->safe_psql('postgres',
	'CREATE TABLE conflicted (k int4 PRIMARY KEY, v text)');
$writer = $node->background_psql('postgres');
$writer->query_safe('BEGIN');
$writer->query_safe(
	keyed(
		'conflicted',
		"INSERT INTO conflicted SELECT i, repeat(md5(i::text), 31) FROM generate_series(1, 6000) i"
	));
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	"SET lock_timeout = 500; "
	  . keyed('conflicted', "INSERT INTO conflicted VALUES (1, 'x')"),
	extra_params => [ '-v', 'VERBOSITY=sqlstate' ]);
like(
	$stderr,
	qr/ERROR:  55P03/,
	"another writer's insert of a spilled running key waits for its transaction"
);
$writer->query_safe('COMMIT');
$writer->quit;
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	keyed('conflicted', "INSERT INTO conflicted VALUES (1, 'x')"),
	extra_params => [ '-v', 'VERBOSITY=sqlstate' ]);
like(
	$stderr,
	qr/ERROR:  23505/,
	"an insert of a committed spilled key is refused with 23505");
folded('conflicted');

# A spill, a savepoint rolled back over a second spill, and a commit: the rows
# the savepoint wrote stay unseen.
$node->safe_psql('postgres',
	'CREATE TABLE saved (k int4 PRIMARY KEY, v text)');
$node->safe_psql(
	'postgres',
	join(
		"\n", 'BEGIN;',
		keyed('saved',
			"INSERT INTO saved SELECT i, repeat(md5(i::text), 31) FROM generate_series(1, 6000) i"
		  )
		  . ';',
		'SAVEPOINT s;',
		keyed(
			'saved',
			"INSERT INTO saved SELECT i, repeat(md5(i::text), 31) FROM generate_series(6001, 12000) i"
		  )
		  . ';',
		'ROLLBACK TO s;',
		'COMMIT;'));
is(counted('saved'), '6000',
	"a savepoint's spilled rows rolled back stay unseen after the commit");
folded('saved');

# A fetch by ctid reads a committed spilled row, through its spilled tuple id.
isnt(
	$node->safe_psql(
		'postgres', q{SELECT doltgres_test_fetch('wide', '(7,1)')}),
	'',
	'a fetch by ctid reads a spilled row');
folded('wide');

# An aborted transaction's spill files go at the second checkpoint after its
# abort, which neither generation of the manifest names them in, the first
# listing them for the readers begun before it (bugs row 124's M3), and a crash
# between the abort and a checkpoint leaves none once the start's checkpoint
# listed them and a later one ran.
$node->safe_psql('postgres', 'CREATE TABLE swept (v text)');
my $aborted = $node->safe_psql(
	'postgres', q{
BEGIN;
SELECT count(*) FROM doltgres_test_insert('swept', 'source_some', true, 0) \gset
SELECT pg_current_xact_id();
ROLLBACK;
});
ok(spill_files($aborted) > 0,
	"an aborted transaction's spill files stay until a checkpoint");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'CHECKPOINT');
is(spill_files($aborted), '0',
	"an aborted transaction's spill files go at the second checkpoint after it"
);
my $crashed = $node->safe_psql(
	'postgres', q{
BEGIN;
SELECT count(*) FROM doltgres_test_insert('swept', 'source_some', true, 0) \gset
SELECT pg_current_xact_id();
ROLLBACK;
});
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', 'CHECKPOINT');
is(spill_files($crashed), '0',
	"a crash between an abort and a checkpoint leaves none of its spill files once a checkpoint after the start ran"
);
folded('swept');

# T1 spills its keys and commits, then T2 changes every one of them and spills
# them in turn: T2's file, linked later, reads as newer, and after a
# checkpoint and a crash the loader links the files in the same order.
$node->safe_psql('postgres',
	'CREATE TABLE ordered (k int4 PRIMARY KEY, v text)');
$node->safe_psql(
	'postgres',
	keyed(
		'ordered',
		"INSERT INTO ordered SELECT i, repeat(md5(i::text), 31) FROM generate_series(1, 6000) i"
	));
$node->safe_psql('postgres',
	keyed('ordered', "UPDATE ordered SET v = 'new' || repeat('x', 980) || k")
);
my $newer =
  q{SELECT count(*) FROM doltgres_test_scan('ordered', false, -1) r WHERE r LIKE '%new%'};
is($node->safe_psql('postgres', $newer),
	'6000', "a later transaction's spill of the same keys reads as newer");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
is( $node->safe_psql('postgres', $newer),
	'6000',
	"after a crash the loader links two transactions' spills of the same keys in their order"
);
folded('ordered');

# spilled_in_savepoint runs a transaction that writes 3,000 rows, then, in a
# savepoint, 2,000 more, whose write spills both and fails at an injection
# point, then rolls the savepoint back, lets another session's insert of one
# of the first rows' keys wait for it, and commits, answering the other
# session's error and the table's count.
sub spilled_in_savepoint
{
	my ($table, $point) = @_;
	$node->safe_psql('postgres',
		"CREATE TABLE $table (k int4 PRIMARY KEY, v text)");
	my $session = $node->background_psql('postgres', on_error_stop => 0);
	$session->query_safe('BEGIN');
	$session->query_safe(
		keyed(
			$table,
			"INSERT INTO $table SELECT i, repeat(md5(i::text), 31) FROM generate_series(1, 3000) i"
		));
	$session->query_safe('SAVEPOINT s');
	$session->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('$point', 'error')"
	);
	$session->query(
		keyed(
			$table,
			"INSERT INTO $table SELECT i, repeat(md5(i::text), 31) FROM generate_series(3001, 5000) i"
		));
	$session->{stderr} = '';
	$session->query_safe('ROLLBACK TO s');
	$session->query_safe("SELECT injection_points_detach('$point')");
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		"SET lock_timeout = 500; "
		  . keyed($table, "INSERT INTO $table VALUES (1, 'x')"),
		extra_params => [ '-v', 'VERBOSITY=sqlstate' ]);
	$session->query_safe('COMMIT');
	$session->quit;
	return ($stderr, counted($table));
}

# A spill whose write fails between its slot and the end of its removal, in a
# savepoint: the versions it took out stay in the memtable too, another
# session's conflict check still finds the key, and the commit's link of the
# pending file takes the memtable's twins out, so each row reads once.
my ($waited, $count) =
  spilled_in_savepoint('pended', 'doltgres-spill-pended');
like(
	$waited,
	qr/ERROR:  55P03/,
	"a spill failed after its slot leaves its key to another writer's conflict check"
);
is($count, '3000',
	"the commit links a spill that failed after its slot, its memtable twins taken out"
);
folded('pended');

# A spill whose filter's link fails in a savepoint: its slot keeps its file,
# another session's conflict check finds the spilled key through it, and the
# commit links it.
($waited, $count) = spilled_in_savepoint('linking', 'doltgres-spill-linking');
like(
	$waited,
	qr/ERROR:  55P03/,
	"a spill whose link failed in a savepoint keeps its key seen through its slot"
);
is($count, '3000',
	"the commit links a spill whose link failed in a savepoint");
folded('linking');

# A prepared transaction's spill: a checkpoint names it, it survives a restart
# while prepared, and its rows appear at COMMIT PREPARED, or none at ROLLBACK
# PREPARED. After the crash the prepared transaction holds the proc array's
# first slot, which the end of recovery's checkpoint reads too (bugs row 89).
for my $end ('COMMIT', 'ROLLBACK')
{
	my $table = lc($end) . '_prepared';
	$node->safe_psql('postgres', "CREATE TABLE $table (v text)");
	$node->safe_psql(
		'postgres', qq{
BEGIN;
SELECT count(*) FROM doltgres_test_insert('$table', 'source_some', true, 0);
PREPARE TRANSACTION '$table';
CHECKPOINT;
});
	$node->stop('immediate');
	$node->start;
	$node->safe_psql('postgres', "$end PREPARED '$table'");
	is( counted($table),
		$end eq 'COMMIT' ? '6000' : '0',
		"a prepared transaction's spill survives a restart and ends at $end PREPARED"
	);
}

# Run kind 9, spilled entries: one transaction writes a btree index's entries
# past the spill threshold, so the index's storage spills its own versions as
# an entries spill. After the commit and a crash, the start links the spill
# back, and the index's storage reads every entry, its spilled ones included.
$node->safe_psql('postgres',
	'CREATE TABLE entried (k int4 PRIMARY KEY, name text); CREATE INDEX entried_name ON entried (name)'
);
$node->safe_psql(
	'postgres',
	"SELECT doltgres_test_index_routine('entried_pkey'); SELECT doltgres_test_index_routine('entried_name'); "
	  . keyed(
		'entried',
		"INSERT INTO entried SELECT i, repeat(md5(i::text), 31) FROM generate_series(1, 6000) i"
	  ));
my $entries_spills =
  "SELECT count(*) FROM pg_ls_dir('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = 'postgres') || '/delta/spill', true, false) f WHERE f LIKE 'entries-%'";
cmp_ok($node->safe_psql('postgres', $entries_spills),
	'>=', 1,
	"an index's entries past the threshold spill as an entries spill");
$node->stop('immediate');
$node->start;
is( scalar(
		() =
		  $node->safe_psql('postgres',
			"SELECT doltgres_test_index_entries('entried_name')") =~ /^put$/mg
	),
	6000,
	"after a crash the index's storage reads every entry of its entries spill"
);

# A prepared transaction holds no unspilled versions, since no process
# publishes them once it is prepared: its prepare spills every tree it wrote
# to, under any threshold, before the transaction leaves the backend (bugs row
# 144), and COMMIT PREPARED reads its rows back.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_few (v text);
INSERT INTO source_few SELECT repeat(md5(i::text), 3) FROM generate_series(1, 100) i;
CREATE TABLE prepared_rows (v text);
});
my $prepared = $node->safe_psql(
	'postgres', q{
BEGIN;
SELECT count(*) FROM doltgres_test_insert('prepared_rows', 'source_few', true, 0) \gset
SELECT pg_current_xact_id();
PREPARE TRANSACTION 'spilled';
});
cmp_ok(spill_files($prepared), '>', 0,
	"a prepare spills its transaction's versions under any threshold");
$node->safe_psql('postgres', "COMMIT PREPARED 'spilled'");
is(counted('prepared_rows'), '100',
	"a prepared transaction's spilled rows read back after its commit");

# A session idle in a transaction after BEGIN and an INSERT runs no code until
# its next command, so its versions are spilled by another process: a backend
# with nothing of its own to spill relieves the budget by spilling the idle
# transaction's versions (bugs row 144), and the idle session's rows read back
# after its commit.
$node->safe_psql('postgres', 'CREATE TABLE idle_rows (v text)');
my $idle = $node->background_psql('postgres');
$idle->query_safe('BEGIN');
$idle->query_safe(
	q{SELECT count(*) FROM doltgres_test_insert('idle_rows', 'source_few', true, 0)}
);
my $idle_xid = $idle->query_safe('SELECT pg_current_xact_id()');
is(spill_files($idle_xid), '0',
	"an idle transaction under every threshold has spilled nothing");
is( $node->safe_psql('postgres', 'SELECT doltgres_test_delta(61, 0, 0)'),
	'1',
	"a backend with nothing of its own spills the idle transaction's versions"
);
cmp_ok(spill_files($idle_xid), '>', 0,
	"the idle transaction's versions are in its spill files");
$idle->query_safe('COMMIT');
$idle->quit;
is(counted('idle_rows'), '100',
	"the idle transaction's spilled rows read back after its commit");

# A statement inserting 4,200 rows whose 8,000-byte values move out of line,
# about 33 MB of their blobs' chunks past the default 32 MB of delta_memory,
# completes: the chunks spill with the transaction's other versions
# (STORAGE_DESIGN §II.9), so the writer doesn't wait at the budget forever,
# and every value reads back. The case runs in a database of its own, its
# source folded, so its chunk spill's removal waits on no other storage's
# unfolded rows.
$node->safe_psql('postgres', 'CREATE DATABASE chunked');
$node->safe_psql(
	'chunked', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE source_wide (v text);
INSERT INTO source_wide SELECT repeat(md5(i::text), 250) FROM generate_series(1, 4200) i;
CREATE TABLE wide_out (v text);
});
$node->safe_psql('chunked',
	q{SELECT doltgres_test_fold((SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'source_wide'::regclass)), doltgres_test_fold('source_wide')}
);
my $wide_out = $node->background_psql('chunked');
$wide_out->query_safe(q{SET statement_timeout = '300s'});
$wide_out->query_safe('BEGIN');
is( $wide_out->query_safe(
		q{SELECT count(*) FROM doltgres_test_insert('wide_out', 'source_wide', true, 0)}
	),
	'4200',
	"a statement whose out-of-line values pass delta_memory completes");
my $wide_xid = $wide_out->query_safe('SELECT pg_current_xact_id()');
$wide_out->query_safe('COMMIT');
$wide_out->quit;

# chunk_files returns the count of the chunk spill files of a transaction's
# xid in a database's repository.
sub chunk_files
{
	my ($database, $xid) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_ls_dir('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = '$database') || '/delta/spill', true, false) f WHERE f LIKE 'chunks-$xid-%'"
	);
}
cmp_ok(chunk_files('chunked', $wide_xid),
	'>', 0, "the transaction's chunks are in its chunk spill files");
my $digest = 'SELECT md5(string_agg(md5(v), NULL ORDER BY md5(v)))';
my $source_digest = $node->safe_psql('chunked', "$digest FROM source_wide");
my $wide_digest =
  "SELECT doltgres_test_standing('wide_out', '$digest FROM wide_out')";
is($node->safe_psql('chunked', $wide_digest),
	$source_digest, "the spilled chunks' values read back");

# A checkpoint names the committed transaction's chunk spill, and after a crash
# every value reads back from it before any fold.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
is($node->safe_psql('chunked', $wide_digest),
	$source_digest,
	"a committed transaction's spilled chunks read back after a crash");

# chunk_listed returns the count of the files of a transaction's xid a
# database's repository's chunk spills' manifest lists, none once the manifest
# lists no file and is removed.
sub chunk_listed
{
	my ($database, $xid) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM regexp_split_to_table(coalesce(pg_read_file('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = '$database') || '/delta/spill/spills-chunks', true), ''), E'\\n') line WHERE split_part(line, ' ', 1) = '$xid'"
	);
}

# A later transaction deletes the rows before any fold: the chunk spill stays
# while the rows' versions are unfolded, and the first checkpoint after a fold
# of the table and of its TOAST relation applied them takes it out of the
# manifest. Its files are unlinked later by the epoch rule a storage's ended
# spills follow, once every reader that may have listed them has ended, which a
# background process's published epoch may hold, so the case asks the manifest.
$node->safe_psql('chunked',
	"SELECT doltgres_test_write('wide_out', true, 'DELETE FROM wide_out')");
$node->safe_psql('postgres', 'CHECKPOINT') for 1 .. 2;
cmp_ok(chunk_files('chunked', $wide_xid),
	'>', 0, "a chunk spill stays while the rows naming its chunks are unfolded");
cmp_ok(chunk_listed('chunked', $wide_xid),
	'>', 0, "and stays listed in the manifest");
my $wide_toast =
  q{(SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'wide_out'::regclass)};
$node->safe_psql('chunked',
	"SELECT doltgres_test_fold($wide_toast), doltgres_test_fold('wide_out')");
cmp_ok(chunk_listed('chunked', $wide_xid),
	'>', 0, "the chunk spill is listed until a checkpoint after the fold");
$node->safe_psql('postgres', 'CHECKPOINT');
is(chunk_listed('chunked', $wide_xid),
	'0',
	"the chunk spill leaves the manifest once the fold applied every version of its transaction"
);

# An aborted transaction's chunk spill leaves the manifest at its abort.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_aborted (v text);
INSERT INTO source_aborted SELECT repeat(md5((-i)::text), 250) FROM generate_series(1, 4200) i;
CREATE TABLE aborted_out (v text);
});
my $aborter = $node->background_psql('postgres');
$aborter->query_safe('BEGIN');
$aborter->query_safe(
	q{SELECT count(*) FROM doltgres_test_insert('aborted_out', 'source_aborted', true, 0)}
);
my $aborted_xid = $aborter->query_safe('SELECT pg_current_xact_id()');
cmp_ok(chunk_files('postgres', $aborted_xid), '>', 0,
	"a running transaction's chunk spill files are there");
$aborter->query_safe('ROLLBACK');
$aborter->quit;
is(chunk_listed('postgres', $aborted_xid),
	'0', "an aborted transaction's chunk spill leaves the manifest at its abort");
is(counted('aborted_out'), '0', "the aborted transaction's rows are gone");

# Two transactions write the same out-of-line value: the second, while the
# first's version of each chunk is in the delta, pushes its own, since a row
# names no chunk another transaction's version holds; the first commits and a
# fold of its table and its TOAST relation applies its row and takes the chunks
# out of the delta, and the second, its row still unfolded, reads its value
# back before and after its commit.
$node->safe_psql(
	'postgres', q{
CREATE TABLE shared_source (v text);
INSERT INTO shared_source SELECT string_agg(md5(g::text), '') FROM generate_series(1, 400) g;
CREATE TABLE shared_first (v text);
CREATE TABLE shared_second (v text);
SELECT count(*) FROM doltgres_test_insert('shared_first', 'shared_source', true, 0);
});
my $second = $node->background_psql('postgres');
$second->query_safe('BEGIN');
$second->query_safe(
	q{SELECT count(*) FROM doltgres_test_insert('shared_second', 'shared_source', true, 0)}
);
my $first_toast =
  q{(SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'shared_first'::regclass)};
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_fold($first_toast), doltgres_test_fold('shared_first')"
	),
	't|t',
	"the folds apply the first transaction's row");
my $shared_digest = $node->safe_psql('postgres', 'SELECT md5(v) FROM shared_source');
my $second_read =
  q{SELECT doltgres_test_standing('shared_second', 'SELECT md5(v) FROM shared_second')};
is($second->query_safe($second_read),
	$shared_digest,
	"the second transaction reads its value after the first's row is folded");
$second->query_safe('COMMIT');
$second->quit;
is($node->safe_psql('postgres', $second_read),
	$shared_digest, "the second transaction's value reads back after its commit");

# Bugs row 246: on a node made for streaming, whose shared_buffers is 1MB, the
# test insert of a source table of more pages than the buffers completes, each
# row's copy holding no pin on its source page while the insert keeps it.
my $pinned = PostgreSQL::Test::Cluster->new('spill_pinned');
$pinned->init(allows_streaming => 1);
$pinned->start;
$pinned->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE pinned_source (v text);
INSERT INTO pinned_source SELECT 'r' || i FROM generate_series(1, 50000) i;
CREATE TABLE pinned (v text);
});
is( $pinned->safe_psql('postgres',
		q{SELECT count(*) FROM doltgres_test_insert('pinned', 'pinned_source', true, 0)}),
	'50000',
	'a test insert of a source past shared_buffers completes on a node made for streaming');
$pinned->stop;

# Bugs row 74 through the real commands: one INSERT ... SELECT whose own
# versions pass the default 32 MB of delta_memory completes, its running
# transaction spilling them (the delta's drain), and every row reads back.
my $wide_rows = q{SELECT count(*) || ' ' || md5(string_agg(md5(v), '' ORDER BY v)) FROM };
$node->safe_psql('postgres', 'CREATE TABLE wide_real (v text STORAGE PLAIN)');
$node->psql('postgres', 'INSERT INTO wide_real SELECT v FROM source_wide');
is( $node->safe_psql('postgres', "$wide_rows wide_real"),
	$node->safe_psql('postgres', "$wide_rows source_wide"),
	'an INSERT ... SELECT writing more than delta_memory completes at the default setting'
);
folded('source_wide', 'wide_real');

# Bugs row 144 through the real commands: with every table folded, so the
# earlier cases' versions leave the delta, four transactions each hold about
# 7.5 MB of unspilled versions, under a spill's threshold, while paused before
# their commits (waiting on an advisory lock) or idle in their transactions, so
# a fifth writer's INSERT finds the 32 MB of delta_memory full. The refused
# writer has the holders' versions spilled and completes while they stay paused
# or idle, and every holder's rows read back after its commit.
$node->safe_psql('postgres',
	"SELECT count(doltgres_test_fold(oid::regclass)) FROM pg_class WHERE oid >= 16384 AND relkind IN ('r', 't')");
for my $held ('paused', 'idle')
{
	my $locker = $node->background_psql('postgres');
	$locker->query_safe('SELECT pg_advisory_lock(144)');
	my (@holders, @xids);
	for my $n (1 .. 4)
	{
		my $table = "${held}_holder_$n";
		$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
		my $holder = $node->background_psql('postgres');
		$holder->query_safe('BEGIN');
		$holder->query_safe(
			"INSERT INTO $table SELECT g, repeat('h', 100) FROM generate_series(1, 18000) g"
		);
		push @xids, $holder->query_safe('SELECT pg_current_xact_id()');
		$holder->query_until(qr/started/,
			"\\echo started\nSELECT pg_advisory_xact_lock(144);\n")
		  if $held eq 'paused';
		push @holders, $holder;
	}
	is(join(',', map { spill_files($_) } @xids),
		'0,0,0,0', "$held holders under the threshold have spilled nothing");
	$node->safe_psql('postgres',
		"CREATE TABLE ${held}_writer (k int4, v text)");
	my ($code, $out, $err) = $node->psql('postgres',
		"SET statement_timeout = '300s'; INSERT INTO ${held}_writer SELECT g, repeat('w', 100) FROM generate_series(1, 20000) g"
	);
	is($code . $err, '0',
		"a writer refused at the budget completes while four holders stay $held"
	);
	ok((grep { spill_files($_) > 0 } @xids) > 0,
		"the writer has the $held holders' versions spilled");
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_stat_activity WHERE xact_start IS NOT NULL AND state = '"
			  . ($held eq 'paused' ? 'active' : 'idle in transaction')
			  . "' AND query LIKE '"
			  . ($held eq 'paused' ? 'SELECT pg_advisory_xact_lock%' : 'SELECT pg_current_xact_id()%')
			  . "'"),
		'4',
		"the holders are still $held");
	$locker->query_safe('SELECT pg_advisory_unlock(144)');
	$locker->quit;
	for my $holder (@holders)
	{
		$holder->query_safe('COMMIT');
		$holder->quit;
	}
	is( $node->safe_psql(
			'postgres',
			'SELECT ' . join(' + ', map { "(SELECT count(*) FROM ${held}_holder_$_)" } 1 .. 4)
			  . " + (SELECT count(*) FROM ${held}_writer)"),
		'92000',
		"the $held holders' rows and the writer's read back");
	folded((map { "${held}_holder_$_" } 1 .. 4), "${held}_writer");
}

# The budget wait text's cases of a statement whose fork scan stays open while
# it writes at a full budget: four sessions idle in their transactions hold
# about 30 MB of unspilled versions, and each statement writes about 8 MB more
# while its scan is open between fetches, finishing with no error and no hang,
# with the rows the same statements give over heap twins, and its holders'
# rows read back after their commits.
$node->safe_psql(
	'postgres', q{
CREATE TABLE open_rows (k int4 PRIMARY KEY, v text);
INSERT INTO open_rows SELECT g, repeat('o', 100) FROM generate_series(1, 20000) g;
CREATE TABLE heap_open_rows (k int4 PRIMARY KEY, v text) USING doltgres_twin;
INSERT INTO heap_open_rows SELECT * FROM open_rows;
CREATE FUNCTION copied() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('INSERT INTO %I VALUES ($1, $2)', TG_ARGV[0]) USING NEW.k, NEW.v;
	RETURN NULL;
END
$$;
});
folded('open_rows');

# held returns four sessions idle in their transactions, each holding about
# 7.5 MB of unspilled versions of a table of its own.
sub held
{
	my ($prefix) = @_;
	my @holders;
	for my $n (1 .. 4)
	{
		$node->safe_psql('postgres', "CREATE TABLE ${prefix}_held_$n (k int4, v text)");
		my $holder = $node->background_psql('postgres');
		$holder->query_safe('BEGIN');
		$holder->query_safe(
			"INSERT INTO ${prefix}_held_$n SELECT g, repeat('h', 100) FROM generate_series(1, 18000) g"
		);
		push @holders, $holder;
	}
	return @holders;
}

# opened runs a case's statements over fork tables while four holders fill the
# budget and over heap twins with none, and checks each finishes and reads
# back as heap's, then that the holders' rows read back after their commits.
# "%" in a statement stands for the tables' prefix, empty for the fork's and
# "heap_" for the twins'.
sub opened
{
	my ($case, $setup, $statements, $read) = @_;
	my %output;
	for my $prefix ('heap_', '')
	{
		(my $made = $setup) =~ s/%/$prefix/g;
		$made =~ s/\@AM/$prefix eq '' ? '' : 'USING doltgres_twin'/ge;
		$node->safe_psql('postgres', $made);
		my @holders = $prefix eq '' ? held($case) : ();
		(my $sql = $statements) =~ s/%/$prefix/g;
		my ($code, $out, $err) = $node->psql('postgres',
			"SET statement_timeout = '300s';\n$sql", on_error_stop => 1);
		(my $reading = $read) =~ s/%/$prefix/g;
		$output{$prefix} =
		  ($code ? "failed: $err" : '') . $node->safe_psql('postgres', $reading);
		for my $holder (@holders)
		{
			$holder->query_safe('COMMIT');
			$holder->quit;
		}
		is( $node->safe_psql(
				'postgres',
				'SELECT ' . join(' + ', map { "(SELECT count(*) FROM ${case}_held_$_)" } 1 .. 4)),
			'72000',
			"the holders' rows read back after the $case case")
		  if $prefix eq '';
	}
	is($output{''}, $output{heap_}, "the $case case at a full budget finishes as over heap twins");
}

opened(
	'inserted',
	'CREATE TABLE %inserted (k int4, v text) @AM',
	'INSERT INTO %inserted SELECT k, v FROM %open_rows',
	"SELECT count(*) || ' ' || md5(string_agg(k || v, ',' ORDER BY k)) FROM %inserted");
opened(
	'updated',
	'CREATE TABLE %updated (k int4 PRIMARY KEY, v text) @AM; INSERT INTO %updated SELECT k, v FROM %open_rows',
	"SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;\n"
	  . "UPDATE %updated u SET v = o.v || 'u' FROM %open_rows o WHERE u.k = o.k",
	"SELECT count(*) || ' ' || md5(string_agg(k || v, ',' ORDER BY k)) FROM %updated");
opened(
	'triggered',
	"CREATE TABLE %triggered (k int4, v text) \@AM; CREATE TABLE %triggering (k int4, v text) USING doltgres_twin;\n"
	  . "CREATE TRIGGER copies AFTER INSERT ON %triggering FOR EACH ROW EXECUTE FUNCTION copied('%triggered')",
	'INSERT INTO %triggering SELECT k, v FROM %open_rows',
	"SELECT count(*) || ' ' || md5(string_agg(k || v, ',' ORDER BY k)) FROM %triggered");
opened(
	'cursored',
	'CREATE TABLE %cursored (k int4, v text) @AM; CREATE TABLE %cursor_written (k int4, v text) @AM',
	"BEGIN;\nINSERT INTO %cursored SELECT g, repeat('c', 100) FROM generate_series(1, 5000) g;\n"
	  . "DECLARE c CURSOR FOR SELECT k FROM %cursored;\n"
	  . "CREATE TEMPORARY TABLE %fetched AS SELECT 0 AS k WITH NO DATA;\n"
	  . "DO \$\$ DECLARE r record; c refcursor := 'c'; BEGIN FOR i IN 1 .. 100 LOOP FETCH c INTO r; INSERT INTO %fetched VALUES (r.k); END LOOP; END \$\$;\n"
	  . "INSERT INTO %cursor_written SELECT g, repeat('w', 100) FROM generate_series(1, 20000) g;\n"
	  . "DO \$\$ DECLARE r record; c refcursor := 'c'; BEGIN LOOP FETCH c INTO r; EXIT WHEN NOT FOUND; INSERT INTO %fetched VALUES (r.k); END LOOP; END \$\$;\n"
	  . "CREATE TABLE %cursor_read AS SELECT * FROM %fetched;\nCOMMIT",
	"SELECT count(*) || ' ' || count(DISTINCT k) || ' ' || sum(k) FROM %cursor_read");

# tables makes count tables of a prefix on a node, a key and a text column
# each, so many to a transaction, and returns the first error, or none.
sub tables
{
	my ($on, $prefix, $count, $each) = @_;
	for (my $from = 1; $from <= $count; $from += $each)
	{
		my $to = $from + $each - 1 < $count ? $from + $each - 1 : $count;
		my ($code, $out, $err) = $on->psql('postgres',
			"SET statement_timeout = '60s'; DO \$\$ BEGIN FOR i IN $from .. $to LOOP EXECUTE format('CREATE TABLE ${prefix}_%s (k int4, v text)', i); END LOOP; END \$\$"
		);
		return $err if $code;
	}
	return '';
}

# written returns a DO block that inserts a row of some bytes into each of a
# prefix's tables, in one transaction.
sub written
{
	my ($prefix, $count, $bytes) = @_;
	return
	  "DO \$\$ BEGIN FOR i IN 1 .. $count LOOP EXECUTE format('INSERT INTO ${prefix}_%s VALUES (%s, repeat(''r'', $bytes))', i, i); END LOOP; END \$\$";
}

# Bugs row 150 through the real commands: at the default 32 MB of
# delta_memory, one transaction writes a row into each of 5,000 tables, more
# small storages than 8 KB slots allow, which the default lock table lets it
# lock, and completes, every row read back. The tables are made a thousand to
# a transaction, as that lock table allows.
my $many = PostgreSQL::Test::Cluster->new('many');
$many->init;
$many->append_conf('postgresql.conf',
	"max_connections = 100\nfold_threshold = 1GB\nfold_timeout = 3600s");
$many->start;
tables($many, 'small', 5000, 1000);
my ($many_code, $many_out, $many_err) = $many->psql('postgres',
	"SET statement_timeout = '600s'; " . written('small', 5000, 100));
is($many_code . $many_err, '0',
	'one transaction writing a row into more small storages than 8 KB slots allow at 32 MB completes'
);
is( $many->safe_psql(
		'postgres',
		"SELECT sum((xpath('/row/c/text()', query_to_xml(format('SELECT count(*) AS c FROM %I', relname), false, true, '')))[1]::text::int8) FROM pg_class WHERE relname LIKE 'small\\_%' AND relkind = 'r'"
	),
	'5000',
	"its rows read back");
$many->stop;

# Bugs row 143 through the real commands: with delta_memory at 1 MB, whose
# 1 KB minimum slots hold 1,024 storages, one transaction makes 1,000 tables,
# with their TOAST relations and indexes, and completes; one transaction writes
# a row of 200 bytes into each of 1,200 tables, passing a spill's threshold,
# and completes;
# then two such transactions at once, over tables of their own, both complete,
# and the delta's used bytes are back at their start once both have ended and
# their tables folded.
my $small = PostgreSQL::Test::Cluster->new('small');
$small->init;
$small->append_conf('postgresql.conf',
	"max_connections = 100\ndelta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s"
);
$small->start;
$small->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
is(tables($small, 'created', 1000, 1000),
	'', 'one transaction making 1,000 tables past the minimum slots completes');
tables($small, $_, 1200, 1200) for 'spilled', 'first', 'second';
my $used = 'SELECT doltgres_test_delta(0, 0, 0)';
my $start = $small->safe_psql('postgres', $used);
my ($one_code, $one_out, $one_err) = $small->psql('postgres',
	"SET statement_timeout = '600s'; " . written('spilled', 1200, 200));
is($one_code . $one_err, '0',
	'one transaction spilling more storages than the budget holds in minimum slots completes'
);
my @sessions;
for my $prefix ('first', 'second')
{
	my $session = $small->background_psql('postgres', on_error_stop => 0);
	$session->query_safe("SET statement_timeout = '600s'");
	$session->query_until(qr/started/,
		"\\echo started\n" . written($prefix, 1200, 200) . ";\n\\echo done\n");
	push @sessions, $session;
}
for my $session (@sessions)
{
	$session->{stdout} = '';
	$session->query('SELECT 1');
	$session->quit;
}
is( $small->safe_psql(
		'postgres',
		"SELECT sum((xpath('/row/c/text()', query_to_xml(format('SELECT count(*) AS c FROM %I', relname), false, true, '')))[1]::text::int8) FROM pg_class WHERE relkind = 'r' AND (relname LIKE 'spilled\\_%' OR relname LIKE 'first\\_%' OR relname LIKE 'second\\_%')"
	),
	'3600',
	'two such transactions at once both complete, every row read back');
$small->safe_psql('postgres',
	"SELECT count(doltgres_test_fold(oid::regclass)) FROM pg_class WHERE relkind = 'r' AND (relname LIKE 'spilled\\_%' OR relname LIKE 'first\\_%' OR relname LIKE 'second\\_%')"
);
is($small->safe_psql('postgres', $used),
	$start, "the delta's used bytes are back at their start once both ended and folded");
$small->stop;

# Bugs row 242: an UPDATE ... FROM through a nested loop whose outer scan reads
# a source whose rows its writer spilled, held at its first read of the
# source's spills while a fold of the source installs a base and drops the
# spills, updates every row, as heap's twin's does.
my %joined;
for my $am ('', 'USING doltgres_twin')
{
	my $p = $am eq '' ? '' : 'heap_';
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE ${p}joined_source (k int4, v text) $am;
INSERT INTO ${p}joined_source SELECT g, repeat('s', 100) FROM generate_series(1, 40000) g;
CREATE TABLE ${p}joined_target (k int4 PRIMARY KEY, v text) $am;
INSERT INTO ${p}joined_target SELECT g, 't' FROM generate_series(1, 40000) g;
});
	my $plan = "SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;"
	  . " SET max_parallel_workers_per_gather = 0;";
	my $update = "UPDATE ${p}joined_target t SET v = s.v || 'u' FROM ${p}joined_source s WHERE t.k = s.k";
	if ($am eq '')
	{
		my $updater = $node->background_psql('postgres');
		$updater->query_safe(
			"$plan SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
		);
		$updater->query_until(qr/started/, "\\echo started\n$update;\n");
		$node->wait_for_event('client backend', 'doltgres-spill-slots-read');
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('joined_source')");
		$node->safe_psql('postgres',
			"SELECT injection_points_detach('doltgres-spill-slots-read'); SELECT injection_points_wakeup('doltgres-spill-slots-read')"
		);
		$updater->query_safe('SELECT 1');
		$updater->quit;
	}
	else
	{
		$node->safe_psql('postgres', "$plan $update");
	}
	$joined{$p} = $node->safe_psql('postgres',
		"SELECT count(*) || ' ' || md5(string_agg(k || v, ',' ORDER BY k)) FROM ${p}joined_target");
}
is($joined{''}, $joined{heap_},
	"an UPDATE ... FROM whose source scan a fold's drop passes under updates every row as heap's twin's");

# Bugs row 286: a helper spilling an idle transaction's versions pauses after
# taking them out of its memtable and before listing its file, and the
# transaction commits; a second transaction's upsert of the same keys, which
# waits for the first while it runs, spills its own versions, and once the
# helper lists its file every key reads as the second transaction's.
$node->safe_psql('postgres',
	'CREATE TABLE helped_order (k int4 PRIMARY KEY, v text)');
my $first = $node->background_psql('postgres');
$first->query_safe('BEGIN');
$first->query_safe(
	"INSERT INTO helped_order SELECT g, 'old' || g FROM generate_series(1, 1000) g"
);
my $helper = $node->background_psql('postgres');
$helper->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-linking', 'wait')"
);
$helper->query_until(qr/helping/,
	"\\echo helping\nSELECT doltgres_test_delta(61, 0, 0);\n");
$node->wait_for_event('client backend', 'doltgres-spill-linking');
$first->query_until(qr/committing/, "\\echo committing\nCOMMIT;\n");
my $upserter = $node->background_psql('postgres');
my $upserter_pid = $upserter->query_safe('SELECT pg_backend_pid()');
$upserter->query_until(
	qr/upserting/,
	"\\echo upserting\n"
	  . keyed(
		'helped_order',
		"INSERT INTO helped_order SELECT g, 'new' || g FROM generate_series(1, 1000) g ON CONFLICT (k) DO UPDATE SET v = EXCLUDED.v"
	  )
	  . ";\n");
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE pid = $upserter_pid AND (state = 'idle' OR wait_event_type = 'Lock')"
) or die 'the upsert neither ended nor waited';
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-spill-linking'); SELECT injection_points_wakeup('doltgres-spill-linking')"
);
$helper->query_safe('SELECT 1');
$first->query_safe('SELECT 1');
$upserter->query_safe('SELECT 1');
$helper->quit;
$first->quit;
$upserter->quit;
is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM doltgres_test_scan('helped_order', false, -1) r WHERE r LIKE '%new%'}
	),
	'1000',
	"a helper's spill listed after the helped transaction's commit leaves a later transaction's spill of the same keys newer"
);

done_testing();
