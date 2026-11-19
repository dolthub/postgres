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


# The fork's inserts (PLAN items 2, 5, and 7; step (4a)), through the test
# module: doltgres_test_insert inserts a heap table's rows through the fork's
# table access method routine, by tuple_insert or by multi_insert as COPY's
# path does, and returns each row's ctid, which is compared with the ctid heap
# gives the same rows inserted the same way into a heap table.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('insert');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# inserted returns the ctids the fork gives a source table's rows inserted
# into a target, one at a time or all at once.
sub inserted
{
	my ($target, $source, $multi) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_insert('$target', '$source', $multi, 0)");
}

# heap returns the ctids heap gives a source table's rows inserted into a new
# heap table of the same columns.
sub heap
{
	my ($source) = @_;
	return $node->safe_psql(
		'postgres', qq{
CREATE TABLE heap_$source (LIKE $source);
INSERT INTO heap_$source SELECT * FROM $source;
SELECT ctid FROM heap_$source;
});
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_one (a int4, b text);
INSERT INTO source_one VALUES (1, 'one');
CREATE TABLE one (a int4 PRIMARY KEY, b text);
});
is(inserted('one', 'source_one', 'false'),
	'(0,1)', 'a row inserted reads back with its ctid and xmin');

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_zero ();
INSERT INTO source_zero SELECT FROM generate_series(1, 292);
CREATE TABLE zero ();
});
is(inserted('zero', 'source_zero', 'false'),
	heap('source_zero'),
	"zero-width rows take (0,1) up to (0,291), then (1,1), as heap's do");

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_wide (a int4, b text);
INSERT INTO source_wide SELECT n, repeat('x', 200) FROM generate_series(1, 40) n;
CREATE TABLE wide (a int4, b text);
});
is( inserted('wide', 'source_wide', 'false'),
	heap('source_wide'),
	"a page that fills by bytes before its last offset sends the next row to the next block, as heap's does"
);

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_copied (a int4, b text);
INSERT INTO source_copied SELECT n, repeat('y', n % 300) FROM generate_series(1, 500) n;
CREATE TABLE copied (a int4, b text);
});
is(inserted('copied', 'source_copied', 'true'),
	heap('source_copied'), "multi_insert's rows take heap's ctids in order");

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_big (a text);
INSERT INTO source_big VALUES (repeat('x', 9000));
CREATE TABLE big (a text STORAGE PLAIN);
});
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	"SELECT doltgres_test_insert('big', 'source_big', false, 0)",
	extra_params => [ '-v', 'VERBOSITY=sqlstate' ]);
like($stderr, qr/ERROR:  54000/, 'a row too big is refused with 54000');
($ret, $stdout, $stderr) = $node->psql('postgres',
	"SELECT doltgres_test_insert('big', 'source_big', false, 0)");
like(
	$stderr,
	qr/ERROR:  row is too big: size 9032, maximum size 8160/,
	'a row too big is refused as heap refuses it');

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_full ();
INSERT INTO source_full SELECT FROM generate_series(1, 291);
CREATE TABLE full_page ();
SELECT doltgres_test_base('full_page', 'source_full');
CREATE TABLE source_full_row ();
INSERT INTO source_full_row DEFAULT VALUES;
});
is( $node->safe_psql('postgres', q{SELECT doltgres_test_blocks('full_page')}),
	$node->safe_psql(
		'postgres', q{SELECT pg_relation_size('source_full') / 8192}),
	"a base of 291 zero-width rows has heap's one block");
is( inserted('full_page', 'source_full_row', 'false'),
	'(1,1)',
	"the next insert after a full page's base starts the next block, as heap's does"
);

# new_cids returns the description of each of heap's NEW_CID records a
# statement that prints nothing logs as the second command of its
# transaction, the file number of the relation it writes replaced by a word.
sub new_cids
{
	my ($relation, $statement) = @_;
	return $node->safe_psql(
		'postgres', qq{
SELECT pg_current_wal_insert_lsn() AS before \\gset
BEGIN;
INSERT INTO filler VALUES (1);
$statement
COMMIT;
SELECT replace(description, '/' || pg_relation_filenode('$relation') || ',', '/written,')
  FROM pg_get_wal_records_info(:'before', pg_current_wal_flush_lsn())
  WHERE resource_manager = 'Heap2' AND record_type = 'NEW_CID';
});
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pg_walinspect;
CREATE TABLE filler (a int4);
CREATE TABLE source_decoded (a int4, b text);
INSERT INTO source_decoded VALUES (1, 'one'), (2, 'two');
CREATE TABLE replica_decoded (a int4 PRIMARY KEY, b text) WITH (user_catalog_table = true);
CREATE TABLE decoded (a int4 PRIMARY KEY, b text) WITH (user_catalog_table = true);
CREATE TABLE decoded_many (a int4 PRIMARY KEY, b text) WITH (user_catalog_table = true);
CREATE TABLE heap_decoded (a int4 PRIMARY KEY, b text) WITH (user_catalog_table = true);
CREATE TABLE undecoded (a int4 PRIMARY KEY, b text);
});
is( new_cids(
		'replica_decoded',
		"SELECT count(*) AS inserted FROM doltgres_test_insert('replica_decoded', 'source_decoded', false, 0) \\gset"
	),
	'',
	"a user_catalog_table's insert logs no NEW_CID record under wal_level replica"
);

$node->append_conf('postgresql.conf', 'wal_level = logical');
$node->restart;
my $heaps = new_cids('heap_decoded',
	'INSERT INTO heap_decoded SELECT * FROM source_decoded;');
is( $heaps,
	"rel: 1663/5/written, tid: 0/1, cmin: 1, cmax: 4294967295, combo: 4294967295\n"
	  . 'rel: 1663/5/written, tid: 0/2, cmin: 1, cmax: 4294967295, combo: 4294967295',
	"heap logs a NEW_CID record of each row inserted into a user_catalog_table"
);
is( new_cids(
		'decoded',
		"SELECT count(*) AS inserted FROM doltgres_test_insert('decoded', 'source_decoded', false, 0) \\gset"
	),
	$heaps,
	"a user_catalog_table's insert logs heap's NEW_CID record under wal_level logical"
);
is( new_cids(
		'decoded_many',
		"SELECT count(*) AS inserted FROM doltgres_test_insert('decoded_many', 'source_decoded', true, 0) \\gset"
	),
	$heaps,
	"multi_insert logs heap's NEW_CID record of each row");
is( new_cids(
		'undecoded',
		"SELECT count(*) AS inserted FROM doltgres_test_insert('undecoded', 'source_decoded', false, 0) \\gset"
	),
	'',
	'an insert into a table logical decoding does not read logs no NEW_CID record'
);

# Bugs row 65: a streaming standby's startup process, connected to no
# database, redoes the fork's inserts in the repository of the database each
# record names.
$node->set_replication_conf;
$node->append_conf('postgresql.conf', 'max_wal_senders = 4');
$node->restart;
$node->backup('replayed');
my $standby = PostgreSQL::Test::Cluster->new('insert_standby');
$standby->init_from_backup($node, 'replayed', has_streaming => 1);
$standby->start;
$node->safe_psql('postgres', 'CREATE TABLE replayed (LIKE source_decoded)');
inserted('replayed', 'source_decoded', 'false');
inserted('replayed', 'source_decoded', 'true');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT 1'),
	'1', "a standby redoes the fork's inserts and stays up");
ok( !$standby->log_contains('connected to no database'),
	"a standby's redo of an insert finds the record's repository");

# B1: a standby's redo of a drop releases the repositories it opened, so a
# database made again with the dropped one's oid is redone into its new
# repository, and the standby stays up with no error of the old one's.
$node->safe_psql('postgres', 'CREATE DATABASE remade');
my $remade = $node->safe_psql('postgres',
	q{SELECT oid FROM pg_database WHERE datname = 'remade'});
my $fill = q{
CREATE EXTENSION doltgres_test; CALL doltgres_test_twins();
CREATE TABLE source (a int4, b text);
INSERT INTO source VALUES (1, 'one');
CREATE TABLE target (LIKE source);
SELECT doltgres_test_insert('target', 'source', false, 0);
};
$node->safe_psql('remade', $fill);
$node->wait_for_replay_catchup($standby);
$node->safe_psql('postgres', 'DROP DATABASE remade');
$node->safe_psql('postgres', "CREATE DATABASE remade OID = $remade");
$node->safe_psql('remade', $fill);
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT 1'),
	'1', "a standby redoes a database made again with a dropped one's oid");
ok(!$standby->log_contains(qr/ERROR|FATAL|PANIC/),
	"a standby's redo of a database made again with a dropped one's oid finds its new repository"
);
$standby->stop;

# A write skew between two serializable transactions, each scanning one table
# and inserting into the other, is refused with Postgres' 40001 over the
# fork's tables as over heap's: each scan's relation lock meets the other's
# insert, whose check for a conflict in is heap_insert's.
sub step
{
	my ($session, $sql) = @_;
	$session->query($sql);
	my ($error) = $session->{stderr} =~ /(ERROR:[^\n]*)/;
	$session->{stderr} = '';
	return $error // 'ok';
}

sub skew
{
	my ($kind) = @_;
	my ($one, $two) = ("${kind}_skew_a", "${kind}_skew_b");
	$node->safe_psql('postgres',
		"CREATE TABLE $one (a int4 PRIMARY KEY, b text); CREATE TABLE $two (a int4 PRIMARY KEY, b text);"
	);
	my %read = (
		heap => sub { "SELECT count(*) FROM $_[0]" },
		fork => sub {
			"SELECT count(*) FROM doltgres_test_scan('$_[0]', false, -1)";
		});
	my %write = (
		heap => sub { "INSERT INTO $_[0] SELECT * FROM source_one" },
		fork => sub {
			"SELECT doltgres_test_insert('$_[0]', 'source_one', false, 0)";
		});
	my $first = $node->background_psql('postgres', on_error_stop => 0);
	my $second = $node->background_psql('postgres', on_error_stop => 0);
	my @steps = (
		step($first, 'BEGIN ISOLATION LEVEL SERIALIZABLE'),
		step($second, 'BEGIN ISOLATION LEVEL SERIALIZABLE'),
		step($first, $read{$kind}->($one)),
		step($second, $read{$kind}->($two)),
		step($first, $write{$kind}->($two)),
		step($second, $write{$kind}->($one)),
		step($first, 'COMMIT'),
		step($second, 'COMMIT'));
	$first->quit;
	$second->quit;
	return join("\n", @steps);
}
my $heap_skew = skew('heap');
like(
	$heap_skew,
	qr/ERROR:  could not serialize access/,
	"heap's scans and inserts make a write skew it refuses");
is(skew('fork'), $heap_skew,
	"a write skew of serializable scans and inserts of the fork's tables is refused with 40001, as heap's is"
);

# A row's values out of line read back through the TOAST pointers its read
# forms from its value ids, detoasted through its TOAST relation's map: whole
# through the output function, and a slice through detoast_attr_slice. An id
# the map doesn't hold is refused as heap refuses a value with no chunk.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_toasted (a int4, b text);
INSERT INTO source_toasted SELECT n, (SELECT string_agg(md5(n || ' ' || i), '') FROM generate_series(1, 400) i)
FROM generate_series(1, 3) n;
CREATE TABLE toasted (a int4 PRIMARY KEY, b text);
});
my ($toasted) = split /\n/, inserted('toasted', 'source_toasted', 'false');
is( $node->safe_psql(
		'postgres',
		q{SELECT md5(string_agg(substring(r from '^\S+ \S+ (.*)$'), ',')) FROM doltgres_test_scan('toasted', false, -1) r}
	),
	$node->safe_psql(
		'postgres',
		q{SELECT md5(string_agg(a || ' ' || b, ',' ORDER BY a)) FROM source_toasted}
	),
	"a toasted row inserted reads back whole");
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_toast('toasted', '$toasted', 2, 5000, 100, 0)"),
	$node->safe_psql(
		'postgres',
		q{SELECT substr(b, 5001, 100) FROM source_toasted WHERE a = 1}),
	"a slice of a toasted value reads its own bytes");
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	"SELECT doltgres_test_toast('toasted', '$toasted', 2, 0, 100, 1)",
	extra_params => [ '-v', 'VERBOSITY=verbose' ]);
like(
	$stderr,
	qr/ERROR:  XX001: missing chunk number 0 for toast value 1 in pg_toast_\d+/,
	"a TOAST value its relation's map doesn't hold is refused as heap refuses a missing chunk"
);

# A row of 450 values each moved out of line, each of 16,400 bytes, whose
# references take more than a tree's entry, keeps the rest of its fields in an
# overflow chunk, which a read deforms through the session's repository.
my $columns = join(', ', map { "c$_ text" } 1 .. 450);
my $values = join(', ', map { "repeat(md5('$_'), 513)::text" } 1 .. 450);
$node->safe_psql(
	'postgres', qq{
CREATE TABLE source_overflowed ($columns);
INSERT INTO source_overflowed VALUES ($values);
CREATE TABLE overflowed ($columns);
SELECT doltgres_test_insert('overflowed', 'source_overflowed', false, 0);
});
is( $node->safe_psql(
		'postgres',
		q{SELECT md5(substring(r from '^\S+ \S+ (.*)$')) FROM doltgres_test_scan('overflowed', false, -1) r}
	),
	$node->safe_psql(
		'postgres',
		'SELECT md5('
		  . join(q{ || ' ' || }, map { "c$_" } 1 .. 450)
		  . ') FROM source_overflowed'),
	"a row whose references take more than a tree's entry reads its overflow chunk back whole"
);
# Bugs row 70: a tsvector's key is exact and is read back from the key: a
# table keyed by a tsvector of many short lexemes, some with positions, gives
# back its key as heap's table does.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_lexemes AS SELECT string_agg(chr(96 + n) || CASE WHEN n % 3 = 0 THEN ':' || n || 'A' ELSE '' END, ' ')::tsvector AS k FROM generate_series(1, 26) n;
CREATE TABLE lexemes (k tsvector PRIMARY KEY);
CREATE TABLE heap_lexemes (k tsvector PRIMARY KEY);
INSERT INTO heap_lexemes SELECT * FROM source_lexemes;
SELECT doltgres_test_insert('lexemes', 'source_lexemes', false, 0);
});
is( $node->safe_psql(
		'postgres',
		q{SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT doltgres_test_standing('lexemes', 'SELECT k::text FROM lexemes')}
	),
	$node->safe_psql('postgres', 'SELECT k::text FROM heap_lexemes'),
	"a table keyed by a tsvector of many short lexemes gives back its key as heap's does");

# Bugs row 97: an insert written frozen, by COPY FREEZE's path (multi_insert
# with TABLE_INSERT_FROZEN, 4), takes no room in the commit status table on
# the primary, and a standby's redo of its row changes takes none either, so
# the standby stays up and reads the rows.
$standby->start;
$node->safe_psql('postgres',
	"CREATE TABLE frozen (LIKE source_decoded); SELECT doltgres_test_insert('frozen', 'source_decoded', true, 4)");
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql('postgres',
		"SELECT count(*) FROM doltgres_test_scan('frozen', false, -1)"),
	$node->safe_psql('postgres', 'SELECT count(*) FROM source_decoded'),
	"a standby redoes a frozen insert's rows and reads them");
ok(!$standby->log_contains('asked for room'),
	"a standby's redo of a frozen insert asks the status table for no room");
$standby->stop;

# Bugs row 126: a row of two 4,096-byte MAIN values, which heap stores by
# compressing them (regress strings' toasttest), is stored and reads back,
# since the fork refuses only a row heap's rules can't store with its MAIN
# values out of line; they stay inline in the tree (Differences row 35).
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_main (f1 text, f2 text);
INSERT INTO source_main VALUES (repeat('1234', 1024), repeat('5678', 1024));
CREATE TABLE main_rows (f1 text STORAGE MAIN, f2 text STORAGE MAIN);
});
is(inserted('main_rows', 'source_main', 'false'),
	'(0,1)', 'a row of MAIN values that heap stores compressed is stored');
is( $node->safe_psql(
		'postgres',
		q{SELECT md5(substring(r from '^\S+ \S+ (.*)$')) FROM doltgres_test_scan('main_rows', false, -1) r}
	),
	$node->safe_psql(
		'postgres', q{SELECT md5(f1 || ' ' || f2) FROM source_main}),
	'a row of MAIN values reads back whole');

# The insert's free space request (CommPF 9's ruling on the switch's regress
# insert): a row of 1000 bytes in a table of fill factor 10, whose reserve
# passes the most a request can ask, takes a nearly empty page as heap's
# RelationGetBufferForTuple caps its target, and rows of exactly
# MaxHeapTupleSize each take a page of their own, at the ctids heap's twin
# gives them.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_sparse (a int4, b text);
INSERT INTO source_sparse VALUES (1, NULL), (2, repeat('a', 1000)), (3, repeat('a', 1000)), (4, NULL);
CREATE TABLE sparse (a int4, b text) WITH (fillfactor = 10);
CREATE TABLE heap_sparse (a int4, b text) WITH (fillfactor = 10);
INSERT INTO heap_sparse SELECT * FROM source_sparse;
CREATE TABLE source_largest (v text STORAGE PLAIN);
INSERT INTO source_largest SELECT repeat('x', 8132) FROM generate_series(1, 2);
CREATE TABLE largest (v text STORAGE PLAIN);
CREATE TABLE heap_largest (v text STORAGE PLAIN);
INSERT INTO heap_largest SELECT * FROM source_largest;
});
is(inserted('sparse', 'source_sparse', 'false'),
	$node->safe_psql('postgres', 'SELECT ctid FROM heap_sparse'),
	"rows in a table of fill factor 10 take the ctids heap's twin gives them");
is(inserted('largest', 'source_largest', 'false'),
	$node->safe_psql('postgres', 'SELECT ctid FROM heap_largest'),
	"rows of MaxHeapTupleSize take the ctids heap's twin gives them");

# Bugs row 238: an INSERT ... SELECT that needs no projection hands over its
# scan's slot, whose descriptor is the source table's, so a value the source
# keeps out of line goes into a PLAIN column of a table with no TOAST relation
# by the target's storage: inline, as heap's twin stores it.
my %plain;
for my $am ('USING heap', 'USING doltgres_twin')
{
	my $p = $am eq 'USING heap' ? '' : 'heap_';
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE ${p}outside (v text) $am;
INSERT INTO ${p}outside SELECT repeat(md5(i::text), 250) FROM generate_series(1, 3) i;
CREATE TABLE ${p}plain (v text STORAGE PLAIN) $am;
});
	my ($code, $out, $err) =
	  $node->psql('postgres', "INSERT INTO ${p}plain SELECT v FROM ${p}outside");
	$plain{$p} = ($err =~ /(ERROR:[^\n]*)/)[0] // $node->safe_psql('postgres',
		"SELECT (SELECT reltoastrelid = 0 FROM pg_class WHERE relname = '${p}plain'), count(*), "
		  . "count(pg_column_toast_chunk_id(v)), md5(string_agg(v, '' ORDER BY v)) FROM ${p}plain");
}
is($plain{''}, $plain{heap_},
	"an INSERT ... SELECT of out-of-line values into a PLAIN column of a table with no TOAST relation stores them inline as heap's twin does");

$node->stop;
done_testing();
