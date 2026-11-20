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

# The fold (the fold text, out/review/macpf-fold-design.md): the commit status
# table its selection reads (the text's 3.10), each commit's end reaching its
# transaction's entry through the committer's slot, COMMIT PREPARED's through
# the process that commits it, and a replayed commit's from its record.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fold');
$node->init;
$node->append_conf('postgresql.conf', "max_prepared_transactions = 2\nwal_level = logical");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql(
	'postgres', q{
CREATE TABLE t (id int4 PRIMARY KEY);
CREATE TABLE src (id int4);
CREATE FUNCTION w(n int4) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DELETE FROM src;
	INSERT INTO src VALUES (n);
	PERFORM doltgres_test_insert('t', 'src', false, 0);
END $$;
});

# A commit's end is below 2^62, and an entry pending in a process is at or past it.
my $pending = 2**62;

# outcome returns the outcome word of an xid's entry, -1 for none.
sub outcome
{
	my ($xid) = @_;
	return $node->safe_psql('postgres', "SELECT doltgres_test_status_table(4, $xid, 0)");
}

# t's first write records its identity pairs (design item 10), whose versions
# count at their transaction's entry too, so it comes first, in its own
# transaction.
$node->safe_psql('postgres', 'SELECT w(0)');

# A commit leaves its entry pending in the committer, whose next write moves
# the end of its commit record into the entry. Each write is a row the test
# module inserts into the fork's storage of t, which counts its row's version
# and its free space take at its transaction's entry (bugs row 148).
my $moved = $node->safe_psql(
	'postgres', q{
	BEGIN;
	DO $$ BEGIN PERFORM w(1); END $$;
	SELECT pg_current_xact_id()::text::int8 AS x \gset
	SELECT doltgres_test_status_table(1, :x, 0);
	COMMIT;
	SELECT pg_current_wal_insert_lsn() - '0/0' AS after \gset
	SELECT doltgres_test_status_table(4, :x, 0) >= 2 ^ 62;
	DO $$ BEGIN PERFORM w(2); END $$;
	SELECT doltgres_test_status_table(4, :x, 0) BETWEEN 1 AND :after;
});
my ($counted, $pending_end, $reached) = split /\n/, $moved;
is($counted, '2', "a write counts its row's version and its take at its transaction's entry");
is("$pending_end $reached", 't t', "a commit's end reaches its entry through its committer's slot at the committer's next write");

# COMMIT PREPARED by another backend points the prepared transaction's entry
# at itself; its next write moves the end of COMMIT PREPARED's record in.
my $prepared = $node->safe_psql(
	'postgres', q{
	BEGIN;
	DO $$ BEGIN PERFORM w(3); END $$;
	SELECT pg_current_xact_id()::text::int8;
	PREPARE TRANSACTION 'p';
});
my $committed = $node->safe_psql(
	'postgres', qq{
	COMMIT PREPARED 'p';
	SELECT pg_current_wal_insert_lsn() - '0/0' AS after \\gset
	DO \$\$ BEGIN PERFORM w(4); END \$\$;
	SELECT doltgres_test_status_table(4, $prepared, 0) BETWEEN 1 AND :after;
});
is($committed, 't', "COMMIT PREPARED's end reaches the prepared transaction's entry through the committer's slot");

# A crash replays a transaction's row change, which takes its entry, and its
# commit record, whose end the entry then holds.
my $replayed = $node->safe_psql(
	'postgres', q{
	BEGIN;
	DO $$ BEGIN PERFORM w(5); END $$;
	SELECT pg_current_xact_id()::text::int8;
	COMMIT;
});
my $end = $node->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn() - '0/0'");
$node->stop('immediate');
$node->start;
my $word = outcome($replayed);
ok($word > 0 && $word < $pending && $word <= $end, "a replayed commit's end reaches its entry from the commit record");

# A transaction spanning a checkpoint, with a crash after its commit: the
# checkpoint's runs bring its first row's version and take back, whose load
# counts them, and redo replays its second's, counting those.
my $spanning = $node->safe_psql(
	'postgres', q{
	BEGIN;
	DO $$ BEGIN PERFORM w(6); END $$;
	CHECKPOINT;
	DO $$ BEGIN PERFORM w(7); END $$;
	SELECT pg_current_xact_id()::text::int8;
	COMMIT;
});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', "SELECT doltgres_test_status_table(1, $spanning, 0)"),
	'4', "a transaction's count is rebuilt by the runs' load and by redo, one for each version, rows' and takes'");

# A table each fold case writes through the test module, one row a call, and
# reads through the fork's scan as its ids in key order.
$node->safe_psql(
	'postgres', q{
CREATE TABLE f (id int4 PRIMARY KEY);
CREATE TABLE fsrc (id int4);
CREATE FUNCTION wf(n int4) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DELETE FROM fsrc;
	INSERT INTO fsrc VALUES (n);
	PERFORM doltgres_test_insert('f', 'fsrc', false, 0);
END $$;
});
my $ids = q{SELECT string_agg(substring(r from '^\S+ \S+ (.*)$'), ',') FROM doltgres_test_scan('f', false, -1) r};

# A fold applies the rows committed below its point and leaves a row whose
# transaction is still open in the delta; each row reads once after.
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(1); PERFORM wf(2); END $$');
my $open = $node->background_psql('postgres');
$open->query_safe('BEGIN; DO $$ BEGIN PERFORM wf(3); END $$;');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 't',
	'a fold applies the rows committed below its point and keeps a row whose commit is past it');
$open->query_safe('COMMIT');
$open->quit;
is($node->safe_psql('postgres', $ids), '1,2,3', "a fold's rows read once, beside the delta's");

# The fold text's draft 12 (design A): a fold applies no transaction at or past
# its storage's horizon, so a REPEATABLE READ snapshot holds back the fold of
# what committed after it, as it holds back VACUUM, past a first fold of what
# committed before it, and reads the rows it saw; the fold applies them once the
# snapshot ends.
my $rr = $node->background_psql('postgres');
$rr->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$rr->query_safe($ids);
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(4); END $$');
$node->safe_psql('postgres', "SELECT doltgres_test_fold('f')");
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 'f',
	"a fold applies nothing a running snapshot counts as running");
is($rr->query_safe($ids), '1,2,3', 'the snapshot reads the rows it saw');
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(5); END $$');
$rr->query_safe('COMMIT');
$rr->quit;
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 't', 'the fold applies them once the snapshot ends');
is($node->safe_psql('postgres', $ids), '1,2,3,4,5', 'every row reads once after the folds');

# A prepared transaction holds its storage's horizon until it commits, as it
# holds back VACUUM, so a fold applies nothing committed after it.
$node->safe_psql(
	'postgres', q{
	CREATE TABLE psrc (id int4);
	INSERT INTO psrc VALUES (6);
	BEGIN;
	SELECT doltgres_test_insert('f', 'psrc', false, 0);
	PREPARE TRANSACTION 'held';
});
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(7); END $$');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 'f', 'a prepared transaction holds back the fold');
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(8); END $$');
$node->safe_psql('postgres', "COMMIT PREPARED 'held'");
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 't', 'the fold applies once the prepared one commits');
is($node->safe_psql('postgres', $ids), '1,2,3,4,5,6,7,8', 'the prepared row reads after its commit');

# A cursor's snapshot, registered past its statement, holds back the fold
# until the cursor closes.
my $cursor = $node->background_psql('postgres');
$cursor->query_safe(q{BEGIN; DECLARE c CURSOR FOR SELECT * FROM doltgres_test_scan('f', false, -1);});
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(9); END $$');
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(10); END $$');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 'f', 'an open cursor holds back the fold');
$cursor->query_safe('CLOSE c');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 't', "a cursor's snapshot holds back the fold until the cursor closes");
$cursor->query_safe('COMMIT');
$cursor->quit;

# A snapshot imported with SET TRANSACTION SNAPSHOT holds back the fold until
# the importing transaction ends.
my $exporter = $node->background_psql('postgres');
$exporter->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $exported = $exporter->query_safe('SELECT pg_export_snapshot()');
my $importer = $node->background_psql('postgres');
$importer->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ; SET TRANSACTION SNAPSHOT '$exported';");
$exporter->query_safe('COMMIT');
$exporter->quit;
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(11); END $$');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 'f', 'an imported snapshot holds back the fold');
$importer->query_safe('COMMIT');
$importer->quit;
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('f')"), 't',
	'an imported snapshot holds back the fold until its transaction ends');

# A logical slot's catalog xmin holds back a user catalog table's fold, as it
# holds back VACUUM of the table.
$node->safe_psql(
	'postgres', q{
CREATE TABLE uc (id int4 PRIMARY KEY) WITH (user_catalog_table = true);
SELECT pg_create_logical_replication_slot('held', 'pgoutput');
INSERT INTO fsrc VALUES (0);
});
my $uc = q{DELETE FROM fsrc; INSERT INTO fsrc VALUES (%d); SELECT doltgres_test_insert('uc', 'fsrc', false, 0);};
$node->safe_psql('postgres', sprintf($uc, 1));
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('uc')"), 'f',
	"a logical slot's catalog xmin holds back a user catalog table's fold");
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('held')");
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('uc')"), 't', "the user catalog table's fold applies once the slot goes");

# Bugs rows 78 and 79: a folded row keeps its tuple id and its value. A scan,
# a fetch by its ctid, an UPDATE, and a DELETE each find it after its fold, a
# keyed table's row and a keyless table's copy alike, as they find the same row
# of a heap table.
$node->safe_psql(
	'postgres', q{
CREATE TABLE folded_source AS SELECT g AS id, 'v' || g AS v FROM generate_series(1, 3) g;
CREATE TABLE keyed (id int4 PRIMARY KEY, v text);
CREATE TABLE keyless (id int4, v text);
CREATE TABLE heap_keyed (id int4 PRIMARY KEY, v text);
CREATE TABLE heap_keyless (id int4, v text);
INSERT INTO heap_keyed SELECT * FROM folded_source;
INSERT INTO heap_keyless SELECT * FROM folded_source;
SELECT doltgres_test_insert('keyed', 'folded_source', false, 0);
SELECT doltgres_test_insert('keyless', 'folded_source', false, 0);
});
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('keyed'), doltgres_test_fold('keyless')"),
	't|t', "a fold applies each table's rows");
my $seq = 'SET enable_indexscan = off; SET enable_bitmapscan = off;';
foreach my $table ('keyed', 'keyless')
{
	# over returns the SQL that runs a statement over the heap table as it is,
	# or over the fork's table through the fork's routine.
	my $over = sub {
		my ($statement, $heap) = @_;
		my $sql = $statement =~ s/%/$heap ? "heap_$table" : $table/ger;
		return $sql if $heap;
		$sql =~ s/'/''/g;
		return $statement =~ /^SELECT/
		  ? "SELECT doltgres_test_standing('$table', '$sql')"
		  : "SELECT doltgres_test_write('$table', true, '$sql')";
	};
	foreach my $step (
		[ q{SELECT ctid::text || ' ' || %::text FROM % ORDER BY ctid}, 'a scan' ],
		[ q{SELECT ctid::text || ' ' || %::text FROM % WHERE ctid = '(0,2)'}, 'a fetch by its ctid' ],
		[ q{UPDATE % SET v = 'w' WHERE id = 1}, 'an UPDATE' ],
		[ q{DELETE FROM % WHERE id = 3}, 'a DELETE' ],
		[ q{SELECT ctid::text || ' ' || %::text FROM % ORDER BY ctid}, 'a read after both' ])
	{
		my ($statement, $what) = @$step;
		my $heap = $node->safe_psql('postgres', $seq . $over->($statement, 1));
		my ($failed, $fork, $error) = $node->psql('postgres', $seq . $over->($statement, 0));
		$fork = ($error =~ /(ERROR:[^\n]*)/)[0] if $failed;
		$heap = 1 if $statement !~ /^SELECT/;
		is($fork, $heap, "$what finds a folded row of the $table table as heap's finds it");
	}
}

# Bugs row 80: a fold's base holds the sizes of the rows it applied. After a
# fold of an insert, of an update that grows a row, and of a delete, the base's
# heap bytes and rows are those of heap's twin's live tuples, each its line
# pointer's length aligned and the line pointer, and its blocks are the twin's.
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE TABLE sized_source AS SELECT g AS id, 'v' || g AS v FROM generate_series(1, 3) g;
CREATE TABLE sized (id int4 PRIMARY KEY, v text);
CREATE TABLE heap_sized (id int4 PRIMARY KEY, v text);
INSERT INTO heap_sized SELECT * FROM sized_source;
SELECT doltgres_test_insert('sized', 'sized_source', false, 0);
});
my $live = q{SELECT sum((lp_len + 7) / 8 * 8 + 4) || ' ' || count(*)
  FROM heap_page_items(get_raw_page('heap_sized', 0)) WHERE lp_flags = 1 AND t_xmax = '0'};
my $based = q{SELECT split_part(doltgres_test_base_sizes('sized'), ' ', 1) || ' ' || split_part(doltgres_test_base_sizes('sized'), ' ', 2)};
foreach my $step (
	[ undef, 'an insert', "the base's heap bytes and rows after an insert are heap's twin's live tuples'" ],
	[
		q{UPDATE % SET v = repeat('w', 100) WHERE id = 1}, 'an update that grows a row',
		"the base's heap bytes and rows after an update that grows a row are heap's twin's live tuples'"
	],
	[
		q{DELETE FROM % WHERE id = 2}, 'a delete',
		"the base's heap bytes and rows after a delete are heap's twin's live tuples'"
	])
{
	my ($statement, $what, $sized) = @$step;
	if (defined $statement)
	{
		(my $heap = $statement) =~ s/%/heap_sized/g;
		(my $fork = $statement) =~ s/%/sized/g;
		$fork =~ s/'/''/g;
		$node->safe_psql('postgres', $heap);
		$node->safe_psql('postgres',
			"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT doltgres_test_write('sized', true, '$fork')");
	}
	is($node->safe_psql('postgres', "SELECT doltgres_test_fold('sized')"), 't', "a fold applies $what");
	is($node->safe_psql('postgres', $based), $node->safe_psql('postgres', $live), $sized);
	is($node->safe_psql('postgres', "SELECT doltgres_test_blocks('sized')"),
		$node->safe_psql('postgres', "SELECT pg_relation_size('heap_sized') / 8192"),
		"the table's blocks after $what are heap's twin's");
}

# Bugs row 85: a fold whose point is the insert position just past a WAL
# switch, which leaves it past the next segment's page header with no record
# there, flushes only the WAL that exists and installs.
$node->safe_psql('postgres', 'DO $$ BEGIN PERFORM wf(85); END $$');
is($node->safe_psql('postgres', "SELECT pg_switch_wal() IS NOT NULL, doltgres_test_fold('f')"),
	't|t', 'a fold just past a WAL switch flushes the WAL that exists and installs');

# The maps' fold: a fork table's out-of-line value is an entry of its TOAST
# relation's value map. A fold of the TOAST relation moves the entry into its
# base, so a second fold finds no value map version left. The table's own fold
# keeps the row's value id in its base entry (bugs row 98), so the row reads its
# value back from the TOAST relation's base, after the fold and after a
# restart.
$node->safe_psql(
	'postgres', q{
CREATE TABLE outlined (id int4, v text);
ALTER TABLE outlined ALTER COLUMN v SET STORAGE EXTERNAL;
CREATE TABLE outsrc (id int4, v text);
INSERT INTO outsrc SELECT 1, string_agg(md5(n::text), '') FROM generate_series(1, 400) n;
SELECT count(*) FROM doltgres_test_insert('outlined', 'outsrc', false, 0);
});
my $outlined = sub {
	my ($query) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('outlined', '$query')");
};
is( $outlined->(
		'SELECT pg_column_toast_chunk_id(v) IS NOT NULL FROM outlined'),
	't',
	'the row stores its value out of line');
my $toast =
  q{(SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'outlined'::regclass)};
is($node->safe_psql('postgres', "SELECT doltgres_test_fold($toast)"),
	't', "a fold of a TOAST relation's value map installs");
is($node->safe_psql('postgres', "SELECT doltgres_test_fold($toast)"),
	'f',
	'a second fold of the TOAST relation finds no value map version left');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('outlined')"),
	't', "the table's own fold installs");
my $md5 = $node->safe_psql('postgres',
	"SELECT md5(string_agg(md5(n::text), '')) FROM generate_series(1, 400) n"
);
is($outlined->('SELECT md5(v) FROM outlined'), $md5,
	"the row reads its out-of-line value back from the TOAST relation's base"
);
$node->restart;
is($outlined->('SELECT md5(v) FROM outlined'),
	$md5, 'the row reads its out-of-line value back after a restart');


# Bugs row 98's count (PLAN's value ids row): a base entry whose trailer lists a
# value id its row holds no value out of line for is data corruption once a
# read deforms the whole row.
$node->safe_psql(
	'postgres', q{
CREATE TABLE miscounted (id int4, v text);
CREATE TABLE miscounted_src (id int4, v text);
INSERT INTO miscounted_src VALUES (1, 'a');
SELECT doltgres_test_base('miscounted', 'miscounted_src', 1);
});
my ($miscounted, $stdout, $stderr) = $node->psql(
	'postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_standing('miscounted', 'SELECT id || v FROM miscounted')"
);
like(
	$stderr,
	qr/XX001: a stored row's value ids aren't its values out of line/,
	"a whole read of a base entry with one value id too many is data corruption"
);

# PRevPF 8's M2 (the fold text's draft 12): a lazy VACUUM's xmin, which
# Postgres' horizon passes over, holds back a fold of a table in its database,
# since a fold takes no lock that conflicts with VACUUM's: with a VACUUM of a heap
# table paused after it took its snapshot, a fold applies nothing committed
# after that, and once the VACUUM finishes the fold applies it.
$node->safe_psql('postgres',
	'CREATE EXTENSION IF NOT EXISTS injection_points; CREATE TABLE vheap (k int4); CREATE TABLE vheld (k int4 PRIMARY KEY)');
$node->safe_psql('postgres', q{SELECT doltgres_test_write('vheld', true, 'INSERT INTO vheld VALUES (1)')});
$node->safe_psql('postgres', "SELECT doltgres_test_fold('vheld')");
my $vacuum = $node->background_psql('postgres');
$vacuum->query_safe('SELECT injection_points_set_local()');
$vacuum->query_safe("SELECT injection_points_attach('vacuum-index-cleanup-auto', 'wait')");
$vacuum->query_until(qr/start/, "\\echo start\nVACUUM vheap;\n");
$node->wait_for_event('client backend', 'vacuum-index-cleanup-auto');
$node->safe_psql('postgres', q{SELECT doltgres_test_write('vheld', true, 'INSERT INTO vheld VALUES (2)')});
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('vheld')"), 'f',
	"a lazy VACUUM held paused holds back a fold of its database's table, and finishes after it");
$node->safe_psql('postgres', "SELECT injection_points_wakeup('vacuum-index-cleanup-auto')");
$vacuum->query_safe("SELECT injection_points_detach('vacuum-index-cleanup-auto')");
$vacuum->quit;
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('vheld')"), 't', 'the fold applies once the VACUUM finishes');

# PRevPF 8's A1 (the fold text's draft 12): a safe CREATE INDEX CONCURRENTLY's
# snapshot, which takes no lock a fold conflicts with, holds back a fold of a
# table in its database: with the build paused in its comparator while it holds
# its snapshot, a fold applies nothing committed after that, and once the build
# finishes with every row indexed once the fold applies it.
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION held_cmp(a int4, b int4) RETURNS int4 LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
	PERFORM public.injection_points_run('doltgres-fold-cic');
	RETURN pg_catalog.btint4cmp(a, b);
END $$;
CREATE OPERATOR CLASS held_ops FOR TYPE int4 USING btree AS
	OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 held_cmp(int4, int4);
CREATE TABLE cheap (k int4);
INSERT INTO cheap SELECT generate_series(1, 3);
CREATE TABLE cheld (k int4 PRIMARY KEY);
});
$node->safe_psql('postgres', q{SELECT doltgres_test_write('cheld', true, 'INSERT INTO cheld VALUES (1)')});
$node->safe_psql('postgres', "SELECT doltgres_test_fold('cheld')");
my $concurrent = $node->background_psql('postgres');
$concurrent->query_safe('SET max_parallel_maintenance_workers = 0');
$concurrent->query_safe('SELECT injection_points_set_local()');
$concurrent->query_safe("SELECT injection_points_attach('doltgres-fold-cic', 'wait')");
$concurrent->query_until(qr/start/, "\\echo start\nCREATE INDEX CONCURRENTLY cheap_k ON cheap (k held_ops);\n");
$node->wait_for_event('client backend', 'doltgres-fold-cic');
$node->safe_psql('postgres', q{SELECT doltgres_test_write('cheld', true, 'INSERT INTO cheld VALUES (2)')});
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('cheld')"), 'f',
	"a safe CREATE INDEX CONCURRENTLY held paused with its snapshot holds back a fold of its database's table");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-cic'); SELECT injection_points_wakeup('doltgres-fold-cic')");
$concurrent->quit;
is( $node->safe_psql(
		'postgres', q{
	SELECT indisvalid FROM pg_index WHERE indexrelid = 'cheap_k'::regclass;
	SET enable_seqscan = off;
	SELECT string_agg(k::text, ' ' ORDER BY k) FROM cheap WHERE k > 0;
}),
	"t\n1 2 3",
	'the concurrent build finishes with every row indexed once');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('cheld')"), 't', 'the fold applies once the build finishes');

# PRevPF 8's M3 (the fold text's draft 12): a fold's bound covers every version
# it applies, the value map's included: with a REPEATABLE READ snapshot open, a
# delete of a row whose value is out of line and an insert of another such row
# commit; neither the TOAST relation's fold nor the table's applies them, the
# snapshot reads the deleted row's value back, and once it ends both apply.
$node->safe_psql(
	'postgres', q{
CREATE TABLE downed (id int4, v text);
ALTER TABLE downed ALTER COLUMN v SET STORAGE EXTERNAL;
CREATE TABLE downed_src (id int4, v text);
INSERT INTO downed_src SELECT 1, string_agg(md5(n::text), '') FROM generate_series(1, 400) n;
SELECT count(*) FROM doltgres_test_insert('downed', 'downed_src', false, 0);
});
my $downed_toast = q{(SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'downed'::regclass)};
my $both = "SELECT doltgres_test_fold($downed_toast), doltgres_test_fold('downed')";
is($node->safe_psql('postgres', $both), 't|t', "the TOAST relation's and the table's folds apply the row");
my $downed_read = q{SELECT doltgres_test_standing('downed', 'SELECT md5(v) FROM downed WHERE id = 1')};
my $down_reader = $node->background_psql('postgres');
$down_reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$down_reader->query_safe($downed_read);
$node->safe_psql(
	'postgres', q{
SELECT doltgres_test_write('downed', true, 'DELETE FROM downed WHERE id = 1');
UPDATE downed_src SET id = 2, v = v || 'x';
SELECT count(*) FROM doltgres_test_insert('downed', 'downed_src', false, 0);
});
is($node->safe_psql('postgres', $both), 'f|f',
	"neither fold applies a delete or a value written past an older snapshot's horizon");
is($down_reader->query_safe($downed_read), $md5,
	"the older snapshot reads the deleted row's out-of-line value back across both folds");
$down_reader->query_safe('COMMIT');
$down_reader->quit;
is($node->safe_psql('postgres', $both), 't|t', 'both folds apply once the snapshot ends');

# Bugs row 142: a fold after a crash restart that follows an earlier fold
# applies only what its base hasn't folded. Ten rows of a keyless table are
# folded, an immediate stop and start replays their versions into the delta,
# one more row is written, and a second fold leaves eleven rows in the base
# and in a read, where a fold that applied the replayed versions again
# doubled them.
$node->safe_psql(
	'postgres', q{
CREATE TABLE restarted_source AS SELECT g AS id, 'v' || g AS v FROM generate_series(1, 10) g;
CREATE TABLE restarted (id int4, v text);
SELECT doltgres_test_insert('restarted', 'restarted_source', false, 0);
});
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('restarted')"),
	't', 'the first fold applies the ten rows');
$node->stop('immediate');
$node->start;
$node->safe_psql(
	'postgres', q{
TRUNCATE restarted_source;
INSERT INTO restarted_source VALUES (11, 'v11');
SELECT doltgres_test_insert('restarted', 'restarted_source', false, 0);
});
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('restarted')"),
	't', 'the fold after the restart applies the eleventh row');
is( $node->safe_psql(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('restarted'), ' ', 2) || ' ' || doltgres_test_standing('restarted', 'SELECT count(*) FROM restarted')"
	),
	'11 11',
	'the base and a read hold the eleven rows, the replayed ten not applied twice');

$node->stop;

# Bugs row 142 through a checkpoint: a fold that installs after a checkpoint's
# runs were written with the versions it applies, and before the checkpoint's
# save names its base beside them, leaves a crash's start with those versions
# loaded back into the delta and the fold's record passed over, as the switched
# 053 found. The case's node holds no other storage, so the checkpoint waits
# after the table's runs, while the fold of its ten rows installs. After the
# crash, an eleventh row is written, and a second fold leaves eleven rows in
# the base and in a read, where applying the loaded ten again doubled them.
my $raced = PostgreSQL::Test::Cluster->new('raced');
$raced->init;
$raced->start;
$raced->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION doltgres_test;
CREATE TABLE raced_source AS SELECT g AS id, 'v' || g AS v FROM generate_series(1, 10) g;
CREATE TABLE raced (id int4, v text);
SELECT doltgres_test_insert('raced', 'raced_source', false, 0);
SELECT injection_points_attach('doltgres-runs-rows-swept', 'wait');
});
my $checkpoint = $raced->background_psql('postgres');
$checkpoint->query_until(qr/start/, "\\echo start\nCHECKPOINT;\n");
$raced->wait_for_event('checkpointer', 'doltgres-runs-rows-swept');
is( $raced->safe_psql('postgres', "SELECT doltgres_test_fold('raced')"),
	't',
	'the fold applies the ten rows while the checkpoint waits after its runs'
);
$raced->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-runs-rows-swept'); SELECT injection_points_wakeup('doltgres-runs-rows-swept')"
);
$checkpoint->quit;
$raced->stop('immediate');
$raced->start;
$raced->safe_psql(
	'postgres', q{
TRUNCATE raced_source;
INSERT INTO raced_source VALUES (11, 'v11');
SELECT doltgres_test_insert('raced', 'raced_source', false, 0);
});
is($raced->safe_psql('postgres', "SELECT doltgres_test_fold('raced')"),
	't', 'the fold after the crash applies the eleventh row');
is( $raced->safe_psql(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('raced'), ' ', 2) || ' ' || doltgres_test_standing('raced', 'SELECT count(*) FROM raced')"
	),
	'11 11',
	'the base and a read hold the eleven rows, the loaded ten not applied twice'
);
$raced->stop;
done_testing();
