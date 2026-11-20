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

# Indexes of methods the fork doesn't know (the unknown methods' text): such an
# index keeps its handler's routine and Postgres' pages, and names a fork
# table's rows by their versions' tids, each case beside heap's twin.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('unknown');
$node->init;
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins(); CREATE EXTENSION pageinspect');

# written returns the SQL that runs a statement over a table, with "%" standing
# for its name, through the fork's routine for a fork table and through heap's
# for heap's twin, whose name starts "heap_".
sub written
{
	my ($table, $statement) = @_;
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	(my $sql = $statement) =~ s/%/$table/g;
	$sql =~ s/'/''/g;
	return "SELECT doltgres_test_write('$table', $fork, '$sql')";
}

# hashed returns the ctids a hash index's bucket and overflow pages hold, in
# ctid order.
sub hashed
{
	my ($index) = @_;
	return $node->safe_psql('postgres',
		    "SELECT string_agg(ctid::text, ' ' ORDER BY ctid) FROM generate_series(1, "
		  . "pg_relation_size('$index') / current_setting('block_size')::int - 1) AS page, "
		  . "LATERAL hash_page_items(get_raw_page('$index', page::int)) "
		  . "WHERE hash_page_type(get_raw_page('$index', page::int)) IN ('bucket', 'overflow')"
	);
}

# Section 2's validate scan: CREATE INDEX CONCURRENTLY builds a hash index that
# misses the rows inserted during its build, here the fork table's three rows,
# which its own files don't hold, and a fourth row inserted once the index takes
# inserts; its validation then inserts each row the index lacks, so every row is
# indexed once, as heap's twin's index holds them.
for my $table ('validated', 'heap_validated')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (1, 'v1'), (2, 'v2'), (3, 'v3')"));
	$node->safe_psql('postgres',
		"CREATE INDEX ${table}_k ON $table USING hash (k)");
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES (4, 'v4')"));
}
$node->safe_psql('postgres',
	"SELECT doltgres_test_index_validate('validated', 'validated_k')");
is( hashed('validated_k') . ' / ' . hashed('heap_validated_k'),
	'(0,1) (0,2) (0,3) (0,4) / (0,1) (0,2) (0,3) (0,4)',
	"a concurrent build's validation of a hash index on a fork table inserts each row its index lacks, once"
);


# both runs each statement over a fork table and over heap's twin, "%" standing
# for the table's name, and returns their outputs, heap's with "heap_" taken out:
# a SELECT reads through the fork's routine for the fork table, under settings
# given before it, "vacuum" runs VACUUM, through the fork's for the fork table,
# and any other statement writes through each table's routine.
sub both
{
	my ($name, $settings, @statements) = @_;
	my %outputs;
	for my $table ($name, "heap_$name")
	{
		my $fork = $table !~ /^heap_/;
		for my $statement (@statements)
		{
			(my $sql = $statement) =~ s/%/$table/g;
			my $run =
				$statement eq 'vacuum'
			  ? ($fork ? "SELECT doltgres_test_vacuum('$table', 'auto')" : "VACUUM $table")
			  : $statement !~ /^SELECT / ? written($table, $statement)
			  : $fork
			  ? "$settings; SELECT doltgres_test_standing('$table', '" . ($sql =~ s/'/''/gr) . "')"
			  : "$settings; $sql";
			push @{ $outputs{$table} }, $node->safe_psql('postgres', $run) =~ s/heap_//gr;
		}
	}
	return ($outputs{$name}, $outputs{"heap_$name"});
}

my $by_index = 'SET enable_seqscan = off; SET enable_bitmapscan = off';
my $by_bitmap = 'SET enable_seqscan = off; SET enable_indexscan = off';

# Section 6's first case: a hash and a GiST index on a fork table answer their
# queries by index scan and by bitmap scan as heap's twin's do, after inserts,
# updates, and deletes, and after a VACUUM: an updated row is found by its new
# key, its old key finds nothing, and a deleted row is found by neither index.
for my $table ('scanned', 'heap_scanned')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, r int4range, v text)");
	$node->safe_psql('postgres', "CREATE INDEX ${table}_k ON $table USING hash (k)");
	$node->safe_psql('postgres', "CREATE INDEX ${table}_r ON $table USING gist (r)");
}
my @queries = (
	q{SELECT k || ' ' || v FROM % WHERE k = 105},
	q{SELECT count(*) FROM % WHERE k = 5},
	q{SELECT count(*) FROM % WHERE k = 3},
	q{SELECT string_agg(k::text, ' ' ORDER BY k) FROM % WHERE r && int4range(5, 9)});
my @writes = (
	q{INSERT INTO % SELECT g, int4range(g, g + 3), 'v' || g FROM generate_series(1, 20) g},
	q{UPDATE % SET k = k + 100 WHERE mod(k, 5) = 0},
	q{DELETE FROM % WHERE k IN (3, 7)});
for my $settings ($by_index, $by_bitmap)
{
	my ($fork, $heap) = both('scanned', $settings,
		($settings eq $by_index ? @writes : ()), @queries, 'vacuum', @queries);
	is_deeply($fork, $heap,
		"a hash and a GiST index on a fork table answer their queries as heap's twin's do, after writes and a VACUUM ("
		  . ($settings eq $by_index ? 'index scans' : 'bitmap scans') . ')');
}
like(
	$node->safe_psql('postgres', "$by_index; EXPLAIN (COSTS OFF) SELECT k FROM scanned WHERE k = 105"),
	qr/Index Scan using scanned_k/,
	'the index scans are the hash index');
like(
	$node->safe_psql('postgres',
		"$by_bitmap; EXPLAIN (COSTS OFF) SELECT k FROM scanned WHERE r && int4range(5, 9)"),
	qr/Bitmap Index Scan on scanned_r/,
	'the bitmap scans are the GiST index');

# Section 6's last case: the hash index keeps hashhandler's routine and Postgres'
# pages, whose count is its file's, as heap's twin's is.
is( $node->safe_psql('postgres',
		"SELECT doltgres_test_index_routine('scanned_k'), pg_relation_size('scanned_k') = pg_relation_size('heap_scanned_k')"
	),
	"f|t",
	"a hash index on a fork table keeps its handler's routine, and its blocks are its file's");

# Section 6: an index-only scan over a GiST index returns no row a delete
# removed, since a fork table has no visibility map and each entry's row is
# fetched by its tid, as heap's twin's scan of a page not all-visible does.
for my $table ('pointed', 'heap_pointed')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (p point)");
	$node->safe_psql('postgres', "CREATE INDEX ${table}_p ON $table USING gist (p)");
}
my ($pointed, $heap_pointed) = both(
	'pointed', $by_index,
	q{INSERT INTO % SELECT point(g, g) FROM generate_series(1, 5) g},
	q{DELETE FROM % WHERE p[0] = 3},
	q{SELECT string_agg(p::text, ' ' ORDER BY p[0]) FROM % WHERE p <@ box '((0,0),(10,10))'});
is( $pointed->[-1] . ' / ' . $heap_pointed->[-1],
	'(1,1) (2,2) (4,4) (5,5) / (1,1) (2,2) (4,4) (5,5)',
	"an index-only scan over a GiST index of a fork table returns no row a delete removed");
like(
	$node->safe_psql('postgres',
		"$by_index; EXPLAIN (COSTS OFF) SELECT p FROM pointed WHERE p <@ box '((0,0),(10,10))'"),
	qr/Index Only Scan using pointed_p/,
	'the scan is an index-only scan of the GiST index');

# Section 3's U1: every update of a fork table with an index the fork doesn't
# store gives each index the new row, so an exclusion constraint's GiST index
# checks it against the row's own old version, which the update's dirty fetch
# answers as gone: an update of a column outside the constraint succeeds, and
# one that moves a row onto another's range fails as heap's twin's does.
for my $table ('excluded', 'heap_excluded')
{
	$node->safe_psql('postgres',
		"CREATE TABLE $table (r int4range, v text, EXCLUDE USING gist (r WITH &&))");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (int4range(1, 5), 'a'), (int4range(10, 15), 'b')"));
	$node->safe_psql('postgres', written($table, "UPDATE % SET v = 'c' WHERE v = 'a'"));
}
my %refused;
for my $table ('excluded', 'heap_excluded')
{
	my ($ret, $out, $err) = $node->psql('postgres',
		written($table, "UPDATE % SET r = int4range(11, 12) WHERE v = 'c'"),
		extra_params => [ '-v', 'VERBOSITY=verbose' ]);
	$refused{$table} = join("\n",
		grep { /^(ERROR|DETAIL):/ } map { s/^psql:[^:]*:\d+: //r } split /\n/, $err)
	  =~ s/heap_//gr;
}
like(
	$refused{heap_excluded},
	qr/^ERROR:  23P01: conflicting key value violates exclusion constraint/,
	"heap's twin refuses the move with 23P01");
is($refused{excluded}, $refused{heap_excluded},
	"an update of a fork table with a GiST exclusion constraint succeeds outside it and is refused as heap's twin's when it moves a row onto another's range"
);
is( $node->safe_psql('postgres',
		q{SELECT doltgres_test_standing('excluded', 'SELECT string_agg(r::text || '' '' || v, '', '' ORDER BY r) FROM excluded')}
	),
	'[1,5) c, [10,15) b',
	"the refused update leaves the rows as the first update left them");

# Section 3's U1: an update of a column outside the primary key of a fork table
# with a hash index gives the primary key's unique index the row's own key,
# which its check finds no conflict in, and the hash index the new row.
for my $table ('keyed', 'heap_keyed')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (id int PRIMARY KEY, k int4, v text)");
	$node->safe_psql('postgres', "CREATE INDEX ${table}_k ON $table USING hash (k)");
}
my ($keyed, $heap_keyed) = both(
	'keyed', $by_index,
	q{INSERT INTO % VALUES (1, 10, 'a'), (2, 20, 'b')},
	q{UPDATE % SET v = 'c' WHERE id = 1},
	q{SELECT id || ' ' || v FROM % WHERE k = 10});
is( $keyed->[-1] . ' / ' . $heap_keyed->[-1],
	'1 c / 1 c',
	"an update of a non-key column of a fork table with a primary key and a hash index succeeds and the hash index finds the new row"
);


# ctids returns the ctids a fork table's rows matching a condition have, through
# the fork's routine, in ctid order.
sub ctids
{
	my ($table, $condition) = @_;
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('$table', 'SELECT string_agg(ctid::text, '' '' ORDER BY ctid) FROM $table WHERE $condition')"
	);
}

# in_order returns ctids in ctid order, by block and then offset.
sub in_order
{
	my @ctids = map { [ /\((\d+),(\d+)\)/ ] } map { split / / } grep { length } @_;
	return join(' ',
		map { "($_->[0],$_->[1])" }
		sort { $a->[0] <=> $b->[0] || $a->[1] <=> $b->[1] } @ctids);
}

# orev-16's R1, section 2's validate scan over blocks and deleted rows: a hash
# index built when the table's files held none of its 300 rows lacks them, and
# holds the tids of 300 more inserted after it; a third of each are deleted.
# The validation's merge inserts each live row the index lacks once, in tid
# order across blocks, and leaves the deleted rows' entries the index already
# held, which VACUUM drops.
$node->safe_psql('postgres', 'CREATE TABLE merged (k int, v text)');
$node->safe_psql('postgres',
	written('merged', "INSERT INTO % SELECT g, 'v' || g FROM generate_series(1, 300) g"));
$node->safe_psql('postgres', 'CREATE INDEX merged_k ON merged USING hash (k)');
$node->safe_psql('postgres',
	written('merged', "INSERT INTO % SELECT g, 'v' || g FROM generate_series(301, 600) g"));
my $held_deleted = ctids('merged', 'k > 300 AND k % 3 = 0');
$node->safe_psql('postgres', written('merged', 'DELETE FROM % WHERE mod(k, 3) = 0'));
my $live = ctids('merged', 'true');
$node->safe_psql('postgres', "SELECT doltgres_test_index_validate('merged', 'merged_k')");
like($live, qr/^\(0,.* \(2,/, 'the rows span more than two blocks');
is(hashed('merged_k'), in_order($live, $held_deleted),
	"a validation merges the index's tids with the rows across blocks, inserting each live row it lacks once");

# orev-16's R2: a partial hash index's validation leaves out the rows its
# predicate rejects, as heap's validate scan does.
$node->safe_psql('postgres', 'CREATE TABLE partial (k int, v text)');
$node->safe_psql('postgres',
	written('partial', "INSERT INTO % SELECT g, 'v' || g FROM generate_series(1, 10) g"));
$node->safe_psql('postgres',
	'CREATE INDEX partial_k ON partial USING hash (k) WHERE k % 2 = 0');
$node->safe_psql('postgres', "SELECT doltgres_test_index_validate('partial', 'partial_k')");
is(hashed('partial_k'), ctids('partial', 'k % 2 = 0'),
	"a partial hash index's validation leaves out the rows its predicate rejects");

# The unknown methods' text's section 6 through the real commands (orev-16's
# R4): CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY of a hash index on a
# fork table while another session inserts. The index's expression waits on an
# advisory lock at one row, so each build pauses there while the other
# session's rows commit after the build's snapshot; the validation then
# inserts each of them, and every row is indexed once. USING heap makes the
# table the fork's, whatever the suite's default.
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION paused(k int4) RETURNS int4 IMMUTABLE LANGUAGE plpgsql
	AS $$ BEGIN IF k = 2 THEN PERFORM pg_advisory_xact_lock_shared(61); END IF; RETURN k; END $$;
CREATE TABLE concurrent (k int4, v text) USING heap;
INSERT INTO concurrent SELECT g, 'v' || g FROM generate_series(1, 3) g;
});
is($node->safe_psql('postgres', q{SELECT doltgres_test_table_routine('concurrent')}),
	't', 'the concurrently indexed table is the fork\'s');
my $locker = $node->background_psql('postgres');
my $builder = $node->background_psql('postgres');
foreach my $build (
	[ 'CREATE INDEX CONCURRENTLY concurrent_k ON concurrent USING hash (paused(k))', 4 ],
	[ 'REINDEX INDEX CONCURRENTLY concurrent_k', 7 ])
{
	my ($statement, $first) = @$build;
	$locker->query_safe('SELECT pg_advisory_lock(61)');
	$builder->query_until(qr/start/, "\\echo start\n$statement;\n");
	$node->poll_query_until('postgres',
		q{SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE wait_event = 'advisory' AND query ~ 'CONCURRENTLY')}
	);
	my $last = $first + 2;
	$node->safe_psql('postgres',
		"INSERT INTO concurrent SELECT g, 'v' || g FROM generate_series($first, $last) g");
	$locker->query_safe('SELECT pg_advisory_unlock(61)');
	$builder->query_safe('SELECT 1');
	is( $node->safe_psql(
			'postgres', q{
SELECT i.indisvalid, (SELECT count(*) FROM concurrent) FROM pg_index i WHERE i.indexrelid = 'concurrent_k'::regclass}),
		"t|$last",
		"$statement while another session inserts leaves a valid index");
	is(hashed('concurrent_k'), in_order(ctids('concurrent', 'true')),
		"$statement while another session inserts indexes every row once");
}
$locker->quit;
$builder->quit;

# Section 1's build scan: a serial BRIN build reads a fork table's rows in tuple
# id order, as heap's build reads its blocks, so each block range's summary
# holds the rows of its own blocks (bugs row 196); a parallel GIN build's
# workers each take the shared scan's units in turn and end their scans, as
# heap's build ends the scan it is given, so the index holds every row and no
# worker leaves a resource open.
$node->safe_psql(
	'postgres', q{
SET max_parallel_maintenance_workers = 0;
CREATE TABLE ranged (i int8, j int2);
INSERT INTO ranged SELECT g, g % 100 FROM generate_series(1, 1000) g;
CREATE INDEX ranged_ij ON ranged USING brin (i, j) WITH (pages_per_range = 1);
});
is( $node->safe_psql(
		'postgres',
		'SET enable_seqscan = off; SELECT count(*) FROM ranged WHERE i > 900'),
	'100',
	"a BRIN index built over a fork table finds every row of its block ranges"
);
my ($ret, $out, $err) = $node->psql(
	'postgres', q{
CREATE TABLE arrays (j int[]);
INSERT INTO arrays SELECT ARRAY[g % 10, g] FROM generate_series(1, 5000) g;
ALTER TABLE arrays SET (parallel_workers = 2);
SET min_parallel_table_scan_size = 0;
CREATE INDEX arrays_j ON arrays USING gin (j);
SET enable_seqscan = off;
SELECT count(*) FROM arrays WHERE j @> ARRAY[3];
});
is("$out|$err", '500|',
	"a parallel GIN build over a fork table indexes every row, its workers ending their scans"
);

$node->stop;
done_testing();
