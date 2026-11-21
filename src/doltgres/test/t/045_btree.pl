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

# Which indexes the fork stores (the btree text's section 4): a btree index on a
# table the fork stores gets the fork's copy of its routine, one with a column its
# class compares by a function included, and one on a catalog, an index of
# another method, and a partitioned index keep their own.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('btree');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$node->safe_psql(
	'postgres', q{
	CREATE TABLE stored (id int PRIMARY KEY, name text, at int);
	CREATE INDEX stored_name ON stored (name);
	CREATE INDEX stored_hash ON stored USING hash (at);
	CREATE TABLE parted (id int, name text) PARTITION BY RANGE (id);
	CREATE INDEX parted_name ON parted (name);
});

# routine returns whether an index got the fork's copy of its routine.
sub routine
{
	my ($index) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_index_routine('$index')");
}

is( routine('stored_name') . routine('pg_class_oid_index'),
	'tf',
	"a btree index on a fork table gets the fork's routine and one on a catalog keeps nbtree's"
);
is(routine('stored_hash'), 'f',
	'an index of another method keeps its own routine');
is(routine('parted_name'), 'f', 'a partitioned index keeps its own routine');

$node->safe_psql(
	'postgres', q{
	CREATE FUNCTION reversed_cmp(x int4, y int4) RETURNS int4 LANGUAGE sql AS 'SELECT btint4cmp(y, x)';
	CREATE OPERATOR CLASS reversed_ops FOR TYPE int4 USING btree AS
		OPERATOR 1 >, OPERATOR 2 >=, OPERATOR 3 =, OPERATOR 4 <=, OPERATOR 5 <,
		FUNCTION 1 reversed_cmp(int4, int4);
	CREATE TYPE reversed_range AS RANGE (subtype = int4, subtype_opclass = reversed_ops);
	CREATE TABLE compared (id int PRIMARY KEY, r reversed_range);
	CREATE INDEX compared_r ON compared (r);
});
is(routine('compared_r'), 't',
	"a btree index with a column its class compares by a function gets the fork's routine"
);

# entries returns the versions each of some indexes' storages holds, after a
# statement over a table that runs through the fork's routines, the table's
# own and each index's.
sub entries
{
	my ($table, $statement, @indexes) = @_;
	my $given = join('',
		map { "SELECT doltgres_test_index_routine('$_');\n" } @indexes);
	if (defined $statement)
	{
		$statement =~ s/'/''/g;
		$node->safe_psql('postgres',
			"$given SELECT doltgres_test_write('$table', true, '$statement');"
		);
	}
	return join(
		' | ',
		map {
			$node->safe_psql('postgres',
				"SELECT doltgres_test_index_entries('$_')")
		} @indexes);
}

$node->safe_psql(
	'postgres', q{
	CREATE TABLE written (id int PRIMARY KEY, name text, at int);
	CREATE INDEX written_name ON written (name);
	CREATE INDEX written_at ON written (at DESC) INCLUDE (name);
	CREATE TABLE copies (name text);
	CREATE INDEX copies_name ON copies (name);
});
is( entries('written',
		q{INSERT INTO written VALUES (1, 'a', 10), (2, 'b', 20)},
		'written_name', 'written_at')
	  . ' / '
	  . entries(
		'copies', q{INSERT INTO copies VALUES ('x'), ('x')},
		'copies_name'),
	"put\nput | put\nput / add (0,2), add (0,1)",
	"an insert writes each btree index's entry into the index's own storage");

$node->stop('immediate');
$node->start;
is( entries('written', undef, 'written_name', 'written_at') . ' / '
	  . entries('copies', undef, 'copies_name'),
	"put\nput | put\nput / add (0,2), add (0,1)",
	"a crash replays each btree index's entry into the index's own storage");

$node->safe_psql(
	'postgres', q{
	CREATE UNLOGGED TABLE passing (id int PRIMARY KEY, name text);
	CREATE INDEX passing_name ON passing (name);
	SELECT doltgres_test_index_buildempty('passing_name');
});
my $before =
  entries('passing', q{INSERT INTO passing VALUES (1, 'p')}, 'passing_name');
$node->stop('immediate');
$node->start;
is( $before . ' / ' . entries('passing', undef, 'passing_name'),
	'put / none',
	"an unlogged btree index's storage is made with no entries and is empty after a crash"
);

$node->safe_psql('postgres',
	'CREATE UNIQUE INDEX written_unique ON written (at)');
is(routine('written_unique'), 't', "a unique btree index gets the fork's routine");

$node->safe_psql(
	'postgres', q{
	CREATE TABLE moved (id int PRIMARY KEY, name text, at int);
	CREATE INDEX moved_name ON moved (name);
	CREATE INDEX moved_at ON moved (at) WHERE at > 5;
	CREATE TABLE twins (name text, n int);
	CREATE INDEX twins_n ON twins (n);
});
my @moved = ('moved_name', 'moved_at');
my @steps = (
	entries(
		'moved', q{INSERT INTO moved VALUES (1, 'a', 1), (2, 'b', 10)},
		@moved),
	entries('moved', q{UPDATE moved SET at = 2 WHERE id = 1}, @moved),
	entries('moved', q{UPDATE moved SET name = 'c' WHERE id = 2}, @moved),
	entries('moved', q{DELETE FROM moved WHERE id = 1}, @moved),
	entries(
		'twins', q{INSERT INTO twins VALUES ('x', 1), ('x', 1)}, 'twins_n'),
	entries(
		'twins', q{UPDATE twins SET name = 'y' WHERE ctid = '(0,1)'},
		'twins_n'),
	entries('twins', q{DELETE FROM twins WHERE ctid = '(0,2)'}, 'twins_n'));
is( join(' / ', @steps),
	join(' / ',
		"put\nput | put\nput",
		"put\nput | put\nput",
		"put\ndelete, put\nput | put\nput",
		"delete, put\ndelete, put\nput | put\nput",
		"add (0,2), add (0,1)",
		"add (0,3)\nremove (0,1), add (0,2), add (0,1)",
		"add (0,3)\nremove (0,2), remove (0,1), add (0,2), add (0,1)"),
	"an update or a delete removes the old row's entries and an update that changes none writes none"
);

$node->safe_psql(
	'postgres', q{
	CREATE TABLE guessed (k int PRIMARY KEY, v text);
	CREATE INDEX guessed_v ON guessed (v);
});
$node->safe_psql(
	'postgres', q{
	SELECT doltgres_test_index_routine('guessed_v');
	SELECT doltgres_test_speculative('guessed', true, 1, 'lost', false, true, true);
	SELECT doltgres_test_speculative('guessed', true, 2, 'kept', true, true, true);
});
is( entries('guessed', undef, 'guessed_v'),
	"put\ndelete, put",
	"a speculative insert that loses removes its entries and one that succeeds keeps them"
);

# rows returns a fork table's rows as text in their ctids' order, read through
# the fork's routine.
sub rows
{
	my ($table) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('$table', 'SELECT string_agg($table\::text, '' '' ORDER BY ctid) FROM $table')"
	);
}

$node->safe_psql(
	'postgres', q{
	CREATE FUNCTION picky(t text) RETURNS text LANGUAGE sql IMMUTABLE
		AS $$SELECT CASE WHEN t = 'boom' THEN (1 / (length(t) - 4))::text ELSE t END$$;
	CREATE TABLE refused (id int PRIMARY KEY, name text);
	CREATE INDEX refused_picky ON refused (picky(name));
});
my $kept =
  entries('refused', q{INSERT INTO refused VALUES (1, 'a')}, 'refused_picky')
  . ' '
  . rows('refused');
my ($code, $out, $err) = $node->psql(
	'postgres', q{
	SELECT doltgres_test_index_routine('refused_picky');
	SELECT doltgres_test_write('refused', true, 'UPDATE refused SET name = ''boom'' WHERE id = 1');
});
is( ($err =~ /(ERROR:\s+division by zero)/)[0] . ' / '
	  . entries('refused', undef, 'refused_picky') . ' '
	  . rows('refused'),
	"ERROR:  division by zero / $kept",
	"an update whose index expression raises after the row's change rolls back the row and its entries together"
);

$node->safe_psql(
	'postgres', q{
	CREATE TABLE saved (id int PRIMARY KEY, name text);
	CREATE INDEX saved_name ON saved (name);
});
entries('saved', q{INSERT INTO saved VALUES (1, 'a')}, 'saved_name');
$node->safe_psql(
	'postgres', q{
	BEGIN;
	SELECT doltgres_test_index_routine('saved_name');
	SAVEPOINT s;
	SELECT doltgres_test_write('saved', true, 'UPDATE saved SET name = ''b'' WHERE id = 1');
	ROLLBACK TO s;
	SELECT doltgres_test_write('saved', true, 'UPDATE saved SET name = ''c'' WHERE id = 1');
	COMMIT;
});
is( entries('saved', undef, 'saved_name') . ' ' . rows('saved'),
	"delete, put\nput (1,c)",
	"an update after a subtransaction's update was rolled back removes the old row's entry through the same removal"
);

entries('refused', q{UPDATE refused SET name = 'b' WHERE id = 1},
	'refused_picky');
is( entries('refused', q{DELETE FROM refused WHERE id = 1}, 'refused_picky'),
	"delete, put\ndelete, put\ndelete, put, delete, put",
	"an update and a delete remove an expression index's old entries");

$node->safe_psql(
	'postgres', q{
	CREATE TABLE dropped (id int PRIMARY KEY, name text, at int);
	CREATE INDEX dropped_name ON dropped (name);
	CREATE INDEX dropped_at ON dropped (at);
});
entries(
	'dropped', q{INSERT INTO dropped VALUES (1, 'a', 1), (2, 'b', 2)},
	'dropped_name', 'dropped_at');
$node->safe_psql(
	'postgres', q{
	BEGIN;
	SELECT doltgres_test_index_routine('dropped_name');
	SELECT doltgres_test_index_routine('dropped_at');
	SELECT doltgres_test_write('dropped', true, 'DELETE FROM dropped WHERE id = 1');
	DROP INDEX dropped_at;
	SELECT doltgres_test_index_routine('dropped_name');
	SELECT doltgres_test_write('dropped', true, 'DELETE FROM dropped WHERE id = 2');
	COMMIT;
});
is( entries('dropped', undef, 'dropped_name'),
	"delete, put\ndelete, put",
	"a removal an index's drop made stale is made again from the table's indexes left"
);

$node->safe_psql(
	'postgres', q{
	CREATE TABLE same (name text, n int);
	CREATE INDEX same_n ON same (n);
});
entries('same', q{INSERT INTO same VALUES ('x', 1)}, 'same_n');
is( entries('same', q{UPDATE same SET n = n}, 'same_n'),
	"add (0,2), remove (0,1), add (0,1)",
	"a keyless table's update that keeps its row's values moves its copy's entry to the new tuple id"
);

$node->safe_psql(
	'postgres', q{
	CREATE FUNCTION boom(i int) RETURNS int LANGUAGE sql IMMUTABLE AS $$SELECT CASE WHEN i = 13 THEN 1 / (i - 13) ELSE i END$$;
	CREATE TABLE reused (id int PRIMARY KEY, name text, at int);
	CREATE INDEX reused_boom ON reused USING hash (boom(at));
	CREATE INDEX reused_name ON reused (name);
});
entries('reused', q{INSERT INTO reused VALUES (1, 'a', 1)}, 'reused_name');
$node->psql(
	'postgres', q{
	SELECT doltgres_test_index_routine('reused_name');
	BEGIN;
	SELECT doltgres_test_write('reused', true, 'UPDATE reused SET at = 13 WHERE id = 1');
	ROLLBACK;
	BEGIN;
	SELECT doltgres_test_vacuum('reused', 'on');
	SELECT doltgres_test_write('reused', true, 'INSERT INTO reused VALUES (2, ''b'', 2)');
	COMMIT;
}, on_error_stop => 0);
is( entries('reused', undef, 'reused_name') . ' '
	  . $node->safe_psql(
		'postgres',
		q{SELECT doltgres_test_standing('reused', 'SELECT string_agg(ctid::text || '' '' || reused::text, '' '' ORDER BY ctid) FROM reused')}
	  ),
	"put\nput (0,1) (1,a,1) (0,2) (2,b,2)",
	"an insert at the tuple id an aborted update's new row had, given out again, writes its entry"
);

$node->safe_psql(
	'postgres', q{
	CREATE TABLE undone (id int PRIMARY KEY, name text);
	CREATE INDEX undone_name ON undone (name);
});
my $undone =
  entries('undone', q{INSERT INTO undone VALUES (1, 'a')}, 'undone_name');
foreach my $statement (
	q{INSERT INTO undone VALUES (2, 'b')},
	q{DELETE FROM undone WHERE id = 1})
{
	(my $quoted = $statement) =~ s/'/''/g;
	$node->safe_psql(
		'postgres', qq{
		BEGIN;
		SELECT doltgres_test_index_routine('undone_name');
		SELECT doltgres_test_write('undone', true, '$quoted');
		ROLLBACK;
	});
	$undone .= ' / ' . entries('undone', undef, 'undone_name');
}
is( $undone . ' ' . rows('undone'),
	"put / put / put (1,a)",
	"a rolled back insert's and delete's index versions are taken back and the index reads as before"
);

$node->safe_psql('postgres', q{
	CREATE TABLE built (id int PRIMARY KEY, name text);
	CREATE INDEX built_name ON built (name);
	CREATE TABLE built_twins (name text, n int);
	CREATE INDEX built_twins_n ON built_twins (n);
	CREATE TABLE built_source AS SELECT n AS id, 'r' || n AS name FROM generate_series(1, 3) n;
	CREATE TABLE built_twins_source (name text, n int);
	INSERT INTO built_twins_source VALUES ('x', 1), ('x', 1), ('y', 2);
	SELECT doltgres_test_insert('built', 'built_source', false, 0);
	SELECT doltgres_test_insert('built_twins', 'built_twins_source', false, 0);
});
is( join(
		' / ',
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_build('built', 'built_name')}),
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_base('built_name')}),
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_build('built_twins', 'built_twins_n')}),
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_base('built_twins_n')})),
	"3 3 whole / -\n-\n- / 3 3 whole / (0,1) (0,2)\n(0,3)",
	"an index built over a table's rows holds an entry for each live row");

# The btree text's B2a: a build by a REPEATABLE READ transaction whose snapshot
# predates another's committed insert still gives that row an entry, which the
# build's horizon, that snapshot's, keeps out of the base as a delta entry
# version under its writer (the fold text's draft 12).
$node->safe_psql('postgres', 'CREATE INDEX built_late ON built (name)');
my $builder = $node->background_psql('postgres', on_error_stop => 0);
$builder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres',
	q{SELECT doltgres_test_write('built', true, 'INSERT INTO built VALUES (4, ''r4'')')});
my $late = $builder->query_safe(q{SELECT doltgres_test_index_build('built', 'built_late')});
$builder->query_safe('COMMIT');
$builder->quit;
is( $late . ' / ' . $node->safe_psql('postgres', q{SELECT doltgres_test_index_base('built_late')}),
	"4 4 whole / -\n-\n-",
	"a build whose snapshot predates another's committed insert gives that row an entry");

# The btree text's B2b: a version a committed delete ended, which an older
# snapshot still sees, gets no entry and marks the build's chain broken, so
# index_build sets indcheckxmin.
$node->safe_psql('postgres', 'CREATE INDEX built_recent ON built (name)');
my $older = $node->background_psql('postgres', on_error_stop => 0);
$older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres',
	q{SET enable_indexscan = off; SET enable_bitmapscan = off;
	SELECT doltgres_test_write('built', true, 'DELETE FROM built WHERE id = 1')});
is( join(' / ',
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_build('built', 'built_recent')}),
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_base('built_recent')})),
	"3 3 broken / -\n-\n-",
	"a version a committed delete ended that an older snapshot still sees gets no entry and breaks the build's chain");
$older->query_safe('COMMIT');
$older->quit;

# The btree text's section 6: VACUUM's bulk delete of a fork btree index removes
# nothing and asks about no tid, and its cleanup reports the tree's counts, which
# for three rows are nbtree's on the reference: two pages, the metapage and one
# leaf, and three tuples. The VACUUM itself runs over the table first, which
# leaves the index's entries.
$node->safe_psql('postgres', q{
	CREATE TABLE vacuumed (id int PRIMARY KEY, name text);
	CREATE INDEX vacuumed_name ON vacuumed (name);
	CREATE TABLE vacuumed_source AS SELECT n AS id, 'v' || n AS name FROM generate_series(1, 3) n;
	SELECT doltgres_test_insert('vacuumed', 'vacuumed_source', false, 0);
	SELECT doltgres_test_index_build('vacuumed', 'vacuumed_name');
});
is( $node->safe_psql(
		'postgres', q{
	SELECT doltgres_test_index_routine('vacuumed_name');
	SELECT doltgres_test_vacuum('vacuumed', 'on');
	SELECT doltgres_test_index_vacuumed('vacuumed_name');
	SELECT doltgres_test_index_base('vacuumed_name');
}),
	"t\n\n0 2 3\n-\n-\n-",
	"VACUUM over a fork table with a btree index removes no entry and reports the tree's counts");


# PLAN's format row for an index storage's base leaf entry: a fold of an index's
# storage reads its base's entries back and writes the base a fresh build of the
# table's rows writes, a keyed table's entries with no stamp and a keyless one's
# copies counted with a stamp each, after inserts, an update, and deletes, one of
# them of one copy of two.
$node->safe_psql('postgres', q{
	CREATE TABLE folded (id int PRIMARY KEY, name text);
	CREATE INDEX folded_name ON folded (name);
	CREATE TABLE folded_twins (name text, n int);
	CREATE INDEX folded_twins_n ON folded_twins (n);
	CREATE TABLE folded_source AS SELECT n AS id, 'f' || n AS name FROM generate_series(1, 3) n;
	CREATE TABLE folded_twins_source (name text, n int);
	INSERT INTO folded_twins_source VALUES ('x', 1), ('x', 1), ('y', 2);
	SELECT doltgres_test_insert('folded', 'folded_source', false, 0);
	SELECT doltgres_test_insert('folded_twins', 'folded_twins_source', false, 0);
	SELECT doltgres_test_index_build('folded', 'folded_name');
	SELECT doltgres_test_index_build('folded_twins', 'folded_twins_n');
});
entries('folded', $_, 'folded_name') for (
	q{INSERT INTO folded VALUES (4, 'f4')},
	q{UPDATE folded SET name = 'g2' WHERE id + 0 = 2},
	q{DELETE FROM folded WHERE id + 0 = 1});
entries('folded_twins', $_, 'folded_twins_n') for (
	q{INSERT INTO folded_twins VALUES ('x', 1), ('z', 3)},
	q{UPDATE folded_twins SET n = 4 WHERE name = 'y'},
	q{DELETE FROM folded_twins WHERE ctid = '(0,1)'});
$node->safe_psql('postgres', q{
	CREATE INDEX folded_fresh ON folded (name);
	CREATE INDEX folded_twins_fresh ON folded_twins (n);
	SELECT doltgres_test_index_build('folded', 'folded_fresh');
	SELECT doltgres_test_index_build('folded_twins', 'folded_twins_fresh');
});
my %fold = map {
	$_ => $node->safe_psql('postgres', "SELECT doltgres_test_index_fold('$_')")
} qw(folded_name folded_fresh folded_twins_n folded_twins_fresh);
is( "$fold{folded_name} / $fold{folded_twins_n}",
	"$fold{folded_fresh} / $fold{folded_twins_fresh}",
	"a fold of an index's storage writes the base a fresh build of the table's rows writes");


# PLAN's stamp order of a keyless table's index entry (orev-7's R1): by writer,
# then by tuple id, each as a number, whether a build or a fold writes the
# stamps. Three copies of one row from one statement cross a block, at (0,1),
# (0,2), and (1,1); then a transaction with an older xid writes a copy at (2,1)
# after a newer one wrote its copy at (1,2), so the fold places the older's stamp
# before the newer's.
$node->safe_psql('postgres', q{
	CREATE TABLE crossed (name text STORAGE PLAIN, n int);
	CREATE INDEX crossed_n ON crossed (n);
	SELECT doltgres_test_index_build('crossed', 'crossed_n');
});
entries('crossed',
	q{INSERT INTO crossed VALUES (repeat('a', 3000), 1), (repeat('a', 3000), 1), (repeat('a', 3000), 1)},
	'crossed_n');
my $older_writer = $node->background_psql('postgres', on_error_stop => 0);
$older_writer->query_safe('BEGIN; SELECT pg_current_xact_id()');
entries('crossed', q{INSERT INTO crossed VALUES (repeat('a', 3000), 1)}, 'crossed_n');
$older_writer->query_safe(q{
	SELECT doltgres_test_index_routine('crossed_n');
	SELECT doltgres_test_write('crossed', true, 'INSERT INTO crossed VALUES (repeat(''a'', 3000), 1)');
	COMMIT;
});
$older_writer->quit;
$node->safe_psql('postgres', q{
	CREATE INDEX crossed_fresh ON crossed (n);
	SELECT doltgres_test_index_build('crossed', 'crossed_fresh');
});
my %crossed = map {
	$_ => $node->safe_psql('postgres', "SELECT doltgres_test_index_fold('$_')")
} qw(crossed_n crossed_fresh);
is( $crossed{crossed_n} . ' / ' . join(' ', $crossed{crossed_n} =~ /:(\(\d+,\d+\))/g),
	"$crossed{crossed_fresh} / (0,1) (0,2) (1,1) (2,1) (1,2)",
	"a keyless index entry's stamps are in writer and then tuple id order, whether a build or a fold writes them");

# nbtree's build progress (orev-7's R2), as the reference's
# pg_stat_progress_create_index shows a btree build: once the scan ends, the
# rows put in the sort are the total and the block counts are reset to 0, and
# the load counts each row it loads (the reference, read by a comparator at its
# first leaf split of 3,000 rows: "loading tuples in tree", blocks 0 and 0, 3000
# total, 367 done). Here the load waits after its first entry, which holds two
# copies of a keyless table's row.
$node->safe_psql('postgres', q{
	CREATE EXTENSION injection_points;
	CREATE TABLE progressed (name text, n int);
	CREATE INDEX progressed_n ON progressed (n);
	CREATE TABLE progressed_source (name text, n int);
	INSERT INTO progressed_source VALUES ('x', 1), ('x', 1), ('y', 2);
	SELECT doltgres_test_insert('progressed', 'progressed_source', false, 0);
	SELECT injection_points_attach('doltgres-index-loaded', 'wait');
});
my $loader = $node->background_psql('postgres', on_error_stop => 0);
$loader->query_until(qr/loading/,
	"\\echo loading\nSELECT doltgres_test_index_build('progressed', 'progressed_n');\n");
$node->wait_for_event('client backend', 'doltgres-index-loaded');
my $progress = $node->safe_psql('postgres', q{
	SELECT phase, blocks_total, blocks_done, tuples_total, tuples_done FROM pg_stat_progress_create_index
});
$node->safe_psql('postgres', q{
	SELECT injection_points_detach('doltgres-index-loaded');
	SELECT injection_points_wakeup('doltgres-index-loaded');
});
$loader->quit;
is($progress, 'building index: loading tuples in tree|0|0|3|2',
	"a build reports nbtree's progress: its rows as the load's total once the scan ends, its block counts reset, and each row loaded");


# A fold of a keyed table's index storage counts a base entry as one copy of its
# row (orev-7's R5): while a delete of the row runs, the fold keeps the entry,
# and once the delete rolls back the index's base is a fresh build's.
$node->safe_psql('postgres', q{
	CREATE TABLE held (id int PRIMARY KEY, name text);
	CREATE INDEX held_name ON held (name);
	CREATE TABLE held_source AS SELECT n AS id, 'h' || n AS name FROM generate_series(1, 3) n;
	SELECT doltgres_test_insert('held', 'held_source', false, 0);
	SELECT doltgres_test_index_build('held', 'held_name');
});
entries('held', q{INSERT INTO held VALUES (4, 'h4')}, 'held_name');
my $deleter = $node->background_psql('postgres', on_error_stop => 0);
$deleter->query_safe(q{
	BEGIN;
	SELECT doltgres_test_index_routine('held_name');
	SELECT doltgres_test_write('held', true, 'DELETE FROM held WHERE id + 0 = 1');
});
my $held = $node->safe_psql('postgres', "SELECT doltgres_test_index_fold('held_name')");
$deleter->query_safe('ROLLBACK');
$deleter->quit;
$node->safe_psql('postgres', q{
	CREATE INDEX held_fresh ON held (name);
	SELECT doltgres_test_index_build('held', 'held_fresh');
});
is($held, $node->safe_psql('postgres', "SELECT doltgres_test_index_fold('held_fresh')"),
	"a fold of a keyed table's index storage keeps the entry of a row whose delete is running");


# The btree text's section 6: a build reads the rows in the base its SnapshotAny
# reads, so a build after the table's fold, whose drop took the folded versions
# out of the delta, gives each live row an entry.
$node->safe_psql('postgres', q{
	CREATE TABLE refolded (id int PRIMARY KEY, name text);
	CREATE INDEX refolded_name ON refolded (name);
	CREATE TABLE refolded_source AS SELECT n AS id, 'r' || n AS name FROM generate_series(1, 3) n;
	SELECT doltgres_test_insert('refolded', 'refolded_source', false, 0);
	SELECT doltgres_test_fold('refolded');
});
is( $node->safe_psql('postgres', q{
	SELECT doltgres_test_index_build('refolded', 'refolded_name');
	SELECT doltgres_test_index_base('refolded_name');
}),
	"3 3 whole\n-\n-\n-",
	"a build after the table's fold gives each of its live rows an entry");


# Bugs row 109's half in the fork's own index (item 4's form D, Differences row
# 32): a fork btree index holds entries wider than nbtree's 8191 bytes, its key
# cut at the index's bound and its value's parts out of line, from a build over
# a 22,400-character value and an insert of a 12,000-character one, and a fold
# of its storage writes the base a fresh build writes.
$node->safe_psql('postgres', q{
	CREATE FUNCTION noise(n int) RETURNS text LANGUAGE sql
		AS $$ SELECT string_agg(md5(g::text), '') FROM generate_series(1, n) g $$;
	CREATE TABLE wide (id int PRIMARY KEY, w text);
	CREATE INDEX wide_w ON wide (w);
	CREATE TABLE wide_source AS SELECT 1 AS id, noise(700) AS w;
	SELECT doltgres_test_insert('wide', 'wide_source', false, 0);
});
my $wide_built =
  $node->safe_psql('postgres', q{SELECT doltgres_test_index_build('wide', 'wide_w')});
entries('wide', q{INSERT INTO wide VALUES (2, noise(375))}, 'wide_w');
my $wide_folded = $node->safe_psql('postgres', q{SELECT doltgres_test_index_fold('wide_w')});
$node->safe_psql('postgres', q{
	CREATE INDEX wide_fresh ON wide (w);
	SELECT doltgres_test_index_build('wide', 'wide_fresh');
});
is( $wide_built . ' / ' . scalar(() = $wide_folded =~ /^/mg) . ' / ' . $wide_folded,
	'1 1 whole / 2 / '
	  . $node->safe_psql('postgres', q{SELECT doltgres_test_index_fold('wide_fresh')}),
	"a fork btree index holds entries wider than nbtree's limit from a build and an insert, and its fold writes a fresh build's base"
);

# Bugs row 161: an index's entries a checkpoint's runs hold load back into the
# index's own storage, so the checkpoints after the load write them as an
# entries run again, and the start after those loads every run.
$node->safe_psql(
	'postgres', q{
	CREATE TABLE reloaded (id int PRIMARY KEY, name text);
	CREATE INDEX reloaded_name ON reloaded (name);
});
entries('reloaded', q{INSERT INTO reloaded VALUES (1, 'r')}, 'reloaded_name');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
my $reloaded_at = -s $node->logfile;
$node->restart;
ok( !$node->log_contains("runs aren't loaded", $reloaded_at),
	"a start loads the runs a checkpoint wrote of an index's entries loaded from runs"
);
is(entries('reloaded', undef, 'reloaded_name'),
	'put', "and the index's storage holds its entry");

# The fold text's draft 12 (design A): a build after inserts that an older
# REPEATABLE READ snapshot counts as running, one of them by a subtransaction,
# keeps their rows out of its base as delta entry versions under their writers,
# so the base folds no transaction that snapshot counts as running. The snapshot
# reads the table as heap's twin with no error, a fold of the index under it
# applies nothing, and once it ends the fold writes a fresh build's base.
$node->safe_psql('postgres', q{
	CREATE TABLE aged (id int PRIMARY KEY, name text);
	CREATE TABLE heap_aged (id int PRIMARY KEY, name text);
	CREATE TABLE aged_source AS SELECT n AS id, 'a' || n AS name FROM generate_series(1, 3) n;
	INSERT INTO heap_aged SELECT * FROM aged_source;
	SELECT doltgres_test_insert('aged', 'aged_source', false, 0);
	SELECT doltgres_test_fold('aged');
});
my $aged_read = q{SELECT string_agg(id || name, ' ' ORDER BY id) FROM %};
my $aged_older = $node->background_psql('postgres', on_error_stop => 0);
$aged_older->query_safe(
	'SET enable_indexscan = off; SET enable_bitmapscan = off; BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres', q{
	BEGIN;
	SAVEPOINT s;
	INSERT INTO heap_aged VALUES (4, 'a4');
	SELECT doltgres_test_write('aged', true, 'INSERT INTO aged VALUES (4, ''a4'')');
	RELEASE s;
	INSERT INTO heap_aged VALUES (5, 'a5');
	SELECT doltgres_test_write('aged', true, 'INSERT INTO aged VALUES (5, ''a5'')');
	COMMIT;
	CREATE INDEX aged_name ON aged (name);
	CREATE INDEX heap_aged_name ON heap_aged (name);
});
is( join(' / ',
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_base('aged_name')}),
		$node->safe_psql('postgres', q{SELECT doltgres_test_index_entries('aged_name')})),
	"-\n-\n- / put\nput",
	"a build keeps the rows of inserts an older snapshot counts as running out of its base, as delta entry versions");
my $aged_fork = $aged_older->query_safe(
	"SELECT doltgres_test_standing('aged', '" . ($aged_read =~ s/%/aged/r =~ s/'/''/gr) . "')");
is($aged_fork, $aged_older->query_safe($aged_read =~ s/%/heap_aged/r),
	"the older snapshot reads the table of an index built after its writers as heap's twin");
unlike($aged_older->{stderr}, qr/ERROR/, 'the older snapshot reads it with no error');
my $aged_held = $node->safe_psql('postgres', q{SELECT doltgres_test_index_fold('aged_name')});
is( scalar(() = $aged_held =~ /^/mg) . ' / '
	  . $node->safe_psql('postgres', q{SELECT doltgres_test_index_entries('aged_name')}),
	"3 / put\nput",
	"a fold of the built index under the older snapshot applies nothing it counts as running and keeps it");
$aged_older->query_safe('COMMIT');
$aged_older->quit;
$node->safe_psql('postgres', q{
	CREATE INDEX aged_fresh ON aged (name);
	SELECT doltgres_test_index_build('aged', 'aged_fresh');
});
is($node->safe_psql('postgres', q{SELECT doltgres_test_index_fold('aged_name')}),
	$node->safe_psql('postgres', q{SELECT doltgres_test_index_fold('aged_fresh')}),
	"once the older snapshot ends, the built index's fold writes a fresh build's base");

# Part 2's slice 5: a primary key's index has no tree of its own (the table
# rewrite text's R2), so it gets the fork's routine and writes no entry: its
# inserts write nothing to its storage, and its build writes nothing and answers
# the table's live rows.
$node->safe_psql('postgres', 'CREATE TABLE pk_only (id int PRIMARY KEY, name text)');
is( routine('pk_only_pkey') . ' / '
	  . entries('pk_only', q{INSERT INTO pk_only VALUES (1, 'a'), (2, 'b')}, 'pk_only_pkey')
	  . ' / '
	  . $node->safe_psql('postgres', q{
	SELECT doltgres_test_index_build('pk_only', 'pk_only_pkey');
	SELECT doltgres_test_index_base('pk_only_pkey');
}),
	"t / none / 2 2 whole\nnone",
	"a primary key's index gets the fork's routine and writes no entry of its own");

# Part 2's slice 5 and (4b)'s §4a: a deferrable primary key's index answers
# UNIQUE_CHECK_PARTIAL by the key's other rows, so an update that shifts every
# key succeeds at the statement's end, and one that moves a row onto another's
# key is refused there with the reference's 23505, as heap's twin is.
for my $table ('deferred', 'heap_deferred')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (k int PRIMARY KEY DEFERRABLE INITIALLY IMMEDIATE, v int)");
	my $routine = $fork eq 'true' ? "SELECT doltgres_test_index_routine('${table}_pkey'); " : '';
	$node->safe_psql('postgres',
		$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (1, 10), (2, 20)')");
}
my %shifted;
for my $table ('deferred', 'heap_deferred')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $routine = $fork eq 'true' ? "SELECT doltgres_test_index_routine('${table}_pkey'); " : '';
	my ($ret, $out, $err) = $node->psql('postgres',
		$routine
		  . "SELECT doltgres_test_write('$table', $fork, 'UPDATE $table SET k = k + 1'); "
		  . "SELECT doltgres_test_write('$table', $fork, 'UPDATE $table SET k = 3 WHERE v = 10')",
		extra_params => [ '-v', 'VERBOSITY=verbose' ]);
	my $refused = join("\n",
		grep { /^(ERROR|DETAIL):/ } map { s/^psql:[^:]*:\d+: //r } split /\n/, $err);
	my $read = $table =~ /^heap_/
	  ? "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k) FROM $table"
	  : "SELECT doltgres_test_standing('$table', 'SELECT string_agg(k || '' '' || v, '', '' ORDER BY k) FROM $table')";
	$shifted{$table} = ($refused . ' / '
		  . $node->safe_psql('postgres', "SET enable_indexscan = off; SET enable_bitmapscan = off; $read"))
	  =~ s/heap_//gr;
}
like($shifted{heap_deferred}, qr/^ERROR:  23505: duplicate key value violates unique constraint "deferred_pkey"/,
	"heap's twin refuses the move at the statement's end with 23505");
is($shifted{deferred}, $shifted{heap_deferred},
	"a deferrable primary key's index queues a recheck of a key another row holds and refuses it at the statement's end as the reference does"
);


# Part 2's slice 5: a primary key's index has no tree of its own, so VACUUM's
# cleanup of it counts its table's tree, which for three rows is nbtree's two
# pages and three tuples on the reference.
$node->safe_psql('postgres', q{
	CREATE TABLE pk_vacuumed (id int PRIMARY KEY, name text);
	CREATE TABLE pk_vacuumed_source AS SELECT n AS id, 'p' || n AS name FROM generate_series(1, 3) n;
	SELECT doltgres_test_insert('pk_vacuumed', 'pk_vacuumed_source', false, 0);
	SELECT doltgres_test_fold('pk_vacuumed');
});
is($node->safe_psql('postgres', q{SELECT doltgres_test_index_vacuumed('pk_vacuumed_pkey')}),
	'0 2 3', "VACUUM reports a primary key's index's counts from its table's tree");


# Part 2's slice 5: a unique index's build refuses two live rows with one key
# as nbtree's build does, with its message and detail, as heap's twin is
# refused, and builds over a repeat whose other row a committed delete ended.
my %unique_built;
for my $table ('repeated', 'heap_repeated')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres', "CREATE TABLE $table (id int PRIMARY KEY, k int)");
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (1, 10), (2, 10)')");
	my ($ret, $out, $err) = $node->psql('postgres',
		$fork eq 'true'
		? "CREATE UNIQUE INDEX ${table}_k ON $table (k); SELECT doltgres_test_index_build('$table', '${table}_k')"
		: "CREATE UNIQUE INDEX ${table}_k ON $table (k)",
		extra_params => [ '-v', 'VERBOSITY=verbose' ]);
	$unique_built{$table} = join("\n",
		grep { /^(ERROR|DETAIL):/ } map { s/^psql:[^:]*:\d+: //r } split /\n/, $err) =~ s/heap_//gr;
}
like($unique_built{heap_repeated}, qr/^ERROR:  23505: could not create unique index "repeated_k"/,
	"heap's twin's unique build refuses the repeat with 23505");
$node->safe_psql('postgres', q{
	SET enable_indexscan = off; SET enable_bitmapscan = off;
	SELECT doltgres_test_write('repeated', true, 'DELETE FROM repeated WHERE id = 1');
	CREATE UNIQUE INDEX repeated_kept ON repeated (k);
});
is( $unique_built{repeated} . ' / '
	  . $node->safe_psql('postgres', q{SELECT doltgres_test_index_build('repeated', 'repeated_kept')}),
	$unique_built{heap_repeated} . ' / 1 1 whole',
	"a unique btree index's build refuses a repeated key as nbtree's does, and builds over a repeat whose other row a committed delete ended"
);


# Bugs row 109's primary key half: a table whose text primary key takes a
# 12,000-character incompressible value inserts it through the fork, since its
# primary key's index has no tree of its own and the table's key holds it in
# its cut form, where nbtree's index_form_tuple refuses an entry over 8,191
# bytes (Differences row 32); the row is then read by its key.
$node->safe_psql('postgres', 'CREATE TABLE wide_pk (k text PRIMARY KEY, v int)');
is( $node->safe_psql('postgres', q{
	SELECT doltgres_test_index_routine('wide_pk_pkey');
	SELECT doltgres_test_write('wide_pk', true, 'INSERT INTO wide_pk VALUES (noise(375), 1)');
	SET enable_indexscan = off; SET enable_bitmapscan = off;
	SELECT doltgres_test_standing('wide_pk', 'SELECT length(k) || '' '' || v FROM wide_pk WHERE k = noise(375)');
}),
	"t\n1\n12000 1",
	"a fork table's primary key holds a 12,000-character key nbtree's index refuses, and the row is read by its key");


# The unknown methods' text, section 3 (orev-16's R3): an update of a column no
# index covers, on a table whose indexes are all the fork's, its primary key's
# included, changes no entry and answers TU_None, so the executor calls no
# index's aminsert, as the fork's aminsert's pause shows by raising when
# reached; beside an index the fork doesn't store, the update answers TU_All and
# reaches it.
for my $table ('tu_none', 'tu_all')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (id int PRIMARY KEY, k int, v text)");
	$node->safe_psql('postgres', "CREATE INDEX ${table}_k ON $table (k)");
	$node->safe_psql('postgres', "CREATE INDEX ${table}_h ON $table USING hash (k)")
	  if $table eq 'tu_all';
	$node->safe_psql('postgres',
		"SELECT doltgres_test_index_routine('${table}_pkey'), doltgres_test_index_routine('${table}_k'); "
		  . "SELECT doltgres_test_write('$table', true, 'INSERT INTO $table VALUES (1, 10, ''a'')')");
}
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-index-insert', 'error')");
my %updated;
for my $table ('tu_none', 'tu_all')
{
	my ($ret, $out, $err) = $node->psql('postgres',
		"SELECT doltgres_test_index_routine('${table}_pkey'), doltgres_test_index_routine('${table}_k'); "
		  . "SET enable_indexscan = off; SET enable_bitmapscan = off; "
		  . "SELECT doltgres_test_write('$table', true, 'UPDATE $table SET v = ''b'' WHERE id = 1')");
	$updated{$table} = $err =~ /doltgres-index-insert/ ? 'reached' : "returned $ret";
}
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-index-insert')");
is( "$updated{tu_none} / $updated{tu_all}",
	'returned 0 / reached',
	"an update of an unindexed column answers TU_None when every index is the fork's and TU_All beside another method's"
);

# The btree's part 2, slice 5 (U1): a fork unique index's insert of a key that
# another row's live entry holds is refused as heap's twin's nbtree refuses it,
# with 23505, its message, and its detail; and an update that changes another
# indexed column, which calls the unique index's aminsert with its row's own key,
# leaves that row's entry alone.
my %unique;
for my $table ('uq', 'heap_uq')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int PRIMARY KEY, v text, w int); "
		  . "CREATE UNIQUE INDEX ${table}_v ON $table (v); CREATE INDEX ${table}_w ON $table (w)");
	my $routine = $fork eq 'true'
	  ? join('', map { "SELECT doltgres_test_index_routine('${table}_$_'); " } 'v', 'w', 'pkey')
	  : '';
	$node->safe_psql('postgres',
		$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (1, ''x'', 1)')");
	my ($ret, $out, $err) = $node->psql('postgres',
		$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (2, ''x'', 2)')",
		extra_params => [ '-v', 'VERBOSITY=verbose' ]);
	$unique{$table} = join("\n",
		grep { /^(ERROR|DETAIL):/ } map { s/^psql:[^:]*:\d+: //r } split /\n/, $err) =~ s/heap_//gr;
	$node->safe_psql('postgres',
		$routine . "SELECT doltgres_test_write('$table', $fork, 'UPDATE $table SET w = 3 WHERE id + 0 = 1')");
}
is( $unique{uq} . ' / ' . entries('uq', undef, 'uq_v'),
	$unique{heap_uq} . ' / put',
	"a fork unique index refuses another row's key with nbtree's 23505 and leaves its own row's entry alone");

# The btree's part 2, slice 5: an insert into a fork unique index of a key
# another transaction's running insert holds waits for that transaction, as
# heap's twin's nbtree waits, and then is refused with 23505 when it commits and
# takes the key when it rolls back.
my %held;
for my $table ('uq', 'heap_uq')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $routine = $fork eq 'true'
	  ? join('', map { "SELECT doltgres_test_index_routine('${table}_$_'); " } 'v', 'w', 'pkey')
	  : '';
	for my $end ('COMMIT', 'ROLLBACK')
	{
		my $v = $end eq 'COMMIT' ? 'y' : 'z';
		my $id = $end eq 'COMMIT' ? 10 : 20;
		my ($holder, $waiter) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
		$holder->query_safe("BEGIN; $routine"
			  . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES ($id, ''$v'', 1)')");
		$waiter->query_safe($routine);
		$waiter->query_until(qr/started/,
			"\\echo started\nSELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES ($id + 1, ''$v'', 2)');\n");
		my $waited = $node->poll_query_until('postgres', 'SELECT count(*) > 0 FROM pg_locks WHERE NOT granted');
		$holder->query_safe($end);
		my ($output) = $waiter->query('SELECT 1');
		my ($error) = $waiter->{stderr} =~ /(ERROR:[^\n]*)/;
		push @{ $held{$table} }, ($waited ? 'waited' : 'ran') . ' ' . ($error // 'inserted') =~ s/heap_//gr;
		$_->quit for $holder, $waiter;
	}
}
is( join(' / ', @{ $held{uq} }),
	join(' / ', @{ $held{heap_uq} }),
	"a fork unique index's insert waits for a running insert of its key, refused when it commits and taking the key when it rolls back"
);

# The btree's part 2, slice 5: a NULLS DISTINCT unique index takes two rows
# whose key is NULL, and a NULLS NOT DISTINCT one refuses the second with
# 23505, as heap's twins' nbtree indexes do.
my %nulls;
for my $table ('nulled', 'heap_nulled')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int PRIMARY KEY, a text, b text); "
		  . "CREATE UNIQUE INDEX ${table}_a ON $table (a); "
		  . "CREATE UNIQUE INDEX ${table}_b ON $table (b) NULLS NOT DISTINCT");
	my $routine = $fork eq 'true'
	  ? join('', map { "SELECT doltgres_test_index_routine('${table}_$_'); " } 'a', 'b', 'pkey')
	  : '';
	my @results;
	for my $row ("1, NULL, ''x''", "2, NULL, ''y''", "3, ''z'', NULL", "4, ''w'', NULL")
	{
		my ($ret, $out, $err) = $node->psql('postgres',
			$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES ($row)')",
			extra_params => [ '-v', 'VERBOSITY=verbose' ]);
		my $refused = join("\n",
			grep { /^(ERROR|DETAIL):/ } map { s/^psql:[^:]*:\d+: //r } split /\n/, $err);
		push @results, ($refused || 'inserted') =~ s/heap_//gr;
	}
	$nulls{$table} = join(' / ', @results);
}
is($nulls{nulled}, $nulls{heap_nulled},
	"a NULLS DISTINCT unique index takes two NULL keys and a NULLS NOT DISTINCT one refuses the second, as nbtree does");

# The btree's part 2, slice 5 (orev-14's R1 and R3): an insert into a fork
# unique index of a key another row holds waits for a running delete of that
# row, held between the row's push and its entries' removal, and takes the key
# when it commits and is refused when it rolls back; and it waits for a running
# update of the row's other column and is refused once it commits, as heap's
# twin's nbtree waits on the row's xmax.
my %ended;
for my $table ('ended', 'heap_ended')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int PRIMARY KEY, v text, w int); "
		  . "CREATE UNIQUE INDEX ${table}_v ON $table (v); CREATE INDEX ${table}_w ON $table (w)");
	my $routine = $fork eq 'true'
	  ? join('', map { "SELECT doltgres_test_index_routine('${table}_$_'); " } 'v', 'w', 'pkey')
	  : '';
	my $id = 100;
	for my $step (['DELETE FROM % WHERE id + 0 = %id', 'COMMIT'],
		['DELETE FROM % WHERE id + 0 = %id', 'ROLLBACK'],
		['UPDATE % SET w = w + 1 WHERE id + 0 = %id', 'COMMIT'])
	{
		my ($change, $end) = @$step;
		my $v = "k$id";
		$node->safe_psql('postgres',
			$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES ($id, ''$v'', 1)')");
		(my $sql = $change) =~ s/%id/$id/g;
		$sql =~ s/%/$table/g;
		my ($holder, $waiter) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
		my $paused = $fork eq 'true' && $sql =~ /^DELETE/;
		$holder->query_safe("BEGIN; $routine");
		$holder->query_safe(
			"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-entries-removing', 'wait')")
		  if $paused;
		if ($paused)
		{
			$holder->query_until(qr/started/, "\\echo started\nSELECT doltgres_test_write('$table', $fork, '$sql');\n");
			$node->wait_for_event('client backend', 'doltgres-entries-removing');
		}
		else
		{
			$holder->query_safe("SELECT doltgres_test_write('$table', $fork, '$sql')");
		}
		$waiter->query_safe($routine);
		$waiter->query_until(qr/started/,
			"\\echo started\nSELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES ($id + 1, ''$v'', 2)');\n");
		my $waited = $node->poll_query_until('postgres', 'SELECT count(*) > 0 FROM pg_locks WHERE NOT granted');
		$node->safe_psql('postgres', "SELECT injection_points_wakeup('doltgres-entries-removing')") if $paused;
		$holder->query_safe($end);
		$waiter->query('SELECT 1');
		my ($error) = $waiter->{stderr} =~ /(ERROR:[^\n]*)/;
		push @{ $ended{$table} }, ($waited ? 'waited' : 'ran') . ' ' . ($error // 'inserted') =~ s/heap_//gr;
		$_->quit for $holder, $waiter;
		$id += 10;
	}
}
is( join(' / ', @{ $ended{ended} }),
	join(' / ', @{ $ended{heap_ended} }),
	"a fork unique index's insert waits for a held delete or a running update of the row holding its key, as nbtree's does"
);

# The btree's part 2, slice 5: a unique index's build over a key two rows
# share, one of which a committed delete ended while an older snapshot still
# sees it, builds, as heap's twin's nbtree build passes over the recently dead
# tuple's spool.
my %recent_built;
my $recent_older = $node->background_psql('postgres');
for my $table ('recent_repeat', 'heap_recent_repeat')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres', "CREATE TABLE $table (id int PRIMARY KEY, k int)");
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (1, 10)')");
}
$recent_older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
for my $table ('recent_repeat', 'heap_recent_repeat')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', $fork, 'DELETE FROM $table WHERE id + 0 = 1'); "
		  . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (2, 10)')");
	my ($ret, $out, $err) = $node->psql('postgres',
		$fork eq 'true'
		? "CREATE UNIQUE INDEX ${table}_k ON $table (k); SELECT doltgres_test_index_build('$table', '${table}_k')"
		: "CREATE UNIQUE INDEX ${table}_k ON $table (k)",
		extra_params => [ '-v', 'VERBOSITY=verbose' ]);
	$recent_built{$table} = ($err =~ /(ERROR:[^\n]*)/)[0] // 'built';
}
$recent_older->query_safe('COMMIT');
$recent_older->quit;
is( $recent_built{recent_repeat},
	$recent_built{heap_recent_repeat},
	"a unique index's build passes over a repeat by a recently dead row, as nbtree's build does");

# The btree's part 2, slice 5 (orev-14's R2 and R5): a keyless table's unique
# index refuses a second row of the same content with 23505, and a row whose
# copy another transaction's running insert holds waits for it, refused when it
# commits and taken when it rolls back, as heap's twin's nbtree does.
my %counted;
for my $table ('counted_uq', 'heap_counted_uq')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (v text, w int); CREATE UNIQUE INDEX ${table}_v ON $table (v)");
	my $routine = $fork eq 'true' ? "SELECT doltgres_test_index_routine('${table}_v'); " : '';
	my $insert = sub {
		my ($session, $v) = @_;
		return $routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (''$v'', 1)')";
	};
	$node->safe_psql('postgres', $insert->(undef, 'x'));
	my ($ret, $out, $err) = $node->psql('postgres', $insert->(undef, 'x'));
	my @results = (($err =~ /(ERROR:[^\n]*)/)[0] // 'inserted');
	for my $end ('COMMIT', 'ROLLBACK')
	{
		my $v = $end eq 'COMMIT' ? 'y' : 'z';
		my ($holder, $waiter) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
		$holder->query_safe('BEGIN; ' . $insert->($holder, $v));
		$waiter->query_until(qr/started/, "\\echo started\n" . $insert->($waiter, $v) . ";\n");
		my $waited = $node->poll_query_until('postgres', 'SELECT count(*) > 0 FROM pg_locks WHERE NOT granted');
		$holder->query_safe($end);
		$waiter->query('SELECT 1');
		my ($error) = $waiter->{stderr} =~ /(ERROR:[^\n]*)/;
		push @results, ($waited ? 'waited ' : 'ran ') . ($error // 'inserted');
		$_->quit for $holder, $waiter;
	}
	$counted{$table} = join(' / ', @results) =~ s/heap_//gr;
}
is($counted{counted_uq}, $counted{heap_counted_uq},
	"a keyless table's unique index refuses a second row of the same content and waits for a running copy, as nbtree does");

# The btree's part 2, slice 5: a fork unique index under a nondeterministic
# collation refuses a key equal by the collation to another row's though its
# bytes differ, as heap's twin's nbtree does.
$node->safe_psql('postgres',
	"CREATE COLLATION insensitive (provider = icu, locale = 'und-u-ks-level2', deterministic = false)");
my %collated;
for my $table ('collated', 'heap_collated')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int PRIMARY KEY, v text COLLATE insensitive); "
		  . "CREATE UNIQUE INDEX ${table}_v ON $table (v)");
	my $routine = $fork eq 'true'
	  ? "SELECT doltgres_test_index_routine('${table}_v'); SELECT doltgres_test_index_routine('${table}_pkey'); "
	  : '';
	my @results;
	for my $row ("1, ''a''", "2, ''A''")
	{
		my ($ret, $out, $err) = $node->psql('postgres',
			$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES ($row)')",
			extra_params => [ '-v', 'VERBOSITY=verbose' ]);
		push @results, (($err =~ /(ERROR:[^\n]*)/)[0] // 'inserted') =~ s/heap_//gr;
	}
	$collated{$table} = join(' / ', @results);
}
is( $collated{collated} . ' / '
	  . $node->safe_psql('postgres',
		"SELECT doltgres_test_index_routine('collated_v'); SELECT doltgres_test_index_entries('collated_v')"),
	$collated{heap_collated} . " / t\nput",
	"a fork unique index under a nondeterministic collation refuses a key its collation calls equal, as nbtree does");

# The btree's part 2, slice 5: a fork unique index over a type whose equal
# values have other bytes, numeric's 1.0 and 1.00, refuses the second as heap's
# twin's nbtree does.
my %scaled;
for my $table ('scaled', 'heap_scaled')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int PRIMARY KEY, v numeric); CREATE UNIQUE INDEX ${table}_v ON $table (v)");
	my $routine = $fork eq 'true'
	  ? "SELECT doltgres_test_index_routine('${table}_v'); SELECT doltgres_test_index_routine('${table}_pkey'); "
	  : '';
	my @results;
	for my $row ('1, 1.0', '2, 1.00')
	{
		my ($ret, $out, $err) = $node->psql('postgres',
			$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES ($row)')",
			extra_params => [ '-v', 'VERBOSITY=verbose' ]);
		push @results, (($err =~ /(ERROR:[^\n]*)/)[0] // 'inserted') =~ s/heap_//gr;
	}
	$scaled{$table} = join(' / ', @results);
}
is( $scaled{scaled} . ' / '
	  . $node->safe_psql('postgres',
		"SELECT doltgres_test_index_routine('scaled_v'); SELECT doltgres_test_index_entries('scaled_v')"),
	$scaled{heap_scaled} . " / t\nput",
	"a fork unique index refuses a numeric key equal to another row's in other bytes, as nbtree does");

# CommPF 9's ruling (a): a deferrable unique index other than a primary key's
# takes the fork's routine once slice 6's reader brings its checks, as a
# deferrable primary key's index, which has no tree of its own, does.
$node->safe_psql('postgres', q{
	CREATE TABLE deferred_uq (id int, v int,
		CONSTRAINT deferred_uq_pk PRIMARY KEY (id) DEFERRABLE,
		CONSTRAINT deferred_uq_v UNIQUE (v) DEFERRABLE);
});
is( join(' ', map { routine($_) } 'deferred_uq_v', 'deferred_uq_pk'),
	't t',
	"a deferrable unique index, a primary key's or another, takes the fork's routine"
);

# orev-29's S1: an insert into a fork unique index that resolves a key while
# another row's delete holds its entry, and reads that row only once the delete
# committed, finds it dead and resolves again, the delete's removal pushed before
# its commit freeing the key, so it takes the key as heap's twin's insert does
# after waiting for the delete.
my %reread;
for my $table ('reread', 'heap_reread')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int PRIMARY KEY, v text); CREATE UNIQUE INDEX ${table}_v ON $table (v)");
	my $routine = $fork eq 'true'
	  ? "SELECT doltgres_test_index_routine('${table}_v'); SELECT doltgres_test_index_routine('${table}_pkey'); "
	  : '';
	$node->safe_psql('postgres',
		$routine . "SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (1, ''m'')')");
	my ($holder, $inserter) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
	$holder->query_safe("BEGIN; $routine");
	$inserter->query_safe($routine);
	if ($fork eq 'true')
	{
		$holder->query_safe(
			"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-entries-removing', 'wait')");
		$inserter->query_safe(
			"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-unique-reading', 'wait')");
	}
	$holder->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_write('$table', $fork, 'DELETE FROM $table WHERE id + 0 = 1');\n");
	if ($fork eq 'true')
	{
		$node->wait_for_event('client backend', 'doltgres-entries-removing');
	}
	else
	{
		$holder->query_safe('SELECT 1');
	}
	$inserter->query_until(qr/started/,
		"\\echo started\nSELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (2, ''m'')');\n");
	if ($fork eq 'true')
	{
		$node->wait_for_event('client backend', 'doltgres-unique-reading');
		$node->safe_psql('postgres', "SELECT injection_points_wakeup('doltgres-entries-removing')");
	}
	else
	{
		$node->poll_query_until('postgres', 'SELECT count(*) > 0 FROM pg_locks WHERE NOT granted');
	}
	$holder->query_safe('SELECT 1');
	$holder->query_safe('COMMIT');
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('doltgres-unique-reading')") if $fork eq 'true';
	$inserter->query('SELECT 1');
	my ($error) = $inserter->{stderr} =~ /(ERROR:[^\n]*)/;
	$reread{$table} = $error // 'inserted';
	$_->quit for $holder, $inserter;
}
is($reread{reread}, $reread{heap_reread},
	"a fork unique insert that reads a row whose delete committed after its resolve resolves again and takes the key");

# The btree's part 2, slice 6 (CommPF 9's ruling (a)): a fork btree index
# answers the planner's tree height as its base tree's levels above its leaves,
# a primary key's index its table's tree's, and 0 for an empty base, where
# nbtree's routine answers its metapage's 0 for rows its file never held.
$node->safe_psql('postgres', q{
	CREATE TABLE tall (id int PRIMARY KEY, name text);
	CREATE INDEX tall_name ON tall (name);
	CREATE INDEX tall_empty ON tall (name);
	CREATE TABLE tall_source AS SELECT n AS id, 'name ' || n AS name FROM generate_series(1, 5000) n;
	SELECT doltgres_test_insert('tall', 'tall_source', false, 0);
	SELECT doltgres_test_fold('tall');
	SELECT doltgres_test_index_build('tall', 'tall_name');
});
is( $node->safe_psql('postgres', q{
	SELECT doltgres_test_index_tree_height('tall_empty'),
		doltgres_test_index_tree_height('tall_name') = doltgres_test_base_levels('tall_name'),
		doltgres_test_index_tree_height('tall_pkey') = doltgres_test_base_levels('tall'),
		doltgres_test_base_levels('tall_name') > 0, doltgres_test_base_levels('tall') > 0}),
	'0|t|t|t|t',
	"a fork btree index answers its base tree's levels above its leaves, a primary key's index its table's tree's, and 0 for an empty base"
);

# The btree's part 2, slice 6 (§3): a fork btree index scan, through the fork's
# index routine and the fork's table routine standing as the table's own, gives
# the rows heap's twin's nbtree index scan gives, in the same order, over a base
# a build made and a delta of later inserts, updates, and deletes: equality,
# range, IN-list (array keys), skip-scan (skip arrays), NULL, row comparison,
# and cross-type searches, forward and backward, of a two-column index, a
# descending one, and a primary key's.
$node->safe_psql('postgres', q{
	CREATE TABLE seek (id int PRIMARY KEY, a int, b text, c int8);
	CREATE INDEX seek_ab ON seek (a, b);
	CREATE INDEX seek_c ON seek (c DESC NULLS LAST);
	CREATE TABLE heap_seek (id int PRIMARY KEY, a int, b text, c int8);
	CREATE INDEX heap_seek_ab ON heap_seek (a, b);
	CREATE INDEX heap_seek_c ON heap_seek (c DESC NULLS LAST);
	CREATE TABLE seek_source AS SELECT n AS id,
		CASE WHEN n % 13 = 0 THEN NULL ELSE n % 10 END AS a,
		'b' || (n % 7) AS b,
		CASE WHEN n % 11 = 0 THEN NULL ELSE n * 3 END::int8 AS c
		FROM generate_series(1, 1500) n;
	INSERT INTO heap_seek SELECT * FROM seek_source;
	SELECT doltgres_test_insert('seek', 'seek_source', false, 0);
	SELECT doltgres_test_fold('seek');
	SELECT doltgres_test_index_build('seek', 'seek_ab');
	SELECT doltgres_test_index_build('seek', 'seek_c');
});
my $seek_routines = join('',
	map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;\n" }
	  qw(seek_ab seek_c seek_pkey));
for my $statement (
	q{INSERT INTO %T SELECT n, n % 9, ''b'' || (n % 5), n * 3 FROM generate_series(1501, 1600) n},
	q{UPDATE %T SET a = a + 1, b = b || ''x'' WHERE id % 17 = 0},
	q{DELETE FROM %T WHERE id % 19 = 0},
	q{UPDATE %T SET c = NULL WHERE id % 23 = 0})
{
	(my $heap = $statement) =~ s/%T/heap_seek/g;
	(my $fork = $statement) =~ s/%T/seek/g;
	$heap =~ s/''/'/g;
	$node->safe_psql('postgres', $heap);
	$node->safe_psql('postgres',
		"$seek_routines SELECT doltgres_test_write('seek', true, '$fork');");
}
my @searches = (
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a = 3 ORDER BY a, b},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a > 2 AND a <= 5 ORDER BY a, b},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a > 2 AND a <= 5 ORDER BY a DESC, b DESC},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a IN (1, 4, 7) AND b IN (''b2'', ''b5'', ''b6x'') ORDER BY a, b},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a IN (1, 4, 7) AND b IN (''b2'', ''b5'') ORDER BY a DESC, b DESC},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE b = ''b3'' ORDER BY a, b},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a > 6 AND b < ''b2'' ORDER BY a DESC, b DESC},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a IS NULL ORDER BY a, b},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE a IS NOT NULL AND b = ''b0'' ORDER BY a DESC, b DESC},
	q{SELECT coalesce(a::text, ''-'') || '':'' || b FROM %T WHERE (a, b) > (5, ''b3'') AND a < 7 ORDER BY a, b},
	q{SELECT id || '':'' || c FROM %T WHERE c > 4000 ORDER BY c DESC},
	q{SELECT id || '':'' || c FROM %T WHERE c < 300 ORDER BY c},
	q{SELECT id || '':'' || c FROM %T WHERE c = 1503::int4 OR c = 4500::int2 ORDER BY c},
	q{SELECT coalesce(c::text, ''-'') FROM %T WHERE c IS NULL ORDER BY c},
	q{SELECT id || '':'' || coalesce(a::text, ''-'') FROM %T WHERE id BETWEEN 100 AND 140 ORDER BY id DESC},
	q{SELECT id || '':'' || coalesce(a::text, ''-'') FROM %T WHERE id IN (5, 77, 1550, 1599) ORDER BY id});
my (@heap_rows, @fork_rows);
for my $search (@searches)
{
	(my $heap = $search) =~ s/%T/heap_seek/g;
	(my $fork = $search) =~ s/%T/seek/g;
	$heap =~ s/''/'/g;
	my $off = 'SET enable_seqscan = off; SET enable_bitmapscan = off;';
	my ($heap_set, $fork_set) = ($heap, $fork);
	for ($heap_set, $fork_set)
	{
		s/^SELECT .*? FROM /SELECT count(*) || ':' || coalesce(sum(id), 0) FROM /;
		s/ ORDER BY .*$//;
	}
	$fork_set =~ s/':'/'':''/;
	push @heap_rows, $node->safe_psql('postgres', "$off $heap; $heap_set");
	push @fork_rows,
	  $node->safe_psql('postgres',
		"$off $seek_routines SELECT doltgres_test_standing('seek', '$fork'); SELECT doltgres_test_standing('seek', '$fork_set');"
	  );
}
$node->safe_psql('postgres', 'SELECT pg_stat_force_next_flush()');
is( join("\n--\n", @fork_rows) . "\n"
	  . $node->safe_psql('postgres',
		q{SELECT string_agg(indexrelname || ' ' || (idx_scan > 0), ', ' ORDER BY indexrelname) FROM pg_stat_user_indexes WHERE relname = 'seek'}
	  ),
	join("\n--\n", @heap_rows)
	  . "\nseek_ab true, seek_c true, seek_pkey true",
	"a fork btree index scan gives heap's twin's rows for equality, range, IN-list, skip-scan, and NULL searches, forward and backward"
);

# The btree's part 2, slice 6 (§3): a fork btree index's bitmap scan adds each
# matching row's tid, a keyless table's copies each its own, and the bitmap
# heap scan over the fork's table gives heap's twin's rows.
$node->safe_psql('postgres', q{
	CREATE TABLE loose (a int, b text);
	CREATE INDEX loose_a ON loose (a);
	CREATE TABLE heap_loose (a int, b text);
	CREATE INDEX heap_loose_a ON heap_loose (a);
	CREATE TABLE loose_source AS SELECT n % 40 AS a, 'v' || (n % 3) AS b FROM generate_series(1, 600) n;
	INSERT INTO heap_loose SELECT * FROM loose_source;
	SELECT doltgres_test_insert('loose', 'loose_source', false, 0);
	SELECT doltgres_test_fold('loose');
	SELECT doltgres_test_index_build('loose', 'loose_a');
});
$node->safe_psql('postgres', q{INSERT INTO heap_loose VALUES (7, 'late'), (7, 'late'), (41, 'new')});
$node->safe_psql('postgres',
	q{SELECT doltgres_test_index_routine('loose_a');
	SELECT doltgres_test_write('loose', true, 'INSERT INTO loose VALUES (7, ''late''), (7, ''late''), (41, ''new'')');});
my $bitmap = 'SET enable_seqscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = off;';
my %bitmapped;
for my $table ('loose', 'heap_loose', 'seek', 'heap_seek')
{
	my $query =
	  $table =~ /loose/
	  ? "SELECT count(*) || '':'' || string_agg(a || b, '','' ORDER BY a, b) FROM $table WHERE a IN (7, 13, 41) OR a < 2"
	  : "SELECT count(*) || '':'' || string_agg(id::text, '','' ORDER BY id) FROM $table WHERE (a = 4 AND b > ''b3'') OR c < 90";
	my $routines =
	  $table =~ /loose/
	  ? "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('loose_a'); END \$\$;"
	  : $seek_routines;
	(my $plain = $query) =~ s/''/'/g;
	$bitmapped{$table} =
	  $table =~ /^heap_/
	  ? $node->safe_psql('postgres', "$bitmap $plain")
	  : $node->safe_psql('postgres',
		"$bitmap $routines SELECT doltgres_test_standing('$table', '$query');");
}
is( "$bitmapped{loose} | $bitmapped{seek}",
	"$bitmapped{heap_loose} | $bitmapped{heap_seek}",
	"a fork btree index's bitmap scan over a keyed and a keyless table gives heap's twin's rows"
);

# The btree's part 2, slice 6 (§3): a merge join whose inner side is a fork
# btree index scan marks and restores its place across each run of equal keys,
# and gives heap's twin's rows.
my $merge =
  'SET enable_hashjoin = off; SET enable_nestloop = off; SET enable_material = off; SET enable_seqscan = off; SET enable_bitmapscan = off;';
my $join =
  q{SELECT count(*) || '':'' || sum(s.id::int8 * t.id) FROM %T s JOIN %T t ON s.a = t.a AND s.b = t.b WHERE s.id < 400};
(my $heap_join = $join) =~ s/%T/heap_seek/g;
$heap_join =~ s/''/'/g;
(my $fork_join = $join) =~ s/%T/seek/g;
is( $node->safe_psql('postgres',
		"$merge $seek_routines SELECT doltgres_test_standing('seek', '$fork_join');"),
	$node->safe_psql('postgres', "$merge $heap_join"),
	"a merge join's mark and restore over a fork btree index gives heap's twin's rows");

# The btree's part 2, slice 6 (§3, PLAN item 16): a fork btree index's cost
# estimate is nbtree's over the pages and height the fork's call sites give,
# with a primary key's index's correlation 1, its entries being its table's
# rows in their tree's order; an index on the same column created before the
# primary key, with no statistics, estimates correlation 0, so the planner
# takes the primary key's index, which it wouldn't on a tie.
$node->safe_psql('postgres', q{
	CREATE TABLE corr (k int, v text);
	CREATE INDEX corr_k ON corr (k);
	ALTER TABLE corr ADD PRIMARY KEY (k);
	CREATE TABLE corr_source AS SELECT n AS k, 'v' || n AS v FROM generate_series(1, 3000) n ORDER BY md5(n::text);
	SELECT doltgres_test_insert('corr', 'corr_source', false, 0);
	SELECT doltgres_test_fold('corr');
	SELECT doltgres_test_index_build('corr', 'corr_k');
	CREATE FUNCTION explained_index(query text) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		plan json;
	BEGIN
		EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
		RETURN plan->0->'Plan'->>'Index Name';
	END $$;
});
is( $node->safe_psql('postgres', q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	DO $$ BEGIN PERFORM doltgres_test_index_routine('corr_k'); PERFORM doltgres_test_index_routine('corr_pkey'); END $$;
	SELECT doltgres_test_standing('corr', 'SELECT explained_index(''SELECT v FROM corr WHERE k BETWEEN 10 AND 900'')');
}),
	'corr_pkey',
	"a fork btree index's cost estimate is nbtree's over the fork's pages and height, with a primary key's index's correlation 1"
);

# The btree's part 2, slice 6 (§3) and §4: a fork btree index whose keys are
# cut at its bound reads a run of cut keys, its entries sharing their cut
# column's first bytes, whole and sorted by their columns' whole encodings, so a
# scan gives the rows in heap's twin's order forward and backward, and a merge
# join's mark and restore inside the run give heap's twin's rows; a parallel
# scan's units never end inside the run, so each participant reads its rows in
# the index's order and every row is read once.
$node->safe_psql('postgres', q{
	CREATE TABLE longs (id int PRIMARY KEY, t text);
	CREATE INDEX longs_t ON longs (t);
	CREATE TABLE heap_longs (id int PRIMARY KEY, t text);
	CREATE TABLE longs_source AS SELECT n AS id,
		CASE WHEN n % 3 = 0 THEN 'a' || lpad(n::text, 4, '0') ELSE repeat('p', 20000) || md5(n::text) END AS t
		FROM generate_series(1, 300) n;
	INSERT INTO heap_longs SELECT * FROM longs_source;
	SELECT doltgres_test_insert('longs', 'longs_source', false, 0);
	SELECT doltgres_test_fold('longs');
	SELECT doltgres_test_index_build('longs', 'longs_t');
});
my $longs_routine =
  "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('longs_t'); END \$\$;";
my $ordered = 'SET enable_seqscan = off; SET enable_bitmapscan = off;';
my %runs;
for my $way ('', ' DESC')
{
	$runs{"fork$way"} = $node->safe_psql('postgres',
		"$ordered $longs_routine SELECT doltgres_test_standing('longs', 'SELECT id FROM longs ORDER BY t$way');");
	$runs{"heap$way"} =
	  $node->safe_psql('postgres', "SELECT id FROM heap_longs ORDER BY t$way");
}
my $run_join =
  q{SELECT count(*) || ':' || sum(s.id::int8 * t.id) FROM %T s JOIN %T t ON s.t = t.t};
(my $heap_run_join = $run_join) =~ s/%T/heap_longs/g;
(my $fork_run_join = $run_join) =~ s/%T/longs/g;
$fork_run_join =~ s/'/''/g;
$runs{fork_join} = $node->safe_psql('postgres',
	"$merge $longs_routine SELECT doltgres_test_standing('longs', '$fork_run_join');");
$runs{heap_join} = $node->safe_psql('postgres', $heap_run_join);
is( "$runs{fork}\n--\n$runs{'fork DESC'}\n--\n$runs{fork_join}",
	"$runs{heap}\n--\n$runs{'heap DESC'}\n--\n$runs{heap_join}",
	"a fork btree index's run of cut keys reads in heap's twin's order forward, backward, and across a merge join's mark and restore"
);

# parallel_ordered returns whether a parallel scan's participants' rows, each
# line its participant's number and the row's id, read every id of an order
# once, each participant's in that order.
sub parallel_ordered
{
	my ($read, $order) = @_;
	my @order = split /\n/, $order;
	my %at = map { $order[$_] => $_ } 0 .. $#order;
	my (%seen, %last);
	for my $line (split /\n/, $read)
	{
		my ($participant, $id) = split /:/, $line;
		return 0 if $seen{$id}++ || !defined $at{$id};
		return 0 if defined $last{$participant} && $at{$id} <= $last{$participant};
		$last{$participant} = $at{$id};
	}
	return scalar(keys %seen) == scalar(@order) && scalar(keys %last) > 1;
}
my ($forward, $backward) = map {
	$node->safe_psql('postgres',
		"$longs_routine SELECT doltgres_test_index_parallel('longs', 'longs_t', 3, $_)")
} ('false', 'true');
is( parallel_ordered($forward, $runs{heap}) . parallel_ordered($backward, $runs{'heap DESC'}),
	'11',
	"a parallel scan of a fork btree gives each of heap's twin's rows once, each participant's in the index's order, with no unit ending inside a run of cut keys"
);

# The btree's part 2, slice 6 (CommPF 9's ruling (a)): a deferrable unique index
# of a fork table, other than its primary key's, is the fork's, its entries
# ending with their rows' keys so equal keys stand until the check; an update
# that swaps two keys passes the statement's recheck, an insert of a key a live
# row holds is refused at the statement's end with nbtree's 23505 and words,
# and NULLs never collide, as on heap's twin.
$node->safe_psql('postgres', q{
	CREATE TABLE later (id int PRIMARY KEY, v text UNIQUE DEFERRABLE);
	CREATE TABLE heap_later (id int PRIMARY KEY, v text UNIQUE DEFERRABLE);
});
my %later;
for my $table ('later', 'heap_later')
{
	my $routines =
	  join('',
		map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" }
		  ('later_v_key', 'later_pkey'));
	my @said;
	for my $statement (
		q{INSERT INTO %T VALUES (1, 'a'), (2, 'b')},
		q{UPDATE %T SET v = CASE v WHEN 'a' THEN 'b' ELSE 'a' END},
		q{INSERT INTO %T VALUES (3, 'a')},
		q{INSERT INTO %T VALUES (4, NULL), (5, NULL)},
		q{SELECT string_agg(id || ':' || coalesce(v, '-'), ' ' ORDER BY id) FROM %T})
	{
		(my $sql = $statement) =~ s/%T/$table/g;
		if ($table eq 'later')
		{
			$sql =~ s/'/''/g;
			$sql =
			  $sql =~ /^SELECT/
			  ? "$routines SELECT doltgres_test_standing('later', '$sql')"
			  : "$routines SELECT doltgres_test_write('later', true, '$sql')";
		}
		my ($code, $out, $err) = $node->psql('postgres', $sql);
		$err =~ s/^psql:<stdin>:\d+: //mg;
		$err =~ s/^CONTEXT:.*\n?//mg;
		$err =~ s/heap_later/later/g;
		$err =~ s/\s+$//;
		push @said, $sql =~ /string_agg/ ? $out : $err;
	}
	$later{$table} = join("\n--\n", @said);
}
is($later{later}, $later{heap_later},
	"a deferrable unique fork btree index refuses a live duplicate at the statement's recheck with heap's twin's 23505 and lets a swap of two keys and repeated NULLs stand"
);

# Design item 4 and the btree's part 1 and part 2: a fork btree index's routine,
# read member by member against bthandler's, replaces its eighteen members of
# the fork's storage, keeps the eight others, sets ampredlocks false until item
# 7's leaf page locks, and keeps every other flag and number.
is( $node->safe_psql('postgres', q{SELECT doltgres_test_index_members('seek_ab')}),
	'replaced=ambuild,ambuildempty,aminsert,aminsertcleanup,ambulkdelete,amvacuumcleanup,amcostestimate,amgettreeheight,ambeginscan,amrescan,amgettuple,amgetbitmap,amendscan,ammarkpos,amrestrpos,amestimateparallelscan,aminitparallelscan,amparallelrescan kept=amcanreturn,amoptions,amproperty,ambuildphasename,amvalidate,amadjustmembers,amtranslatestrategy,amtranslatecmptype ampredlocks=f same=t',
	"a fork btree index's routine replaces bthandler's eighteen members of the fork's storage, keeps its eight others, sets ampredlocks false, and keeps every other flag and number"
);

# An expression index whose function is wrongly marked IMMUTABLE would compute a
# delete's old entry differently from the entry the insert wrote (item 4's
# I15f); the delete evaluates no expression (CommPF 10's no-user-code removal)
# and removes the entry the insert wrote, which the row's record names, so the
# index stays exact, with no warning, and the row is removed.
$node->safe_psql('postgres', q{
	CREATE TABLE shift (n int);
	INSERT INTO shift VALUES (0);
	CREATE FUNCTION shifty(int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT $1 + (SELECT n FROM public.shift)';
	CREATE TABLE shifted (id int PRIMARY KEY, v int);
	CREATE INDEX shifted_v ON shifted (shifty(v));
});
my $shifted_routines =
  "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('shifted_v'); PERFORM doltgres_test_index_routine('shifted_pkey'); END \$\$;";
$node->safe_psql('postgres',
	"$shifted_routines SELECT doltgres_test_write('shifted', true, 'INSERT INTO shifted VALUES (1, 5), (2, 6)');");
$node->safe_psql('postgres', 'UPDATE shift SET n = 1');
my ($shifted_code, $shifted_out, $shifted_err) = $node->psql('postgres',
	"$shifted_routines SELECT doltgres_test_write('shifted', true, 'DELETE FROM shifted WHERE id = 1');");
is( join(' | ',
		$shifted_err =~ /(WARNING:[^\n]*)/,
		$node->safe_psql('postgres',
			"$shifted_routines SELECT doltgres_test_standing('shifted', 'SELECT string_agg(id::text, '','' ORDER BY id) FROM shifted')"
		),
		entries('shifted', undef, 'shifted_v')),
	"2 | delete, put\nput\ndelete, put\nput",
	"a fork table's delete whose expression a function wrongly marked IMMUTABLE would compute differently removes the row and the entry its insert wrote, by its record, with no warning"
);

# The btree's part 2, slice 6 (§3): WHERE CURRENT OF a cursor over an
# index-only scan of a fork btree index updates the row the cursor is on, its
# tid the version the scan's snapshot sees (execCurrent's call site), as on
# heap's twin.
$node->safe_psql('postgres', q{
	CREATE FUNCTION moved_current(target regclass) RETURNS text LANGUAGE plpgsql AS $$
	DECLARE
		cur refcursor := 'cur';
		found int;
	BEGIN
		SET LOCAL enable_seqscan = off;
		SET LOCAL enable_bitmapscan = off;
		OPEN cur FOR EXECUTE format('SELECT a FROM %s WHERE a = 4 ORDER BY a', target);
		FETCH cur INTO found;
		EXECUTE format('UPDATE %s SET b = ''current'' WHERE CURRENT OF cur', target);
		CLOSE cur;
		RETURN found;
	END $$;
});
$node->safe_psql('postgres', q{SELECT moved_current('heap_seek')});
$node->safe_psql('postgres',
	"$seek_routines SELECT doltgres_test_write('seek', true, 'SELECT moved_current(''seek'')');");
is( $node->safe_psql('postgres',
		"$seek_routines SELECT doltgres_test_standing('seek', 'SELECT count(*) || '':'' || min(a) FROM seek WHERE b = ''current''')"),
	$node->safe_psql('postgres',
		q{SELECT count(*) || ':' || min(a) FROM heap_seek WHERE b = 'current'}),
	"WHERE CURRENT OF on an index-only scan of a fork btree updates the row heap's twin's does"
);

# The btree text's section 6 (part 1's B2a and B2b), read through slice 6's
# scans: an index-only scan by a REPEATABLE READ transaction opened before an
# insert that precedes the build doesn't return that row, as heap's twin's
# doesn't (B2b); and a later transaction finds by the index the row a build in
# a REPEATABLE READ transaction gave an entry though the build's snapshot
# predates the row (B2a).
$node->safe_psql('postgres', q{
	CREATE TABLE early (id int PRIMARY KEY, v int);
	CREATE INDEX early_v ON early (v);
	CREATE INDEX early_w ON early (v);
	CREATE TABLE heap_early (id int PRIMARY KEY, v int);
	CREATE INDEX heap_early_v ON heap_early (v);
});
my $early_routines = join('',
	map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" }
	  ('early_v', 'early_w', 'early_pkey'));
$node->safe_psql('postgres',
	"$early_routines SELECT doltgres_test_write('early', true, 'INSERT INTO early VALUES (1, 10), (2, 20)');");
$node->safe_psql('postgres', 'INSERT INTO heap_early VALUES (1, 10), (2, 20)');
my $early_reader = $node->background_psql('postgres');
$early_reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$early_reader->query_safe('SELECT count(*) FROM pg_class');
$node->safe_psql('postgres',
	"$early_routines SELECT doltgres_test_write('early', true, 'INSERT INTO early VALUES (3, 30)');");
$node->safe_psql('postgres', 'INSERT INTO heap_early VALUES (3, 30)');
my $early_builder = $node->background_psql('postgres');
$early_builder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$early_builder->query_safe('SELECT count(*) FROM pg_class');
$node->safe_psql('postgres',
	"$early_routines SELECT doltgres_test_write('early', true, 'INSERT INTO early VALUES (4, 40)');");
$node->safe_psql('postgres', 'SELECT doltgres_test_fold(\'early\')');
$node->safe_psql('postgres', q{SELECT doltgres_test_index_build('early', 'early_v')});
$early_builder->query_safe(q{SELECT doltgres_test_index_build('early', 'early_w')});
$early_builder->query_safe('COMMIT');
my $only = 'SET enable_seqscan = off; SET enable_bitmapscan = off;';
my $early_fork = $early_reader->query_safe("$only $early_routines SELECT doltgres_test_standing('early', 'SELECT string_agg(v::text, '','' ORDER BY v) FROM early WHERE v > 0')");
my $early_heap = $early_reader->query_safe("$only SELECT string_agg(v::text, ',' ORDER BY v) FROM heap_early WHERE v > 0");
$early_reader->quit;
$early_builder->quit;
is( "$early_fork | "
	  . $node->safe_psql('postgres',
		"$only $early_routines DROP INDEX early_v; SELECT doltgres_test_standing('early', 'SELECT id FROM early WHERE v = 40')"),
	"$early_heap | 4",
	"an index-only scan by a REPEATABLE READ transaction older than a fork btree's build gives heap's twin's rows (B2b), and a later transaction finds by an index a REPEATABLE READ build made the row its snapshot predates (B2a)"
);

# The btree's part 2, slice 6 (§3): an index-only scan of a fork btree whose
# entries are long, each cut in its key with its original out of line, returns
# each row's whole value from the entry, in heap's twin's order.
is( $node->safe_psql('postgres',
		"$ordered $longs_routine SELECT doltgres_test_standing('longs', 'SELECT md5(t) FROM longs WHERE t > ''b'' ORDER BY t')"
	),
	$node->safe_psql('postgres', q{SELECT md5(t) FROM heap_longs WHERE t > 'b' ORDER BY t}),
	"an index-only scan of a fork btree's long entries gives heap's twin's whole values in its order");

# The btree's part 2, slice 6 (the keyed index's index-only scan): an index-only
# scan of a primary key's index, which reads its table's rows, whose long text
# key column is cut past the table's bound with its value out of line, returns
# the whole value and the key columns after it, as heap's twin's does.
$node->safe_psql('postgres', q{
	CREATE TABLE pkl (t text, k int, v int, PRIMARY KEY (t, k));
	CREATE TABLE heap_pkl (t text, k int, v int, PRIMARY KEY (t, k));
	CREATE TABLE pkl_rows AS SELECT repeat('L', 12000) || (g % 4) AS t, g AS k, g * 3 AS v
	FROM generate_series(1, 24) g;
	INSERT INTO heap_pkl SELECT * FROM pkl_rows;
});
my $pkl_routine = "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('pkl_pkey'); END \$\$;";
$node->safe_psql('postgres',
	"$pkl_routine SELECT doltgres_test_write('pkl', true, 'INSERT INTO pkl SELECT * FROM pkl_rows');");
my $pkl_query = q{SELECT length(t) || ':' || md5(t) || ':' || k FROM %s WHERE t > 'L' ORDER BY t, k};
is( $node->safe_psql('postgres',
		"$ordered $pkl_routine SELECT doltgres_test_standing('pkl', '" . (sprintf($pkl_query, 'pkl') =~ s/'/''/gr) . "')"),
	$node->safe_psql('postgres', $ordered . sprintf($pkl_query, 'heap_pkl')),
	"an index-only scan of a primary key's index whose long key column is out of line gives heap's twin's whole values and the columns after it"
);

# The btree's part 2, slice 6, with the fold text's draft 12: a unique index's
# build over a key whose repeat it drops, a dead row by a writer an older
# REPEATABLE READ snapshot counts as running, leaves that writer out of its
# base's maxFolded as it leaves out the row, so the older snapshot reads the key
# through the index with no error, as heap's twin's.
for my $table ('passed', 'heap_passed')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres', "CREATE TABLE $table (id int PRIMARY KEY, k int)");
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (1, 10)')");
}
my $passed_older = $node->background_psql('postgres', on_error_stop => 0);
$passed_older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
for my $table ('passed', 'heap_passed')
{
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('$table', $fork, 'INSERT INTO $table VALUES (2, 10)'); "
		  . "SELECT doltgres_test_write('$table', $fork, 'DELETE FROM $table WHERE id + 0 = 2')");
	$node->safe_psql('postgres',
		"CREATE UNIQUE INDEX ${table}_k ON $table (k)"
		  . ($fork eq 'true' ? "; SELECT doltgres_test_index_build('$table', '${table}_k')" : ''));
}
my $passed_off = 'SET enable_seqscan = off; SET enable_bitmapscan = off;';
my $passed_routines = join('',
	map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" } 'passed_k',
	'passed_pkey');
my $passed_read = q{SELECT string_agg(id::text, ' ' ORDER BY id) FROM %s WHERE k = 10};
my $passed_fork = $passed_older->query("$passed_off $passed_routines SELECT doltgres_test_standing('passed', '"
	  . (sprintf($passed_read, 'passed') =~ s/'/''/gr) . "')");
my $passed_heap = $passed_older->query("$passed_off " . sprintf($passed_read, 'heap_passed'));
is($passed_fork, $passed_heap,
	"an older snapshot reads a unique index whose build dropped a repeat by a writer it counts as running as heap's twin"
);
unlike($passed_older->{stderr}, qr/ERROR/, 'the older snapshot reads the index with no error');
$passed_older->query_safe('COMMIT');
$passed_older->quit;
# The btree's part 2 (pending line 51; the owner's decision of 2026-11-16): until
# item 7's leaf page locks, a fork btree's routine has ampredlocks false, so
# Postgres locks a serializable scan's whole index and checks it at each insert,
# and a write skew through a fork btree fails one transaction with 40001 at the
# step where heap's twin's does through nbtree, over keys its rows hold and over
# empty key ranges, a phantom only the index's lock sees, since no read row is
# locked.
$node->safe_psql('postgres', q{
	CREATE TABLE skew (k int, v int);
	CREATE INDEX skew_k ON skew (k);
	CREATE TABLE heap_skew (k int, v int);
	CREATE INDEX heap_skew_k ON heap_skew (k);
	INSERT INTO heap_skew VALUES (1, 1), (2, 2);
});
my $skew_routine = "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('skew_k'); END \$\$;";
$node->safe_psql('postgres',
	"$skew_routine SELECT doltgres_test_write('skew', true, 'INSERT INTO skew VALUES (1, 1), (2, 2)');");
my %skewed;
for my $run (map { my $base = $_; map { [ $_, $base ] } ('skew', 'heap_skew') } (0, 4))
{
	my ($table, $base) = @$run;
	my $fork = $table eq 'skew';
	my @sessions = map {
		$node->background_psql('postgres', on_error_stop => 0, extra_params => [ '-v', 'VERBOSITY=verbose' ])
	} 0 .. 1;
	my @steps;
	for my $i (0, 1)
	{
		push @steps, [ $i, 'BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off;'
			  . ' SET LOCAL enable_bitmapscan = off' ];
	}
	for my $i (0, 1)
	{
		my $read = 'SELECT count(*) FROM ' . $table . ' WHERE k = ' . ($base + $i + 1);
		push @steps, [ $i, $fork ? "$skew_routine SELECT doltgres_test_standing('skew', '$read')" : $read ];
	}
	for my $i (0, 1)
	{
		my $write = "INSERT INTO $table VALUES (" . ($base + 2 - $i) . ', 0)';
		push @steps, [ $i, $fork ? "$skew_routine DO \$\$ BEGIN PERFORM doltgres_test_write('skew', true, '$write'); END \$\$;" : $write ];
	}
	push @steps, [ $_, 'COMMIT' ] for (0, 1);
	my @said;
	for my $step (0 .. $#steps)
	{
		my ($i, $sql) = @{ $steps[$step] };
		$sessions[$i]->{stderr} = '';
		my $output = $sessions[$i]->query($sql);
		push @said, "$step:$i:" . ($sessions[$i]->{stderr} =~ /ERROR:  (\w+):/ ? $1 : $output);
	}
	$_->quit for @sessions;
	$skewed{"$table$base"} = join("\n", @said);
}
like($skewed{skew0}, qr/40001/, "a write skew through a fork btree fails a transaction with 40001");
is($skewed{skew0}, $skewed{heap_skew0},
	"a write skew through a fork btree fails the transaction and step heap's twin's write skew through nbtree does");
like($skewed{skew4}, qr/40001/,
	"a write skew over empty key ranges through a fork btree, which only the index's lock sees, fails a transaction with 40001"
);
is($skewed{skew4}, $skewed{heap_skew4},
	"a write skew over empty key ranges through a fork btree fails the transaction and step heap's twin's does through nbtree"
);

# The comparator btrees (design item 4's long keys; PLAN's comparator column past
# the bound): a fork btree index over citext, a type whose class compares by a
# function, gives heap's twin's rows in its order forward and backward, over a
# range, and for an equality probe, its values past the bound kept out of line and
# read by their blobs' names; an index of (long text, citext) orders the citext by
# its class among rows that share a long text past the bound, which in a
# comparator tree is out of line too, so its scan sorts no run of cut keys, as
# trace_sort shows, where a byte-ordered index of the same long texts sorts one;
# an index whose column's class isn't its type's default orders by the class; and
# a unique citext index refuses two keys its class calls equal whose bytes differ,
# with heap's twin's 23505.
$node->safe_psql('postgres', q{
	CREATE EXTENSION citext;
	CREATE TABLE cmp (id int PRIMARY KEY, c citext, t text, i int4);
	CREATE INDEX cmp_c ON cmp (c);
	CREATE INDEX cmp_tc ON cmp (t, c);
	CREATE INDEX cmp_i ON cmp (i reversed_ops);
	CREATE TABLE heap_cmp (id int PRIMARY KEY, c citext, t text, i int4);
	CREATE INDEX heap_cmp_c ON heap_cmp (c);
	CREATE INDEX heap_cmp_tc ON heap_cmp (t, c);
	CREATE INDEX heap_cmp_i ON heap_cmp (i reversed_ops);
	CREATE TABLE cmp_bytes (id int PRIMARY KEY, t text);
	CREATE INDEX cmp_bytes_t ON cmp_bytes (t);
	CREATE TABLE cmp_rows AS SELECT g AS id,
		CASE WHEN g % 7 = 0 THEN repeat(CASE WHEN g % 2 = 0 THEN 'Q' ELSE 'q' END, 20000) || g
			ELSE chr(CASE WHEN g % 2 = 0 THEN 65 ELSE 97 END + g % 26) || g END AS c,
		CASE WHEN g % 5 = 0 THEN repeat('p', 20000) || (g % 3) ELSE 'v' || g END AS t,
		g * 7 - 300 AS i
	FROM generate_series(1, 120) g;
	INSERT INTO heap_cmp SELECT * FROM cmp_rows;
	CREATE TABLE cmpu (id int PRIMARY KEY, c citext);
	CREATE UNIQUE INDEX cmpu_c ON cmpu (c);
	CREATE TABLE heap_cmpu (id int PRIMARY KEY, c citext);
	CREATE UNIQUE INDEX heap_cmpu_c ON heap_cmpu (c);
	INSERT INTO heap_cmpu VALUES (1, 'Abc');
});
my $cmp_routines = join('',
	map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" }
	  ('cmp_c', 'cmp_tc', 'cmp_i', 'cmp_pkey', 'cmp_bytes_t', 'cmp_bytes_pkey', 'cmpu_c', 'cmpu_pkey'));
$node->safe_psql('postgres',
	"$cmp_routines SELECT doltgres_test_write('cmp', true, 'INSERT INTO cmp SELECT * FROM cmp_rows');"
	  . " SELECT doltgres_test_write('cmp_bytes', true, 'INSERT INTO cmp_bytes SELECT id, t FROM cmp_rows');"
	  . " SELECT doltgres_test_write('cmpu', true, 'INSERT INTO cmpu VALUES (1, ''Abc'')');");
my $cmp_only = 'SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;';
my @cmp_queries = (
	'SELECT id FROM %s ORDER BY c',
	'SELECT id FROM %s ORDER BY c DESC',
	q{SELECT id FROM %s WHERE c BETWEEN 'c' AND 'M' ORDER BY c},
	q{SELECT id FROM %s WHERE c = repeat('q', 20000) || '14'},
	'SELECT id FROM %s ORDER BY t, c',
	q{SELECT id FROM %s WHERE t = repeat('p', 20000) || '1' ORDER BY t, c DESC});
for my $query (@cmp_queries)
{
	my $fork = sprintf($query, 'cmp') =~ s/'/''/gr;
	is( $node->safe_psql('postgres',
			"$cmp_only $cmp_routines SELECT doltgres_test_standing('cmp', '$fork')"),
		$node->safe_psql('postgres', $cmp_only . sprintf($query, 'heap_cmp')),
		"a fork btree index over a column compared through its class gives heap's twin's rows in its order, its values past the bound out of line included: "
		  . sprintf($query, 'cmp') =~ s/repeat\('(.)', \d+\)/repeat($1)/gr);
}
is( $node->safe_psql('postgres',
		"$cmp_only $cmp_routines SELECT doltgres_test_standing('cmp', 'SELECT id FROM cmp ORDER BY i USING >')"),
	$node->safe_psql('postgres', "$cmp_only SELECT id FROM heap_cmp ORDER BY i USING >"),
	"a fork btree index whose column's operator class isn't its type's default orders by the class, as heap's twin's does"
);
my $cmp_sorts = sub {
	my ($table, $query) = @_;
	my ($ret, $out, $err) = $node->psql('postgres',
		"SET client_min_messages = log; SET trace_sort = on; $cmp_only $cmp_routines "
		  . "SELECT count(*) FROM doltgres_test_standing('$table', '$query')");
	return $err =~ /begin datum sort/ ? 'sorted' : 'unsorted';
};
is( join(' ',
		$cmp_sorts->('cmp', 'SELECT id FROM cmp ORDER BY t, c'),
		$cmp_sorts->('cmp_bytes', 'SELECT id FROM cmp_bytes ORDER BY t')),
	'unsorted sorted',
	"a comparator tree's scan sorts no run of cut keys, which a byte-ordered tree's of the same long texts sorts");
my %cmp_refused;
for my $table ('cmpu', 'heap_cmpu')
{
	my $insert = "INSERT INTO $table VALUES (2, 'aBC')";
	my $sql = $table eq 'cmpu'
	  ? "$cmp_routines SELECT doltgres_test_write('cmpu', true, '" . ($insert =~ s/'/''/gr) . "')"
	  : $insert;
	my ($ret, $out, $err) = $node->psql('postgres', $sql, extra_params => [ '-v', 'VERBOSITY=verbose' ]);
	($cmp_refused{$table}) = $err =~ /(ERROR:.*?\n.*?DETAIL:[^\n]*)/s;
	$cmp_refused{$table} =~ s/heap_// if defined $cmp_refused{$table};
}
like($cmp_refused{cmpu} // '', qr/23505/, 'a unique citext fork btree index refuses a key its class calls equal');
is($cmp_refused{cmpu}, $cmp_refused{heap_cmpu},
	"a unique citext fork btree index refuses a key its class calls equal whose bytes differ with heap's twin's 23505");

# The comparator btrees' build (the btree text's section 6): an index built over a
# fork table's rows of a column compared through its class holds them in its
# class's order, its scans giving heap's twin's rows, and again after more rows and
# a fold of its storage; a unique one refuses two rows its class calls equal with
# heap's twin's 23505. A table keyed by (long text, citext) reads in its key's
# order through its primary key's index, its long text out of line in its key;
# and an index of (long text under an ICU collation, citext) orders the long texts
# as the collation does.
$node->safe_psql('postgres', q{
	DROP INDEX cmp_c, cmp_tc;
	CREATE INDEX cmp_cb ON cmp (c);
	CREATE INDEX cmp_tcb ON cmp (t, c);
	DROP INDEX heap_cmp_c, heap_cmp_tc;
	CREATE INDEX heap_cmp_cb ON heap_cmp (c);
	CREATE INDEX heap_cmp_tcb ON heap_cmp (t, c);
	CREATE TABLE cmpb (id int PRIMARY KEY, c citext);
	CREATE TABLE heap_cmpb (id int PRIMARY KEY, c citext);
	INSERT INTO heap_cmpb VALUES (1, 'Abc'), (2, 'aBC');
	CREATE TABLE cmpk (t text, c citext, PRIMARY KEY (t, c));
	CREATE TABLE heap_cmpk (t text, c citext, PRIMARY KEY (t, c));
	CREATE TABLE cmpk_rows AS SELECT repeat('k', 12000) || (g % 3) AS t,
		chr(CASE WHEN g % 2 = 0 THEN 65 ELSE 97 END + g % 26) || g AS c
	FROM generate_series(1, 40) g;
	INSERT INTO heap_cmpk SELECT * FROM cmpk_rows;
	CREATE TABLE cmpl (id int PRIMARY KEY, u text COLLATE "und-x-icu", c citext);
	CREATE INDEX cmpl_uc ON cmpl (u, c);
	CREATE TABLE heap_cmpl (id int PRIMARY KEY, u text COLLATE "und-x-icu", c citext);
	CREATE INDEX heap_cmpl_uc ON heap_cmpl (u, c);
	CREATE TABLE cmpl_rows AS SELECT g AS id,
		repeat('a', 20000) || (ARRAY['B', 'b', 'á', 'A', 'c', 'ä'])[g % 6 + 1] AS u,
		chr(CASE WHEN g % 2 = 0 THEN 65 ELSE 97 END + g % 26) || g AS c
	FROM generate_series(1, 36) g;
	INSERT INTO heap_cmpl SELECT * FROM cmpl_rows;
});
my $cmpb_routines = join('',
	map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" }
	  ('cmp_cb', 'cmp_tcb', 'cmp_i', 'cmp_pkey', 'cmpb_pkey', 'cmpk_pkey', 'cmpl_uc', 'cmpl_pkey'));
$node->safe_psql('postgres',
	"$cmpb_routines SELECT doltgres_test_index_build('cmp', 'cmp_cb'); SELECT doltgres_test_index_build('cmp', 'cmp_tcb');"
	  . " SELECT doltgres_test_write('cmpb', true, 'INSERT INTO cmpb VALUES (1, ''Abc''), (2, ''aBC'')');"
	  . " SELECT doltgres_test_write('cmpk', true, 'INSERT INTO cmpk SELECT * FROM cmpk_rows');"
	  . " SELECT doltgres_test_write('cmpl', true, 'INSERT INTO cmpl SELECT * FROM cmpl_rows');");
my $cmp_all = sub {
	my ($fork_table, $heap_table, @queries) = @_;
	my (@fork, @heap);
	for my $query (@queries)
	{
		my $fork = sprintf($query, $fork_table) =~ s/'/''/gr;
		push @fork, $node->safe_psql('postgres',
			"$cmp_only $cmpb_routines SELECT doltgres_test_standing('$fork_table', '$fork')");
		push @heap, $node->safe_psql('postgres', $cmp_only . sprintf($query, $heap_table));
	}
	return (join("\n--\n", @fork), join("\n--\n", @heap));
};
my ($cmp_built, $heap_built) = $cmp_all->('cmp', 'heap_cmp', @cmp_queries);
is($cmp_built, $heap_built,
	"an index built over a table's rows of a column compared through its class holds them in its class's order, and a unique one refuses two its class calls equal"
);
$node->safe_psql('postgres', q{
	CREATE TABLE cmp_more AS SELECT g AS id,
		CASE WHEN g % 7 = 0 THEN repeat('q', 20000) || g ELSE chr(97 + g % 26) || g END AS c,
		'w' || g AS t, g * 7 - 300 AS i
	FROM generate_series(121, 160) g;
	INSERT INTO heap_cmp SELECT * FROM cmp_more;
});
$node->safe_psql('postgres',
	"$cmpb_routines SELECT doltgres_test_write('cmp', true, 'INSERT INTO cmp SELECT * FROM cmp_more');"
	  . " SELECT length(doltgres_test_index_fold('cmp_cb')) > 0;"
	  . " SELECT length(doltgres_test_index_fold('cmp_tcb')) > 0;");
my ($cmp_folded, $heap_folded) = $cmp_all->('cmp', 'heap_cmp', @cmp_queries);
is($cmp_folded, $heap_folded,
	"a comparator tree's fold keeps its class's order, its scans giving heap's twin's rows");
my %cmpb_refused;
for my $table ('cmpb', 'heap_cmpb')
{
	my $sql = $table eq 'cmpb'
	  ? "CREATE UNIQUE INDEX cmpb_c ON cmpb (c); $cmpb_routines"
	  . " DO \$\$ BEGIN PERFORM doltgres_test_index_routine('cmpb_c'); END \$\$;"
	  . " SELECT doltgres_test_index_build('cmpb', 'cmpb_c')"
	  : "CREATE UNIQUE INDEX heap_cmpb_c ON heap_cmpb (c)";
	my ($ret, $out, $err) = $node->psql('postgres', $sql, extra_params => [ '-v', 'VERBOSITY=verbose' ]);
	($cmpb_refused{$table}) = $err =~ /(ERROR:.*?\n.*?DETAIL:[^\n]*)/s;
	$cmpb_refused{$table} =~ s/heap_// if defined $cmpb_refused{$table};
}
like($cmpb_refused{cmpb} // '', qr/23505/, "a unique comparator index's build refuses two rows its class calls equal");
is($cmpb_refused{cmpb}, $cmpb_refused{heap_cmpb},
	"a unique comparator index's build refuses two rows its class calls equal with heap's twin's 23505");
my ($cmpk_read, $heap_cmpk_read) = $cmp_all->('cmpk', 'heap_cmpk',
	'SELECT c FROM %s ORDER BY t, c', 'SELECT c FROM %s ORDER BY t DESC, c DESC',
	q{SELECT c FROM %s WHERE t = repeat('k', 12000) || '1' AND c > 'M' ORDER BY t, c});
is($cmpk_read, $heap_cmpk_read,
	"a table keyed by (long text, citext) reads in its key's order through its primary key's index, as heap's twin's");
my ($cmpl_read, $heap_cmpl_read) = $cmp_all->('cmpl', 'heap_cmpl',
	'SELECT id FROM %s ORDER BY u, c', 'SELECT id FROM %s ORDER BY u DESC, c DESC',
	q{SELECT id FROM %s WHERE u > repeat('a', 20000) || 'b' ORDER BY u, c});
is($cmpl_read, $heap_cmpl_read,
	"a comparator index orders a long text past the bound as its ICU collation does, as heap's twin's");

# The comparator btrees' build (prevpf-310's C1): a non-unique index over citext
# whose rows' keys its class calls equal, 'a' and 'A' alternating, builds in its
# tree's order, which ties class-equal keys by the rows' keys, and reads back in
# heap's twin's order, and again after more rows and a fold of its storage.
$node->safe_psql('postgres', q{
	CREATE TABLE cmpt (id int PRIMARY KEY, c citext);
	CREATE TABLE heap_cmpt (id int PRIMARY KEY, c citext);
	CREATE TABLE cmpt_rows AS SELECT g AS id, CASE WHEN g % 2 = 0 THEN 'a' ELSE 'A' END AS c
	FROM generate_series(1, 40) g;
	INSERT INTO heap_cmpt SELECT * FROM cmpt_rows;
	CREATE INDEX heap_cmpt_c ON heap_cmpt (c);
});
$node->safe_psql('postgres',
	"DO \$\$ BEGIN PERFORM doltgres_test_index_routine('cmpt_pkey'); END \$\$;"
	  . " SELECT doltgres_test_write('cmpt', true, 'INSERT INTO cmpt SELECT * FROM cmpt_rows');"
	  . " CREATE INDEX cmpt_c ON cmpt (c);");
my $cmpt_routines = join('',
	map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" } ('cmpt_c', 'cmpt_pkey'));
my $cmpt_built = $node->safe_psql('postgres', "$cmpt_routines SELECT doltgres_test_index_build('cmpt', 'cmpt_c')");
my $cmpt_query = q{SELECT id FROM %s WHERE c = 'a' ORDER BY c};
my $cmpt_read = sub {
	return $node->safe_psql('postgres',
		"$cmp_only $cmpt_routines SELECT doltgres_test_standing('cmpt', '"
		  . (sprintf($cmpt_query, 'cmpt') =~ s/'/''/gr) . "')");
};
is( $cmpt_built . ' / ' . $cmpt_read->(),
	'40 40 whole / ' . $node->safe_psql('postgres', $cmp_only . sprintf($cmpt_query, 'heap_cmpt')),
	"a comparator index over rows its class calls equal builds in its tree's order and reads back in heap's twin's order"
);
$node->safe_psql('postgres', q{INSERT INTO heap_cmpt SELECT id + 40, c FROM cmpt_rows});
$node->safe_psql('postgres',
	"$cmpt_routines SELECT doltgres_test_write('cmpt', true, 'INSERT INTO cmpt SELECT id + 40, c FROM cmpt_rows');"
	  . " SELECT length(doltgres_test_index_fold('cmpt_c')) > 0;");
is($cmpt_read->(), $node->safe_psql('postgres', $cmp_only . sprintf($cmpt_query, 'heap_cmpt')),
	"a comparator index over rows its class calls equal keeps heap's twin's order after a fold");

# The btree's part 2, slice 7 (bugs row 168): a concurrent build's validation of
# a fork btree index, CREATE INDEX CONCURRENTLY's third phase, takes the rows the
# index already holds from its own entries, so it inserts only the rows it lacks,
# and a keyless table's index counts each row's copies once, as heap's twin's
# does.
$node->safe_psql('postgres', q{
	CREATE TABLE cic (k int, v int);
	CREATE INDEX cic_k ON cic (k);
	CREATE TABLE heap_cic (k int, v int);
	CREATE INDEX heap_cic_k ON heap_cic (k);
	CREATE TABLE cic_rows AS SELECT g % 5 AS k, g AS v FROM generate_series(1, 40) g;
	INSERT INTO heap_cic SELECT * FROM cic_rows;
	INSERT INTO heap_cic SELECT * FROM cic_rows WHERE v <= 3;
});
my $cic_routine = "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('cic_k'); END \$\$;";
$node->safe_psql('postgres',
	"$cic_routine SELECT doltgres_test_write('cic', true, 'INSERT INTO cic SELECT * FROM cic_rows');"
	  . " SELECT doltgres_test_write('cic', true, 'INSERT INTO cic SELECT * FROM cic_rows WHERE v <= 3');"
	  . " SELECT doltgres_test_index_validate('cic', 'cic_k');");
my $cic_query = q{SELECT k || ':' || count(*) FROM %s WHERE k >= 0 GROUP BY k ORDER BY k};
is( $node->safe_psql('postgres',
		"$ordered SET enable_hashagg = off; $cic_routine SELECT doltgres_test_standing('cic', '"
		  . (sprintf($cic_query, 'cic') =~ s/'/''/gr) . "')"),
	$node->safe_psql('postgres', "$ordered SET enable_hashagg = off; " . sprintf($cic_query, 'heap_cic')),
	"a concurrent build's validation of a fork btree index inserts only the rows it lacks, a keyless table's copies counted once"
);

# The btree's part 2, slice 7 (bugs row 168): a concurrent build's validation of
# a unique fork btree index inserts a row the index lacks through its aminsert,
# whose unique check refuses a row whose key another live row holds with
# nbtree's 23505, as a concurrent build refuses a row inserted while it runs.
$node->safe_psql('postgres', q{
	CREATE TABLE cu (id int PRIMARY KEY, k int);
	CREATE UNIQUE INDEX cu_k ON cu (k);
});
my $cu_routines = join('',
	map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" } ('cu_k', 'cu_pkey'));
$node->safe_psql('postgres',
	"$cu_routines SELECT doltgres_test_write('cu', true, 'INSERT INTO cu VALUES (1, 1), (2, 2)');");
$node->safe_psql('postgres',
	"DO \$\$ BEGIN PERFORM doltgres_test_index_routine('cu_pkey'); END \$\$;"
	  . " SELECT doltgres_test_write('cu', true, 'INSERT INTO cu VALUES (3, 1)');");
my ($cu_ret, $cu_out, $cu_err) = $node->psql('postgres',
	"$cu_routines SELECT doltgres_test_index_validate('cu', 'cu_k')",
	extra_params => [ '-v', 'VERBOSITY=verbose' ]);
like(
	$cu_err,
	qr/ERROR:  23505: duplicate key value violates unique constraint "cu_k"/,
	"a concurrent build's validation of a unique fork btree index refuses a row whose key another live row holds with nbtree's 23505"
);

# The btree's switch (CommPF 9): a key column's key type is its attribute's
# type's, as its table's key holds the column, not its operator class's input
# type's, which for enum_ops, array_ops, range_ops, and record_ops is a
# pseudo-type, so a point lookup through a primary key's index over an enum, an
# array, a range, or a composite reads its table's rows by the index, as heap's
# twin's does.
$node->safe_psql('postgres', q{
	CREATE TYPE poly_rainbow AS ENUM ('red', 'orange', 'yellow');
	CREATE TYPE poly_pair AS (a int, b text);
});
my %poly = (
	poly_enum => [ 'poly_rainbow', q{'red', 'orange', 'yellow'}, q{'orange'} ],
	poly_array => [ 'int[]', q{'{1,2}', '{2}', '{3,1}'}, q{'{2}'} ],
	poly_range => [ 'int4range', q{'[1,3)', '[2,9)', '[5,6)'}, q{'[2,9)'} ],
	poly_row => [ 'poly_pair', q{'(1,a)', '(2,b)', '(3,c)'}, q{'(2,b)'::poly_pair} ]);
my ($poly_fork, $poly_heap) = ('', '');
for my $table (sort keys %poly)
{
	my ($type, $values, $sought) = @{ $poly{$table} };
	my @rows = split /, /, $values;
	my $rows = join(', ', map { "($rows[$_], $_)" } 0 .. $#rows);
	for my $each ($table, "heap_$table")
	{
		my $fork = $each =~ /^heap_/ ? 'false' : 'true';
		$node->safe_psql('postgres', "CREATE TABLE $each (id $type PRIMARY KEY, v int)");
		$node->safe_psql('postgres',
			"SELECT doltgres_test_write('$each', $fork, '" . ("INSERT INTO $each VALUES $rows" =~ s/'/''/gr) . "')");
	}
	my $query = "SELECT v FROM %s WHERE id = $sought";
	my $fork = sprintf($query, $table) =~ s/'/''/gr;
	my $routine = join('', map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" } "${table}_pkey");
	$poly_fork .= $node->safe_psql('postgres',
		"SET enable_seqscan = off; SET enable_bitmapscan = off; $routine "
		  . "SELECT doltgres_test_standing('$table', 'SELECT explained_index(''" . ($fork =~ s/'/''/gr) . "'')'); "
		  . "SELECT doltgres_test_standing('$table', '$fork')") . "\n";
	my $heap = sprintf($query, "heap_$table");
	$poly_heap .= $node->safe_psql('postgres',
		"SET enable_seqscan = off; SET enable_bitmapscan = off; "
		  . "SELECT explained_index('" . ($heap =~ s/'/''/gr) . "'); $heap") . "\n";
}
is($poly_fork, $poly_heap =~ s/heap_//gr,
	"a point lookup through a primary key's index over an enum, an array, a range, or a composite reads its table's row by the index, as heap's twin's"
);

# The btree's switch (CommPF 9): a speculative insert's partial check of an
# immediate unique index answers whether no other row holds the key, as
# btinsert answers _bt_check_unique's, so INSERT ... ON CONFLICT into a keyless
# table or a keyed one whose arbiter is a unique index the fork stores inserts
# a free key, updates or skips a held one, and ends, as heap's twin's does.
my %conflicted = (conflict_keyless => '(key int4, fruit text)', conflict_keyed => '(id serial PRIMARY KEY, key int4, fruit text)');
my ($conflict_fork, $conflict_heap) = ('', '');
for my $table (sort keys %conflicted)
{
	for my $each ($table, "heap_$table")
	{
		my $fork = $each =~ /^heap_/ ? 'false' : 'true';
		$node->safe_psql('postgres',
			"CREATE TABLE $each $conflicted{$table}; CREATE UNIQUE INDEX ${each}_key ON $each (key)");
		my $routines =
		  $fork eq 'true'
		  ? join('', map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" } "${each}_key", ($table eq 'conflict_keyed' ? "${each}_pkey" : ()))
		  : '';
		my @statements = (
			"INSERT INTO $each (key, fruit) VALUES (1, 'Apple') ON CONFLICT (key) DO UPDATE SET fruit = excluded.fruit",
			"INSERT INTO $each (key, fruit) VALUES (1, 'Banana') ON CONFLICT (key) DO UPDATE SET fruit = excluded.fruit",
			"INSERT INTO $each (key, fruit) VALUES (1, 'Cherry'), (2, 'Orange') ON CONFLICT (key) DO NOTHING");
		for my $statement (@statements)
		{
			$node->safe_psql('postgres',
				"SET statement_timeout = '30s'; $routines "
				  . "SELECT doltgres_test_write('$each', $fork, '" . ($statement =~ s/'/''/gr) . "')");
		}
		my $read = "SELECT string_agg(key || ' ' || fruit, ', ' ORDER BY key) FROM $each";
		my $rows = $node->safe_psql('postgres',
			$fork eq 'true' ? "$routines SELECT doltgres_test_standing('$each', '" . ($read =~ s/'/''/gr) . "')" : $read);
		$fork eq 'true' ? ($conflict_fork .= "$rows\n") : ($conflict_heap .= "$rows\n");
	}
}
is($conflict_fork, $conflict_heap,
	"INSERT ... ON CONFLICT on a fork table whose arbiter is a unique index the fork stores inserts, updates, and skips as heap's twin's"
);

# The btree's switch (CommPF 9): name_ops keys a name column by name, its
# class's input type, though the index's attribute stores a cstring, so an
# index on a name column takes its rows and reads them by the index, as heap's
# twin's does.
{
	my $table = 'named';
	$node->safe_psql('postgres', q{
		CREATE TABLE named (id int, n name);
		CREATE TABLE heap_named (id int, n name);
		CREATE INDEX named_n ON named (n);
		CREATE INDEX heap_named_n ON heap_named (n);
		CREATE TABLE named_rows AS SELECT g AS id, ('n' || (g * 37 % 100))::name AS n FROM generate_series(1, 100) g;
		INSERT INTO heap_named SELECT * FROM named_rows;
		CREATE FUNCTION explained_scan(query text) RETURNS text LANGUAGE plpgsql AS $$
		DECLARE
			plan json;
		BEGIN
			EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
			plan := plan->0->'Plan';
			WHILE plan->>'Index Name' IS NULL AND plan->'Plans' IS NOT NULL LOOP
				plan := plan->'Plans'->0;
			END LOOP;
			RETURN (plan->>'Node Type') || ' ' || coalesce(plan->>'Index Name', '-');
		END $$;
	});
	my $routines = join('', map { "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('$_'); END \$\$;" } 'named_n');
	$node->safe_psql('postgres',
		"$routines SELECT doltgres_test_write('named', true, 'INSERT INTO named SELECT * FROM named_rows')");
	my @queries = (q{SELECT string_agg(id::text, ',' ORDER BY n, id) FROM %s WHERE n > 'n5'},
		q{SELECT string_agg(id::text, ',' ORDER BY id) FROM %s WHERE n = 'n37'});
	my ($fork, $heap) = ('', '');
	for my $query (@queries)
	{
		my $forked = sprintf($query, $table) =~ s/'/''/gr;
		$fork .= $node->safe_psql('postgres',
			"SET enable_seqscan = off; SET enable_bitmapscan = off; $routines "
			  . "SELECT doltgres_test_standing('$table', 'SELECT explained_scan(''" . ($forked =~ s/'/''/gr) . "'')'); "
			  . "SELECT doltgres_test_standing('$table', '$forked')") . "\n";
		my $heaped = sprintf($query, "heap_$table");
		$heap .= $node->safe_psql('postgres',
			"SET enable_seqscan = off; SET enable_bitmapscan = off; "
			  . "SELECT explained_scan('" . ($heaped =~ s/'/''/gr) . "'); $heaped") . "\n";
	}
	is($fork, $heap =~ s/heap_//gr,
		"an index on a name column takes its rows and reads them by the index, as heap's twin's");
}

# The btree's switch (SwitchPF 2): the planner reads the first or last entry of
# a range qual's index as an index tuple when the qual's bound falls in its
# column's first or last histogram bucket (get_actual_variable_endpoint), which a
# fork btree's index-only entry gives beside its heap tuple, so the query plans
# and reads as heap's twin's does.
{
	my $table = 'ends';
	for my $each ($table, "heap_$table")
	{
		my $fork = $each =~ /^heap_/ ? 'false' : 'true';
		$node->safe_psql('postgres',
			"CREATE TABLE $each (thousand int4, tenthous int4); CREATE INDEX ${each}_tt ON $each (thousand, tenthous)");
		my $routine = $fork eq 'true' ? "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('${each}_tt'); END \$\$;" : '';
		$node->safe_psql('postgres',
			"$routine SELECT doltgres_test_write('$each', $fork, "
			  . "'INSERT INTO $each SELECT g % 1000, g FROM generate_series(0, 9999) g')");
		$node->safe_psql('postgres',
			"SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', '$each', "
			  . "'relpages', 45::int4, 'reltuples', 10000::real); "
			  . "SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', '$each', "
			  . "'attname', 'thousand', 'inherited', false, 'null_frac', 0::real, 'avg_width', 4::int4, "
			  . "'n_distinct', 1000::real, 'histogram_bounds', '{0,100,200,300,400,500,600,700,800,900,999}'::text)");
	}
	my $routines = "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('ends_tt'); END \$\$;";
	my @queries = (
		q{SELECT string_agg(thousand || ':' || tenthous, ',' ORDER BY thousand) FROM %s WHERE thousand < 2 AND tenthous IN (1001, 3000)},
		q{SELECT string_agg(thousand || ':' || tenthous, ',' ORDER BY thousand) FROM %s WHERE thousand > 997 AND tenthous IN (998, 9999)});
	my ($fork, $heap) = ('', '');
	for my $query (@queries)
	{
		my $forked = sprintf($query, $table) =~ s/'/''/gr;
		$fork .= $node->safe_psql('postgres',
			"SET enable_seqscan = off; SET enable_bitmapscan = off; $routines "
			  . "SELECT doltgres_test_standing('$table', 'SELECT explained_scan(''" . ($forked =~ s/'/''/gr) . "'')'); "
			  . "SELECT doltgres_test_standing('$table', '$forked')") . "\n";
		my $heaped = sprintf($query, "heap_$table");
		$heap .= $node->safe_psql('postgres',
			"SET enable_seqscan = off; SET enable_bitmapscan = off; "
			  . "SELECT explained_scan('" . ($heaped =~ s/'/''/gr) . "'); $heaped") . "\n";
	}
	is($fork, $heap =~ s/heap_//gr,
		"a range qual whose bound falls in its column's first or last histogram bucket plans and reads by a fork btree as heap's twin's"
	);
}

# The btree's switch (CommPF 10): a key that fits an index tuple only
# compressed, a long text of one letter, gives the planner's endpoint probe its
# index tuple compressed as index_form_tuple compresses it, so a range qual
# whose bound falls in its column's first or last histogram bucket plans and
# reads as heap's twin's does.
my %longkeys;
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'longkeys' : 'twin_longkeys';
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int4, t text) $am;
		CREATE INDEX ${t}_t ON $t (t);
		INSERT INTO $t SELECT g, repeat('L', 12000) || lpad(g::text, 4, '0') FROM generate_series(0, 999) g;
		SELECT pg_restore_relation_stats('schemaname', 'public', 'relname', '$t', 'relpages', 10::int4, 'reltuples', 1000::real);
		SELECT pg_restore_attribute_stats('schemaname', 'public', 'relname', '$t', 'attname', 't', 'inherited', false,
			'null_frac', 0::real, 'avg_width', 100::int4, 'n_distinct', -1::real, 'histogram_bounds',
			(SELECT array_agg(repeat('L', 12000) || lpad(g::text, 4, '0') ORDER BY g) FROM generate_series(0, 999, 111) g)::text);
	});
	$longkeys{$am} = $node->safe_psql('postgres', qq{
		SET enable_seqscan = off; SET enable_bitmapscan = off;
		SELECT explained_scan(\$q\$SELECT id FROM $t WHERE t < repeat('L', 12000) || '0002'\$q\$);
		SELECT string_agg(id::text, ',' ORDER BY id) FROM $t WHERE t < repeat('L', 12000) || '0002';
		SELECT string_agg(id::text, ',' ORDER BY id) FROM $t WHERE t > repeat('L', 12000) || '0997';
	});
}
is($longkeys{''}, $longkeys{'USING doltgres_twin'} =~ s/twin_//gr,
	"a range qual past its histogram over keys that fit an index tuple only compressed plans and reads by a fork btree as heap's twin's"
);

# A speculative insert's kill (CommPF 10, insert-conflict-specconflict): an
# INSERT ... ON CONFLICT whose check found no row waits before its speculative
# insert while another session commits the key, so its arbiter's partial check
# finds the key taken and writes no entry, and its kill removes only the entry
# its other index's aminsert wrote, running no index expression, as
# heap_abort_speculative runs none; its retry then skips the key, with the
# notices and rows of heap's twin's.
my %killed;
$node->safe_psql('postgres', q{
	CREATE FUNCTION spec_noisy(int4) RETURNS int4 IMMUTABLE LANGUAGE plpgsql
		AS $$ BEGIN RAISE NOTICE 'spec_noisy(%)', $1; RETURN $1; END $$;
});
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'killed' : 'twin_killed';
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (k int4, v text) $am;
		CREATE UNIQUE INDEX ${t}_k ON $t ((spec_noisy(k)));
		CREATE INDEX ${t}_v ON $t ((spec_noisy(k + 10)));
		INSERT INTO $t VALUES (0, 'first');
	});
	my $inserter = $node->background_psql('postgres', on_error_stop => 0);
	$inserter->query_safe(q{
		SET statement_timeout = '60s';
		SELECT injection_points_set_local();
		SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
	});
	$inserter->query_until(qr/started/,
		"\\echo started\nINSERT INTO $t VALUES (1, 'inserter') ON CONFLICT ((spec_noisy(k))) DO NOTHING;\n");
	$node->wait_for_event('client backend', 'exec-insert-before-insert-speculative');
	$node->safe_psql('postgres', "INSERT INTO $t VALUES (1, 'other')");
	$node->safe_psql('postgres', q{
		SELECT injection_points_detach('exec-insert-before-insert-speculative');
		SELECT injection_points_wakeup('exec-insert-before-insert-speculative');
	});
	$inserter->query('SELECT 1');
	$killed{$am} = $inserter->{stderr} . "\n"
	  . $node->safe_psql('postgres', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k) FROM $t");
	$inserter->quit;
}
is($killed{''}, $killed{'USING doltgres_twin'},
	"a speculative insert killed after its arbiter found the key taken runs no index expression and skips the key as heap's twin's");
is( join(' | ', map { $node->safe_psql('postgres', "SELECT doltgres_test_index_entries('$_')") } 'killed_k', 'killed_v'),
	"add (0,1)\nadd (0,2)\nadd (0,1)\nadd (0,2) | add (0,1)\nadd (0,2)\nremove (0,3), add (0,3)\nadd (0,1)\nadd (0,2)\nremove (0,3), add (0,3)",
	"a killed speculative insert removes the entry and record its other index wrote and none at its arbiter, which wrote none");

# A speculative insert's partial check (CommPF 10, insert-conflict-specconflict)
# answers another transaction's running write of its key as a conflict and waits
# for none, as _bt_check_unique answers UNIQUE_CHECK_PARTIAL, so the insert is
# killed, its token released, and its retry's check waits for the writer, with
# the locks and rows of heap's twin's.
my %running;
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'running' : 'twin_running';
	$node->safe_psql('postgres', "CREATE TABLE $t (k int4, v text) $am; CREATE UNIQUE INDEX ${t}_k ON $t (k)");
	my ($inserter, $holder) = map { $node->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
	my $pid = $inserter->query_safe(q{
		SELECT injection_points_set_local();
		SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
		SELECT pg_backend_pid();
	}) =~ s/.*\n//sr;
	$inserter->query_until(qr/started/,
		"\\echo started\nINSERT INTO $t VALUES (1, 'inserter') ON CONFLICT (k) DO NOTHING;\n");
	$node->wait_for_event('client backend', 'exec-insert-before-insert-speculative');
	$holder->query_safe("BEGIN; INSERT INTO $t VALUES (1, 'holder')");
	$node->safe_psql('postgres', q{
		SELECT injection_points_detach('exec-insert-before-insert-speculative');
		SELECT injection_points_wakeup('exec-insert-before-insert-speculative');
	});
	$node->poll_query_until('postgres', "SELECT wait_event_type = 'Lock' FROM pg_stat_activity WHERE pid = $pid");
	$running{$am} = $node->safe_psql('postgres',
		"SELECT string_agg(locktype || ' ' || mode || ' ' || granted, ', ' ORDER BY locktype, mode) "
		  . "FROM pg_locks WHERE pid = $pid AND locktype IN ('spectoken', 'transactionid')");
	$holder->query_safe('COMMIT');
	$inserter->query('SELECT 1');
	$running{$am} .= "\n" . $node->safe_psql('postgres', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k) FROM $t");
	$_->quit for $inserter, $holder;
}
is($running{''}, $running{'USING doltgres_twin'},
	"a speculative insert's partial check answers a running write of its key with no wait, and its retry waits holding no token, as heap's twin's");

# Bugs row 232 (create_index's concur_heap): a VACUUM FULL rebuilds a unique
# index a failed CIC left invalid without its unique check, since CLUSTER's
# reindex skips constraint checks (ii_Unique false) and leaves the index
# invalid and not ready; a DELETE after it leaves that index alone, as heap's
# twin's does.
my %rebuilt;
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'rebuilt' : 'twin_rebuilt';
	$node->safe_psql('postgres', "CREATE TABLE $t (f1 text, f2 text) $am; INSERT INTO $t VALUES ('a', 'b'), ('b', 'b'), ('b', 'x')");
	for my $statement ("CREATE UNIQUE INDEX CONCURRENTLY ${t}_f2 ON $t (f2)", "VACUUM FULL $t",
		"DELETE FROM $t WHERE f1 = 'b'", "VACUUM FULL $t")
	{
		my ($code, $out, $err) = $node->psql('postgres', $statement);
		$rebuilt{$am} .= join(' ', $err =~ /((?:ERROR|WARNING|DETAIL):[^\n]*)/g) . ' | ';
	}
	$rebuilt{$am} .= $node->safe_psql('postgres',
		"SELECT string_agg(f1 || f2, ',' ORDER BY f1, f2) FROM $t; "
		  . "SELECT indisvalid || ' ' || indisready FROM pg_index WHERE indexrelid = '${t}_f2'::regclass");
}
is($rebuilt{''}, $rebuilt{'USING doltgres_twin'} =~ s/twin_//gr,
	"a VACUUM FULL rebuilds a unique index a failed CIC left invalid without its unique check, and a DELETE after leaves the index that isn't ready alone, as heap's twin's");
# CommPF 10's no-user-code removal: a row's entries are removed with no index
# expression or predicate run, as heap's DELETE and HOT UPDATE run none. An
# index whose entries need user code to form keeps a record of each, which the
# removal reads. A DELETE from a table whose expression index a failed CIC
# left invalid deletes the row, as heap's twin's does.
my %unbuilt;
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'unbuilt' : 'twin_unbuilt';
	$node->safe_psql('postgres', "CREATE TABLE $t (a int, b int) $am; INSERT INTO $t VALUES (1, 0), (2, 1)");
	my ($built_code, $built_out, $built_err) =
	  $node->psql('postgres', "CREATE INDEX CONCURRENTLY ${t}_ab ON $t ((a / b))");
	my ($code, $out, $err) = $node->psql('postgres', "DELETE FROM $t WHERE b = 0");
	$unbuilt{$am} = join(' | ',
		$built_err =~ /(ERROR:[^\n]*)/,
		$err =~ /(ERROR:[^\n]*)/,
		$node->safe_psql('postgres', "SELECT string_agg(a || ' ' || b, ', ' ORDER BY a) FROM $t"));
}
is($unbuilt{''}, $unbuilt{'USING doltgres_twin'},
	"a DELETE from a table whose expression index a failed CIC left invalid deletes the row and evaluates no expression, as heap's twin's");

# An index function's calls (spec_noisy's NOTICEs): a DELETE and an UPDATE of a
# column the index doesn't read call it no time, and an UPDATE of a column it
# reads calls it once, for the new row, as heap's twin's; the index holds the
# live rows' entries and records only.
my %noisy;
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'noisy' : 'twin_noisy';
	$node->safe_psql('postgres',
		"CREATE TABLE $t (id int PRIMARY KEY, k int, other text) $am; CREATE INDEX ${t}_k ON $t ((spec_noisy(k)))");
	$node->safe_psql('postgres', "INSERT INTO $t VALUES (1, 1, 'a'), (2, 2, 'b')");
	for my $statement ("UPDATE $t SET other = 'c' WHERE id = 1", "UPDATE $t SET k = 3 WHERE id = 1",
		"DELETE FROM $t WHERE id = 2")
	{
		my ($code, $out, $err) = $node->psql('postgres', $statement);
		$noisy{$am} .= scalar(() = $err =~ /NOTICE:  spec_noisy/g) . ' ';
	}
	$noisy{$am} .= $node->safe_psql('postgres',
		"SET enable_seqscan = off; SELECT string_agg(id || ' ' || other, ', ') FROM $t WHERE spec_noisy(k) > 0");
}
is($noisy{''}, $noisy{'USING doltgres_twin'},
	"an UPDATE of a column a volatile expression index doesn't read calls its function no time, and one of a column it reads calls it once, as heap's twin's");
is($node->safe_psql('postgres', "SELECT doltgres_test_index_entries('noisy_k')"),
	"delete, put\ndelete, put\nput\nput, delete, put\ndelete, put",
	"an expression index's entries and records follow its rows: the moved row's old entry and its record removed, the deleted row's both");

# A partial index keeps no record of a row its predicate leaves out, and that
# row's delete removes nothing; a keyless table's copies keep their records
# under their tuple ids, and a delete of one copy leaves the other's.
$node->safe_psql('postgres', q{
	CREATE TABLE partial (id int PRIMARY KEY, v int);
	CREATE INDEX partial_v ON partial (v) WHERE v > 0;
	INSERT INTO partial VALUES (1, -1), (2, 2);
	DELETE FROM partial WHERE id = 1;
	CREATE TABLE copied (v int);
	CREATE INDEX copied_v ON copied ((v * 2));
	INSERT INTO copied VALUES (1), (1);
	DELETE FROM copied WHERE ctid = '(0,1)';
});
is( join(' | ', map { $node->safe_psql('postgres', "SELECT doltgres_test_index_entries('$_')") } 'partial_v', 'copied_v'),
	"put\nput | remove (0,1), add (0,2), add (0,1)\nremove (0,1), add (0,1)\nadd (0,2)",
	"a partial index keeps no record of a row it leaves out, and a keyless table's copies keep theirs under their tuple ids");

# A not-ready index (a CIC waiting for the table's writers before its build):
# a DELETE and an UPDATE meanwhile leave it alone, as heap's do, and the
# finished index holds exactly the live rows' entries and records.
$node->safe_psql('postgres', q{
	CREATE TABLE unready (id int PRIMARY KEY, v int);
	INSERT INTO unready VALUES (1, 1), (2, 2), (3, 3);
});
my $unready_writer = $node->background_psql('postgres');
$unready_writer->query_safe("BEGIN; INSERT INTO unready VALUES (4, 4)");
my $unready_builder = $node->background_psql('postgres');
$unready_builder->query_until(qr/started/, "\\echo started\nCREATE INDEX CONCURRENTLY unready_v ON unready (v);\n");
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_index WHERE indexrelid::regclass::text = 'unready_v' AND NOT indisready");
$node->safe_psql('postgres', "DELETE FROM unready WHERE id = 1; UPDATE unready SET v = 20 WHERE id = 2");
$unready_writer->query_safe('COMMIT');
$unready_builder->query('SELECT 1');
$_->quit for $unready_writer, $unready_builder;
is( $node->safe_psql('postgres', "SELECT doltgres_test_index_entries('unready_v')") . ' | '
	  . $node->safe_psql('postgres',
		"SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY v) FROM unready WHERE v > 0"),
	"none | 3,4,2",
	"a DELETE and an UPDATE while a CIC's index isn't ready leave it alone, and the built index holds exactly the live rows");

# Every reader of an index's entries reads below its records: index scans both
# ways over a comparator tree and a byte-ordered one, an index-only scan, a
# bitmap scan, a unique check, and the planner's tuple count after VACUUM, the
# entries built by CREATE INDEX over rows already there and then changed, as
# heap's twin's.
my %readers;
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'readers' : 'twin_readers';
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int PRIMARY KEY, c text, n int) $am;
		INSERT INTO $t SELECT g, CASE WHEN g % 2 = 0 THEN 'V' ELSE 'v' END || g, g - 5 FROM generate_series(1, 30) g;
		CREATE INDEX ${t}_c ON $t ((c::citext));
		CREATE INDEX ${t}_n ON $t ((n * 2)) WHERE n > 0;
		CREATE UNIQUE INDEX ${t}_u ON $t ((n + 1000));
		DELETE FROM $t WHERE id IN (3, 12);
		UPDATE $t SET c = 'w' || id, n = n + 100 WHERE id IN (7, 20);
	});
	my @queries = (
		"SELECT string_agg(id::text, ',' ORDER BY c::citext) FROM $t WHERE c::citext BETWEEN 'V1' AND 'v3'",
		"SELECT string_agg(id::text, ',') FROM (SELECT id FROM $t ORDER BY c::citext DESC LIMIT 8) s",
		"SELECT string_agg(x::text, ',') FROM (SELECT n * 2 AS x FROM $t WHERE n > 0 AND n * 2 > 4 ORDER BY n * 2 DESC) s",
		"SET enable_indexscan = off; SET enable_bitmapscan = on; SELECT count(*) FROM $t WHERE n > 0 AND n * 2 > 4");
	$readers{$am} = join(' | ',
		map { $node->safe_psql('postgres', "SET enable_seqscan = off; SET enable_sort = off; $_") } @queries);
	my ($code, $out, $err) = $node->psql('postgres', "INSERT INTO $t VALUES (99, 'x', 2)");
	$readers{$am} .= ' | ' . (($err =~ /(ERROR:[^\n]*)/)[0] // 'inserted');
	$node->safe_psql('postgres',
		join('', map { "SELECT doltgres_test_index_fold('${t}_$_'); " } 'c', 'n', 'u')) if $am eq '';
	$node->safe_psql('postgres', "VACUUM $t");
	$readers{$am} .= ' | ' . $node->safe_psql('postgres',
		"SELECT string_agg(reltuples::text, ',' ORDER BY relname) FROM pg_class WHERE relname IN ('${t}_c', '${t}_n', '${t}_u')");
}
is($readers{''}, $readers{'USING doltgres_twin'} =~ s/twin_//gr,
	"an index that keeps records reads, checks, and counts only its entries, built over existing rows and changed after, as heap's twin's");

# A crash replays an entry and its record together: an insert's pairs and a
# delete's removal of one.
$node->safe_psql('postgres', q{
	CREATE TABLE crashed (id int PRIMARY KEY, v int);
	CREATE INDEX crashed_v ON crashed ((v + 1));
	INSERT INTO crashed VALUES (1, 1), (2, 2);
	DELETE FROM crashed WHERE id = 2;
});
my $crashed = $node->safe_psql('postgres', "SELECT doltgres_test_index_entries('crashed_v')");
$node->stop('immediate');
$node->start;
is( $crashed . ' / ' . $node->safe_psql('postgres', "SELECT doltgres_test_index_entries('crashed_v')"),
	"put\ndelete, put\nput\ndelete, put / put\ndelete, put\nput\ndelete, put",
	"a crash replays an entry and its record together, an insert's and a delete's");

# A build under an older snapshot that counts inserts as running keeps their
# rows' entries and records out of its base, as delta versions under their
# writers; the snapshot reads the table as heap's twin, and a later DELETE of
# such a row removes its entry through its record.
my %lated;
for my $am ('', 'USING doltgres_twin')
{
	my $t = $am eq '' ? 'lated' : 'twin_lated';
	$node->safe_psql('postgres',
		"CREATE TABLE $t (id int PRIMARY KEY, name text) $am; INSERT INTO $t SELECT n, 'a' || n FROM generate_series(1, 3) n");
	my $older = $node->background_psql('postgres', on_error_stop => 0);
	$older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
	$node->safe_psql('postgres',
		"INSERT INTO $t VALUES (4, 'a4'), (5, 'a5'); CREATE INDEX ${t}_x ON $t ((name || '!'))");
	my $read = "SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY name || '!') FROM $t WHERE name || '!' > ''";
	$lated{$am} = $older->query_safe($read);
	$node->safe_psql('postgres', "DELETE FROM $t WHERE id = 4");
	$older->query_safe('COMMIT');
	$older->quit;
	$lated{$am} .= ' | ' . $node->safe_psql('postgres', $read);
}
is($lated{''}, $lated{'USING doltgres_twin'},
	"a build under an older snapshot keeps its running writers' entries and records as delta versions, and a DELETE of such a row removes its entry by its record");

# Bugs row 288: a table whose removal of old entries the backend doesn't hold
# answers none and leaves its caller's handle as it was, and a skip array's
# step past its type's end answers none and leaves its caller's datum as it was.
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_listed_unwritten('lated'), doltgres_test_step_unwritten()"),
	't|t',
	"an answer of none leaves the caller's handle and datum as they were");

# Bugs row 313: a concurrent build's validation forms no key of a row its
# build's snapshot saw, which the build indexed, so a REINDEX CONCURRENTLY runs
# an indexed row's expression once, as heap's validation does: the regress
# suite's concur_reindex_ddl sequence gives the reference's one NOTICE and its
# two errors.
my (undef, undef, $ddl_err) = $node->psql(
	'postgres', q{
CREATE TABLE concur_reindex_ddl_tab (i int);
INSERT INTO concur_reindex_ddl_tab VALUES (1);
CREATE FUNCTION concur_reindex_ddl_f() RETURNS void LANGUAGE plpgsql AS
  'begin DROP INDEX IF EXISTS public.concur_reindex_ddl_ind2; end';
CREATE FUNCTION concur_reindex_ddl_im(int) RETURNS int
  LANGUAGE plpgsql IMMUTABLE AS
  'begin perform public.concur_reindex_ddl_f(); return $1; end';
CREATE INDEX concur_reindex_ddl_ind1 ON
  concur_reindex_ddl_tab (concur_reindex_ddl_im(i));
CREATE INDEX concur_reindex_ddl_ind2 ON concur_reindex_ddl_tab (i);
REINDEX TABLE CONCURRENTLY concur_reindex_ddl_tab;
REINDEX INDEX CONCURRENTLY concur_reindex_ddl_ind2;
REINDEX INDEX CONCURRENTLY concur_reindex_ddl_ind1;
REINDEX INDEX CONCURRENTLY concur_reindex_ddl_ind2;
},
	on_error_stop => 0,
	extra_params => [ '-v', 'VERBOSITY=verbose' ]);
is( join("\n", $ddl_err =~ /((?:NOTICE|ERROR):  \w{5}: [^\n]*)/g),
	join("\n",
		'NOTICE:  00000: index "concur_reindex_ddl_ind2" does not exist, skipping',
		'ERROR:  55006: cannot DROP INDEX "concur_reindex_ddl_ind2" because it is being used by active queries in this session',
		'ERROR:  42P01: relation "concur_reindex_ddl_ind2" does not exist'),
	"a REINDEX CONCURRENTLY computes an indexed row's expression once, as heap's validation does"
);

# Bugs row 313: CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY of a fork
# btree expression index while another session, which held its lock across the
# build, inserts while the command waits for writers before its validation:
# the index is valid and holds every row once. The index's expression waits on
# an advisory lock at one row, so the writer's lock comes after the build's
# first wait.
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION validated_at(k int) RETURNS int IMMUTABLE LANGUAGE plpgsql
	AS $$ BEGIN IF k = 2 THEN PERFORM pg_advisory_xact_lock_shared(45); END IF; RETURN k; END $$;
CREATE TABLE validated (k int);
INSERT INTO validated SELECT generate_series(1, 3);
});
my $validated_locker = $node->background_psql('postgres');
my $validated_builder = $node->background_psql('postgres');
my $validated_writer = $node->background_psql('postgres');
my $validated_first = 4;
foreach my $statement (
	'CREATE INDEX CONCURRENTLY validated_k ON validated ((validated_at(k)))',
	'REINDEX INDEX CONCURRENTLY validated_k')
{
	$validated_locker->query_safe('SELECT pg_advisory_lock(45)');
	$validated_builder->query_until(qr/start/, "\\echo start\n$statement;\n");
	$node->poll_query_until('postgres',
		q{SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE wait_event = 'advisory' AND query ~ 'CONCURRENTLY')}
	);
	$validated_writer->query_safe(
		'BEGIN; LOCK TABLE validated IN ROW EXCLUSIVE MODE');
	$validated_locker->query_safe('SELECT pg_advisory_unlock(45)');
	$node->poll_query_until('postgres',
		q{SELECT EXISTS (SELECT 1 FROM pg_stat_progress_create_index WHERE phase = 'waiting for writers before validation')}
	);
	my $last = $validated_first + 1;
	$validated_writer->query_safe(
		"INSERT INTO validated SELECT generate_series($validated_first, $last); COMMIT"
	);
	$validated_builder->query_safe('SELECT 1');
	is( $node->safe_psql(
			'postgres',
			"SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT (SELECT indisvalid FROM pg_index WHERE indexrelid = 'validated_k'::regclass) || ' ' || string_agg(k::text, ',' ORDER BY k) FROM validated WHERE validated_at(k) > 0"
		),
		'true ' . join(',', 1 .. $last),
		"$statement while another session inserts before its validation leaves a valid index holding every row once"
	);
	$validated_first = $last + 1;
}
$_->quit for $validated_locker, $validated_builder, $validated_writer;

$node->stop;

# P2's index build and index move: a build whose snapshot predates another
# transaction's committed insert of rows past a 4 MB delta_memory carries their
# late entries to spill files under the writer's xid and completes; held
# unfolded by an older snapshot, the index then moves into a tablespace with
# those entries carried again, never waiting at the budget, and its storage
# holds an entry for every row after the move and after a crash.
my $moving_index = PostgreSQL::Test::Cluster->new('index_move');
$moving_index->init;
$moving_index->append_conf('postgresql.conf',
	"delta_memory = 4MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncollector_naptime = 3600s\nallow_in_place_tablespaces = on"
);
$moving_index->start;
$moving_index->safe_psql(
	'postgres', q{
	CREATE EXTENSION doltgres_test;
	CREATE TABLESPACE index_moved LOCATION '';
	CREATE TABLE indexed (id int PRIMARY KEY, name text);
	CREATE INDEX indexed_name ON indexed (name);
});
my $entries = sub {
	scalar(() = $moving_index->safe_psql('postgres', q{SELECT doltgres_test_index_entries('indexed_name')}) =~ /^put$/mg);
};
my $index_older = $moving_index->background_psql('postgres');
$index_older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
my $index_builder = $moving_index->background_psql('postgres');
$index_builder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$moving_index->safe_psql('postgres',
	q{SELECT doltgres_test_write('indexed', true, 'INSERT INTO indexed SELECT g, repeat(md5(g::text), 31) FROM generate_series(1, 10000) g')},
	timeout => 600);
$index_builder->query_safe(q{SELECT doltgres_test_index_build('indexed', 'indexed_name')});
$index_builder->query_safe('COMMIT');
$index_builder->quit;
is($entries->(), 10000, "a build at a 4 MB delta_memory carries every late entry of another transaction's rows");
$moving_index->safe_psql('postgres',
	q{SELECT doltgres_test_index_routine('indexed_name'); SELECT doltgres_test_index_move('indexed_name', 'index_moved')},
	timeout => 600);
is($entries->(), 10000, "an index move held unfolded carries every entry of another transaction past delta_memory");
$index_older->query_safe('COMMIT');
$index_older->quit;
$moving_index->stop('immediate');
$moving_index->start;
is($entries->(), 10000, 'and the moved entries survive a crash');
$moving_index->stop;

done_testing();
