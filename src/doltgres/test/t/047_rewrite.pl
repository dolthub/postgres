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

# The table rewrite (the rewrite text, out/review/macpf-table-rewrite-design.md):
# the storage swaps of the fork's table access method routine.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('rewrite');
$node->init(allows_streaming => 1);
# No fold worker and no scheduled collection writes a pack of its own, so a case
# reads exactly the packs its folds and its asked collection write.
$node->append_conf('postgresql.conf', "fold_threshold = 1GB\nfold_timeout = 3600s\ncollector_naptime = 3600s");
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test; CALL doltgres_test_twins();
CREATE TABLE t (id int4 PRIMARY KEY);
CREATE TABLE src (id int4);
CREATE FUNCTION w(n int4) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DELETE FROM src;
	INSERT INTO src VALUES (n);
	PERFORM doltgres_test_insert('t', 'src', false, 0);
END $$;
});
my $ids = q{SELECT coalesce(string_agg(substring(r from '^\S+ \S+ (.*)$'), ','), '') FROM doltgres_test_scan('t', false, -1) r};

# A new relfilenode, as a transactional TRUNCATE gives one, reads empty; one
# its transaction made is emptied in place, which reads empty and then reads
# the rows written after, and a crash replays the emptying between them.
my $truncated = $node->safe_psql(
	'postgres', qq{
	DO \$\$ BEGIN PERFORM w(1); END \$\$;
	BEGIN;
	SELECT doltgres_test_new_file('t');
	$ids;
	DO \$\$ BEGIN PERFORM w(2); PERFORM w(3); END \$\$;
	$ids;
	SELECT doltgres_test_truncate('t');
	$ids;
	DO \$\$ BEGIN PERFORM w(4); END \$\$;
	$ids;
	COMMIT;
});
is($truncated, "\n\n2,3\n\n\n4",
	'a table made and truncated in one transaction reads empty, then reads the rows written after');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $ids), '4', "a crash replays a storage's emptying before the rows written after it");

# written returns the SQL that runs a statement over a table, "%" standing for its
# name: an insert, an update, a delete, or a select through the fork's routine
# for a fork table and through heap's for its heap twin, and any other as it is.
sub written
{
	my ($table, $statement) = @_;
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	(my $sql = $statement) =~ s/%/$table/g;
	return $sql if $statement !~ /^(INSERT|UPDATE|DELETE|SELECT) /;
	$sql =~ s/'/''/g;
	return "SELECT doltgres_test_write('$table', $fork, '$sql')";
}

# rows returns the SQL that runs a query over a table, "%" standing for its
# name, by default a read of its rows with their ctids in ctid order, through
# the fork's routine for a fork table.
sub rows
{
	my ($table, $given) = @_;
	my $query = ($given // "SELECT string_agg(ctid::text || %::text, ' ' ORDER BY ctid) FROM %") =~ s/%/$table/gr;
	return $table =~ /^heap_/
	  ? $query
	  : "SELECT doltgres_test_standing('$table', '" . ($query =~ s/'/''/gr) . "')";
}

# rewritten returns the SQL that rewrites a table as CLUSTER on an index, or as
# VACUUM FULL with none, with CLUSTER's sort or without, through the fork's
# routine for a fork table, giving the copy's counts.
sub rewritten
{
	my ($table, $index, $sorted) = @_;
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	my $on = defined $index ? "'" . ($index =~ s/%/$table/gr) . "'" : 'NULL';
	return "SELECT doltgres_test_rewrite('$table', $on, $sorted, $fork)";
}

# stepped returns the SQL of a step over a table: "rows" for a read of its rows,
# "read <query>" for a query over it, "rewrite [index] [sorted]" for its
# rewrite, and any other a statement over it.
sub stepped
{
	my ($table, $step) = @_;
	return
		$step eq 'rows' ? rows($table)
	  : $step =~ /^read (.*)$/s ? rows($table, $1)
	  : $step =~ /^rewrite ?(\S*) ?(\S*)$/
	  ? rewritten($table, $1 eq '' ? undef : $1, $2 eq 'sorted' ? 'true' : 'false')
	  : written($table, $step);
}

# twins runs steps over a fork table and its heap twin, made by a definition:
# each one or more statements apart by "; " over "%" (stepped's), with
# "rows" for a read of its rows, "rewrite [index]
# [sorted]" for its rewrite, "hold" for another session's REPEATABLE READ
# snapshot taken without reading the table, "held rows" for that session's read
# of its rows, "held read <query>" for its query, "release" to end it, or "@<name> <statement>" for a statement in
# another session of that name, made at its first, "@<name> quit" ending it, a
# step after "heap: " or "fork: " run on that table alone with its output set
# aside, and a step after "counts <fork's>/<heap's>: " whose rewrite counts are
# each table's own, a line of its output each must give; it checks that the
# fork's outputs are heap's.
sub twins
{
	my ($name, $definition, $case, @steps) = @_;
	my %outputs;
	my ($held, %sessions);
	for my $table ($name, "heap_$name")
	{
		$node->safe_psql('postgres', "CREATE TABLE $table $definition");
		for my $given (@steps)
		{
			my ($counts, $rest) = $given =~ m{^counts ([^/]*)/([^:]*): }s ? ([$1, $2], $') : (undef, $given);
			my ($only, $step) = $rest =~ /^(heap: |fork: )?(.*)$/s;
			push @{ $outputs{$table} }, '' and next
			  if $only && ($only eq 'heap: ') != ($table =~ /^heap_/);
			if ($step =~ /^@(\w+) (.*)$/s)
			{
				my ($name, $statement) = ($1, $2);
				$sessions{$name} //= $node->background_psql('postgres');
				if ($statement eq 'quit')
				{
					delete($sessions{$name})->quit;
				}
				else
				{
					$sessions{$name}->query_safe(join('; ', map { stepped($table, $_) } split /; /, $statement));
				}
				push @{ $outputs{$table} }, '';
				next;
			}
			if ($step eq 'hold')
			{
				$held = $node->background_psql('postgres');
				$held->query_safe(
					'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
				push @{ $outputs{$table} }, '';
				next;
			}
			if ($step =~ /^held (rows|read (.*))$/s || $step eq 'release')
			{
				my $sql =
					$step eq 'release' ? 'COMMIT'
				  : $step eq 'held rows' ? rows($table)
				  :                        rows($table, $2);
				push @{ $outputs{$table} }, $held->query_safe($sql) =~ s/heap_//gr;
				$held->quit if $step eq 'release';
				next;
			}
			my $sql = join('; ', map { stepped($table, $_) } split /; /, $step);
			my $output = $node->safe_psql('postgres', $sql) =~ s/heap_//gr;
			if ($counts)
			{
				my $own = $counts->[ $table =~ /^heap_/ ? 1 : 0 ];
				$output =~ s/^\Q$own\E$/counts as their own/m or $output = "counts other than $own: $output";
			}
			push @{ $outputs{$table} }, $only ? '' : $output;
		}
	}
	is_deeply($outputs{$name}, $outputs{"heap_$name"}, $case);
}

# Section 4's VACUUM FULL: rows of growing widths over several pages under a
# fill factor, some deleted, are given heap's tids in old tid order, with heap's
# counts; the heap twin's VACUUM first takes out the dead rows the fork's fold
# applies.
twins(
	'packed',
	'(k int4 PRIMARY KEY, v text) WITH (fillfactor = 70)',
	"VACUUM FULL gives a table's rows the tids heap's rewrite gives them",
	"INSERT INTO % SELECT g, repeat('x', g * 5) FROM generate_series(1, 300) g",
	'DELETE FROM % WHERE mod(k, 7) = 0',
	'rows',
	'heap: VACUUM %',
	'rewrite',
	'rows');

# Section 4's CLUSTER: on a btree with ties, by CLUSTER's own sort fed in old tid
# order and by the btree's order, its key and then the old tid, rewriting the
# rewritten table again; and on a gist index, in the order its scan gives.
twins(
	'clustered',
	'(k int4 PRIMARY KEY, w int4, v text) WITH (fillfactor = 80)',
	"CLUSTER gives a table's rows the tids heap's rewrite gives them in each of its orders",
	'CREATE INDEX %_w ON % (w DESC)',
	"INSERT INTO % SELECT g, mod(g * 37, 11), repeat('y', g * 3) FROM generate_series(1, 200) g",
	'DELETE FROM % WHERE mod(k, 5) = 0',
	'heap: VACUUM %',
	'rewrite %_w sorted',
	'rows',
	'rewrite %_w',
	'rows');
twins(
	'scanned',
	'(k int4, p point, v text)',
	"CLUSTER on a gist index gives a table's rows the tids heap's rewrite gives them",
	'CREATE INDEX %_p ON % USING gist (p)',
	"INSERT INTO % SELECT g, point(mod(g * 13, 17), mod(g * 7, 19)), repeat('z', g * 2) FROM generate_series(1, 150) g",
	'DELETE FROM % WHERE mod(k, 4) = 0',
	'heap: VACUUM %',
	'rewrite %_p',
	'rows');

# Section 4's recently dead versions: updates an older snapshot doesn't see keep
# their older versions, each written right after its newer one, and the older
# snapshot reads them after the rewrite; the delta's versions are carried. The
# fill factor leaves each update room on its own page (bugs row 87).
twins(
	'chained',
	'(k int4 PRIMARY KEY, v text) WITH (fillfactor = 50)',
	"VACUUM FULL keeps an updated row's older versions for an older snapshot, each after its newer one, as heap's rewrite does",
	"INSERT INTO % SELECT g, repeat('c', g * 4) FROM generate_series(1, 120) g",
	'hold',
	"UPDATE % SET v = v || 'u' WHERE mod(k, 3) = 0",
	"UPDATE % SET v = v || 'w' WHERE mod(k, 9) = 0",
	'rows',
	'rewrite',
	'rows',
	'held rows',
	'release',
	'heap: VACUUM %',
	'rewrite',
	'rows');

# Section 4's rows of the rewriting transaction: its own inserts keep their
# cmin, and its aborted subtransaction's insert and an earlier aborted insert are
# dropped. The earlier one is taken back at its abort, as the heap twin's VACUUM
# takes it out, and the fold drops the subtransaction's at its install, so the
# fork counts no removable version where heap counts it (Differences row 46).
twins(
	'own',
	'(k int4 PRIMARY KEY, v text)',
	"VACUUM FULL keeps the rewriting transaction's rows with their cmin and drops aborted inserts, as heap's rewrite does",
	"INSERT INTO % SELECT g, repeat('o', g * 9) FROM generate_series(1, 30) g",
	"BEGIN; INSERT INTO % VALUES (31, 'a'); ROLLBACK",
	'heap: VACUUM %',
	"counts 40 0 0/40 1 0: BEGIN; INSERT INTO % SELECT g, repeat('n', g) FROM generate_series(32, 40) g; SAVEPOINT s; INSERT INTO % VALUES (41, 'r'); ROLLBACK TO s; INSERT INTO % VALUES (42, 'm'); rewrite; read SELECT string_agg(ctid::text || cmin::text || k::text, ' ' ORDER BY ctid) FROM %; COMMIT",
	'rows');

# Section 4's re-formed rows: a column added with a default is materialized, as
# reform_and_rewrite_tuple forms the rows heap's packing counts. A dropped
# column's case waits on bugs row 88 (pending.tsv).
twins(
	'reformed',
	'(k int4 PRIMARY KEY, v text)',
	"VACUUM FULL packs rows as heap re-forms them, an added column's default stored",
	"INSERT INTO % SELECT g, repeat('v', g * 3) FROM generate_series(1, 150) g",
	"ALTER TABLE % ADD COLUMN w text DEFAULT 'filled in'",
	'rewrite',
	'rows');

# Section 4's re-formed carried rows: two columns added with defaults after an
# older snapshot's updates, then CLUSTER and, once the snapshot ends, VACUUM
# FULL, every column read, the carried recently dead rows by the older
# snapshot, with heap's missing values. The older snapshot's read holds its lock,
# so the second rewrite comes after it ends; the sort's keys are unique, which
# keeps it heap's after columns were added (Differences row 49).
twins(
	'added',
	'(k int4 PRIMARY KEY, w int4, v text) WITH (fillfactor = 50)',
	"a rewrite stores added columns' defaults in base and carried rows alike, as heap's re-forms them",
	'CREATE INDEX %_w ON % (w, k)',
	"INSERT INTO % SELECT g, mod(g, 7), repeat('a', g * 2) FROM generate_series(1, 90) g",
	'hold',
	"UPDATE % SET v = v || 'u' WHERE mod(k, 4) = 0",
	'ALTER TABLE % ADD COLUMN c int4 DEFAULT 7',
	"ALTER TABLE % ADD COLUMN d text DEFAULT 'dd'",
	'rewrite %_w sorted',
	'rows',
	'held rows',
	'release',
	'heap: VACUUM %',
	'rewrite',
	'rows',
	"read SELECT string_agg(attname || ':' || atthasmissing::text, ' ' ORDER BY attnum) FROM pg_attribute WHERE attrelid = '%'::regclass AND attnum > 0");

# Section 4's DELETE and INSERT of one key in one transaction, under an older
# snapshot: the deleted row is kept for it, with no successor. The reinsert is
# wider than any page's leftover room (bugs row 87).
twins(
	'reinserted',
	'(k int4 PRIMARY KEY, v text)',
	"VACUUM FULL keeps a key's deleted row for an older snapshot beside its reinsert, as heap's rewrite does",
	"INSERT INTO % SELECT g, repeat('r', g * 3) FROM generate_series(1, 60) g",
	'hold',
	"BEGIN; DELETE FROM % WHERE k = 5; INSERT INTO % VALUES (5, repeat('again', 100)); COMMIT",
	'rewrite',
	'rows',
	'held rows',
	'release');

# Section 4's keyless table: duplicate rows are counted copies of one entry,
# some deleted, each kept copy given heap's tid.
twins(
	'keyless',
	'(k int4, v text)',
	"VACUUM FULL gives a keyless table's duplicate rows heap's tids",
	"INSERT INTO % SELECT mod(g, 10), repeat('k', mod(g, 10) * 20) FROM generate_series(1, 80) g",
	'DELETE FROM % WHERE k = 3',
	'heap: VACUUM %',
	'rewrite',
	'rows');

# Differences row 48: CLUSTER on a non-unique btree without its sort orders equal
# keys by each version's own old tid, where heap's index scan returns a HOT
# chain whole, so a HOT-updated row comes after the row sharing its key.
my %tied;
for my $table ('tied', 'heap_tied')
{
	$node->safe_psql('postgres',
		"CREATE TABLE $table (k int4 PRIMARY KEY, w int4, v text) WITH (fillfactor = 50); CREATE INDEX ${table}_w ON $table (w)"
	);
	$node->safe_psql('postgres', written($table, $_))
	  for ("INSERT INTO % VALUES (1, 1, 'a'), (2, 1, 'b')", "UPDATE % SET v = 'a2' WHERE k = 1");
	$node->safe_psql('postgres', "VACUUM $table") if $table =~ /^heap_/;
	$node->safe_psql('postgres', rewritten($table, "${table}_w", 'false'));
	$tied{$table} = $node->safe_psql('postgres', rows($table)) =~ s/heap_//gr;
}
is($tied{tied}, '(0,1)(2,1,b) (0,2)(1,1,a2)',
	"CLUSTER without a sort orders a non-unique btree's equal keys by old tid (Differences row 48)");
is($tied{heap_tied}, '(0,1)(1,1,a2) (0,2)(2,1,b)', "heap's CLUSTER returns the HOT chain whole");

# The fold text's draft 12: an open xid with no snapshot holds OldestXmin back,
# so heap keeps a delete's rows as recently dead, and the fork, whose fold
# applies no transaction at or past that horizon, keeps them too, with heap's
# ctids, counts, and reltuples; the table has no index, whose rebuild would count
# its live rows.
my $open = $node->background_psql('postgres');
$open->query_safe('BEGIN; SELECT txid_current()');
my %window;
for my $table ('gap', 'heap_gap')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres', written($table, $_))
	  for ("INSERT INTO % SELECT g, repeat('w', g) FROM generate_series(1, 6) g", 'DELETE FROM % WHERE k <= 2');
	$window{$table} = join(' | ',
		$node->safe_psql('postgres', rewritten($table, undef, 'false')),
		$node->safe_psql('postgres', rows($table)) =~ s/heap_//gr,
		$node->safe_psql('postgres', "SELECT reltuples FROM pg_class WHERE relname = '$table'"));
}
$open->query_safe('COMMIT');
$open->quit;
is($window{heap_gap}, '6 0 2 | (0,3)(3,www) (0,4)(4,wwww) (0,5)(5,wwwww) (0,6)(6,wwwwww) | 6',
	"heap keeps a delete's rows as recently dead while an open xid holds OldestXmin back");
is($window{gap}, $window{heap_gap}, "and the fork's rewrite keeps them as heap's does");

# Section 4's redo: a crash after a rewrite whose new storage carries versions
# reads its rows as before, the base's record and the carried rows replayed.
$node->safe_psql('postgres', 'CREATE TABLE crashed (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50)');
$node->safe_psql('postgres', written('crashed', "INSERT INTO % SELECT g, repeat('c', g) FROM generate_series(1, 40) g"));
my $holding = $node->background_psql('postgres');
$holding->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
$node->safe_psql('postgres', written('crashed', "UPDATE % SET v = v || 'x' WHERE mod(k, 3) = 0"));
$node->safe_psql('postgres', rewritten('crashed', undef, 'false'));
my $before = $node->safe_psql('postgres', rows('crashed'));
$holding->quit;
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', rows('crashed')), $before, "a crash after a rewrite with carried versions reads its rows as before");

# Differences row 49: CLUSTER's sort is fed each row's canonical form, so after
# ADD COLUMN a sort that spills may return equal keys in another order than
# heap's; the keys come back in heap's order, compared without their rows.
twins(
	'spilled',
	'(k int4 PRIMARY KEY, w int4, v text)',
	"CLUSTER's spilling sort after ADD COLUMN returns its keys in heap's order (Differences row 49)",
	'CREATE INDEX %_w ON % (w)',
	"INSERT INTO % SELECT g, mod(g * 7, 13), repeat('s', mod(g, 30)) FROM generate_series(1, 3000) g",
	'ALTER TABLE % ADD COLUMN x int4 DEFAULT 1',
	"SET maintenance_work_mem = '64kB'; rewrite %_w sorted",
	"read SELECT string_agg(w::text, ' ' ORDER BY ctid) FROM %");

# Section 4's standby: a standby made before a rewrite replays the new
# storage's base record and its carried versions, and reads the table's rows as
# the primary does: the rows folded before the older snapshot are base copies,
# its updates carried.
$node->backup('rewrite_backup');
my $standby = PostgreSQL::Test::Cluster->new('rewrite_standby');
$standby->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$standby->start;
$node->safe_psql('postgres', 'CREATE TABLE followed (k int4 PRIMARY KEY, v text) WITH (fillfactor = 50)');
$node->safe_psql('postgres', written('followed', "INSERT INTO % SELECT g, repeat('f', g) FROM generate_series(1, 40) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('followed')");
my $older = $node->background_psql('postgres');
$older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
$node->safe_psql('postgres', written('followed', "UPDATE % SET v = v || 'y' WHERE mod(k, 4) = 0"));
$node->safe_psql('postgres', rewritten('followed', undef, 'false'));
$older->quit;
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres', rows('followed')), $node->safe_psql('postgres', rows('followed')),
	"a standby reads a rewritten table's rows as the primary does");
$node->safe_psql('postgres', 'CREATE INDEX followed_v ON followed (v)');
$node->safe_psql('postgres', rewritten('followed', 'followed_v', 'true'));
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres', rows('followed')), $node->safe_psql('postgres', rows('followed')),
	"a standby reads a clustered table's rows as the primary does");
$standby->stop;

# Section 4's recovery from a base backup taken before the rewrites: a server
# restored from it replays the built bases' records and, once promoted, reads
# the rewritten table as the primary does.
my $restored = PostgreSQL::Test::Cluster->new('rewrite_restored');
$restored->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$restored->start;
$node->wait_for_catchup($restored);
$restored->promote;
is($restored->safe_psql('postgres', rows('followed')), $node->safe_psql('postgres', rows('followed')),
	"a server restored from a backup taken before a rewrite reads the table as the primary does");
$restored->stop;

# Section 4's crash between a built base's Chunk records and its base record:
# recovery replays the records with no base and no commit, so the table reads
# its rows as before the rewrite.
$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS injection_points');
$node->safe_psql('postgres', 'CREATE TABLE interrupted (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres', written('interrupted', "INSERT INTO % SELECT g, repeat('n', g) FROM generate_series(1, 30) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('interrupted')");
my $unrewritten = $node->safe_psql('postgres', rows('interrupted'));
$node->safe_psql('postgres', "SELECT injection_points_attach('doltgres-built-logged', 'wait')");
my $rewriting = $node->background_psql('postgres');
$rewriting->query_until(qr/start/, "\\echo start\n" . rewritten('interrupted', undef, 'false') . ";\n");
$node->wait_for_event('client backend', 'doltgres-built-logged');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', rows('interrupted')), $unrewritten,
	"a crash between a built base's chunk records and its base record reads the table as before the rewrite");

# Section 4's xid inversion (W2): an update ends a row, and an older transaction
# deletes the updated row; with OldestXmin between their xids, the deleted row
# is dead and the updated one recently dead. In old tid order heap drops both
# (rewrite_heap_dead_tuple); on an index where the deleted row comes first, the
# updated one is held to the end (end_heap_rewrite).
for my $order ('rewrite', 'rewrite %_w')
{
	twins(
		$order eq 'rewrite' ? 'inverted' : 'inverted_last',
		'(k int4 PRIMARY KEY, w int4, v text) WITH (fillfactor = 50)',
		$order eq 'rewrite'
		? "VACUUM FULL drops an updated row with its dead successor, as heap's rewrite does"
		: "CLUSTER writes an updated row whose successor is dead at the end, as heap's rewrite does",
		'CREATE INDEX %_w ON % (w)',
		"INSERT INTO % SELECT g, g + 10, repeat('i', g * 3) FROM generate_series(1, 30) g",
		"fork: SELECT doltgres_test_fold('%')",
		'@older BEGIN; SELECT txid_current()',
		'@between BEGIN; SELECT txid_current()',
		"UPDATE % SET w = 1, v = 'updated' WHERE k = 5",
		'@older DELETE FROM % WHERE k = 5; COMMIT',
		'hold',
		"UPDATE % SET v = v || 't' WHERE k = 9",
		$order,
		'rows',
		'held rows',
		'release',
		'@between COMMIT',
		'@between quit',
		'@older quit');
}

# Section 4's TOAST (§7): rows with values out of line, half updated under an
# older snapshot so both versions name each value, keep their value ids under
# the swap by content, and both snapshots read them after the rewrite.
my $wide = "(SELECT string_agg(md5(i::text || g::text), '') FROM generate_series(1, 200) i)";
my $toasted = "read SELECT string_agg(ctid::text || ':' || k || ':' || w || ':' || md5(v), ' ' ORDER BY ctid) FROM %";
twins(
	'toasted',
	'(k int4 PRIMARY KEY, w int4, v text) WITH (fillfactor = 50)',
	"VACUUM FULL keeps a table's values out of line for every version that names them, as heap's rewrite does",
	"INSERT INTO % SELECT g, g, $wide FROM generate_series(1, 8) g",
	'hold',
	'UPDATE % SET w = w + 100 WHERE mod(k, 2) = 0',
	'rewrite',
	$toasted,
	"held $toasted",
	'release');

# Section 4's TOAST (§7, PRevPF 8's case): a column added with a default wider
# than the TOAST threshold moves each re-formed row's default out of line under
# a value id of its own, base and carried rows alike, as heap's rewrite toasts it.
srand(47);
my $noise = join('', map { sprintf('%x', int(rand(16))) } 1 .. 3000);
twins(
	'widened',
	'(k int4 PRIMARY KEY, w int4) WITH (fillfactor = 50)',
	"VACUUM FULL moves an added wide default out of line for each row, as heap's rewrite does",
	"INSERT INTO % SELECT g, g FROM generate_series(1, 6) g",
	'hold',
	'UPDATE % SET w = w + 100 WHERE mod(k, 2) = 0',
	"ALTER TABLE % ADD COLUMN d text DEFAULT '$noise'",
	'rewrite',
	"read SELECT string_agg(ctid::text || ':' || k || ':' || w || ':' || md5(d), ' ' ORDER BY ctid) FROM %",
	"held read SELECT string_agg(ctid::text || ':' || k || ':' || w || ':' || md5(d), ' ' ORDER BY ctid) FROM %",
	'release',
	"read SELECT count(DISTINCT pg_column_toast_chunk_id(d)) FROM %");

# Section 4's TOAST on a base copy (bugs row 98's value ids): a column added with
# a default wider than the TOAST threshold moves a folded row's default out of
# line under a value id of its own, as a delta row's, and a second rewrite keeps
# each value out of line, as heap's rewrites do; the first rewrite's fold applies
# the update, so its counts are each one's own (Differences row 46).
twins(
	'folded_wide',
	'(k int4 PRIMARY KEY, w int4)',
	"VACUUM FULL moves a base row's added wide default out of line and keeps it there, as heap's rewrite does",
	"INSERT INTO % SELECT g, g FROM generate_series(1, 4) g",
	"fork: SELECT doltgres_test_fold('%')",
	'UPDATE % SET w = w + 100 WHERE k = 2',
	"ALTER TABLE % ADD COLUMN d text DEFAULT '$noise'",
	'counts 4 0 0/4 1 0: rewrite',
	"read SELECT string_agg(ctid::text || ':' || k || ':' || w || ':' || md5(d), ' ' ORDER BY ctid) FROM %",
	"read SELECT count(DISTINCT pg_column_toast_chunk_id(d)) FROM %",
	"fork: SELECT doltgres_test_fold('%')",
	'rewrite',
	"read SELECT string_agg(ctid::text || ':' || k || ':' || w || ':' || md5(d), ' ' ORDER BY ctid) FROM %",
	"read SELECT count(DISTINCT pg_column_toast_chunk_id(d)) FROM %");

# The fold text's draft 12: with an open xid holding OldestXmin back, a rewrite
# keeps the rows a delete ended as recently dead, as heap's does, so the
# horizon its base record carries (BASE_ADVANCED field 11), its maxFolded,
# cancels no hot standby snapshot taken before the delete, which reads them.
my $conflicted = PostgreSQL::Test::Cluster->new('rewrite_conflicted');
$conflicted->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$conflicted->append_conf('postgresql.conf', 'max_standby_streaming_delay = 0');
$conflicted->start;
$node->safe_psql('postgres', 'CREATE TABLE gone (k int4, v text)');
$node->safe_psql('postgres', written('gone', "INSERT INTO % SELECT g, repeat('g', g) FROM generate_series(1, 6) g"));
my $opened = $node->background_psql('postgres');
$opened->query_safe('BEGIN; SELECT txid_current()');
$node->wait_for_catchup($conflicted);
my $reader = $conflicted->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
my $offset = -s $conflicted->logfile;
$node->safe_psql('postgres', written('gone', 'DELETE FROM % WHERE k <= 2'));
$node->safe_psql('postgres', rewritten('gone', undef, 'false'));
$node->wait_for_catchup($conflicted);
is($reader->query_safe(rows('gone', 'SELECT count(*) FROM %')), '6',
	"a standby snapshot taken before a rewrite's delete reads its rows after the rewrite replays, as on heap");
unlike(slurp_file($conflicted->logfile, $offset), qr/conflict with recovery/, 'and the replay cancels it for no conflict');
$reader->quit;
$opened->query_safe('COMMIT');
$opened->quit;
$conflicted->stop;

# Section 4's install of a built base: inserts after VACUUM FULL and after CLUSTER
# take the tids heap's take gives them, past every row the rewrite kept, the first
# on the last page while it has room; and after a crash replays the rewrite, an
# insert takes no tid a kept row holds.
twins(
	'appended',
	'(k int4 PRIMARY KEY, v text)',
	"inserts after a rewrite take the tids heap's take gives them",
	"INSERT INTO % SELECT g, repeat('a', g * 7) FROM generate_series(1, 80) g",
	'DELETE FROM % WHERE mod(k, 3) = 0',
	'heap: VACUUM %',
	'rewrite',
	"INSERT INTO % SELECT g, repeat('b', g) FROM generate_series(81, 120) g",
	'rows',
	'rewrite %_pkey',
	"INSERT INTO % SELECT g, repeat('c', g) FROM generate_series(121, 140) g",
	'rows');
$node->safe_psql('postgres', 'CREATE TABLE replayed (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres',
	written('replayed', "INSERT INTO % SELECT g, repeat('r', g * 7) FROM generate_series(1, 60) g"));
$node->safe_psql('postgres', rewritten('replayed', undef, 'false'));
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres',
	written('replayed', "INSERT INTO % SELECT g, repeat('s', g) FROM generate_series(61, 90) g"));
is( $node->safe_psql('postgres', rows('replayed', "SELECT count(*) || ' ' || count(DISTINCT ctid) FROM %")),
	'90 90', "a crash replays a built base's tuple id counter, so an insert after it takes no kept row's tid");

# Differences row 46's field 11 (BASE_ADVANCED, the base's MAX_ENDED): a rewrite
# of a table whose folds applied only inserts, newer than an open xid that holds
# the removable horizon back, leaves out no row, so a hot standby's snapshot
# taken before it isn't cancelled, as heap's rewrite cancels none.
my $kept_standby = PostgreSQL::Test::Cluster->new('rewrite_kept');
$kept_standby->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$kept_standby->append_conf('postgresql.conf', 'max_standby_streaming_delay = 0');
$kept_standby->start;
$node->safe_psql('postgres', 'CREATE TABLE inserted (k int4, v text)');
my $xid_open = $node->background_psql('postgres');
$xid_open->query_safe('BEGIN; SELECT txid_current()');
$node->safe_psql('postgres',
	written('inserted', "INSERT INTO % SELECT g, repeat('i', g) FROM generate_series(1, 6) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('inserted')");
$node->wait_for_catchup($kept_standby);
my $kept_reader = $kept_standby->background_psql('postgres', on_error_stop => 0);
$kept_reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
$node->safe_psql('postgres', rewritten('inserted', undef, 'false'));
$node->wait_for_catchup($kept_standby);
ok(eval { $kept_reader->query_safe('SELECT 1'); 1 },
	"a rewrite that leaves out no row cancels no standby snapshot (Differences row 46's field 11)");
eval { $kept_reader->quit };
$xid_open->query_safe('COMMIT');
$xid_open->quit;
$kept_standby->stop;

# PRevPF 8's C1 (P2: an end is carried only for a kept row its version ended): row
# 1 is deleted by an xid above an open one, so it is kept as recently dead, and
# its key's reinsert is deleted by an older xid, below the open one, so the
# rewrite drops it; a snapshot in another database keeps the fold from applying
# the versions without holding the table's horizon. The counts and ctids are
# heap's, and the new storage holds no delete by the reinsert's writer.
$node->safe_psql('postgres', 'CREATE DATABASE elsewhere');
my $elsewhere = $node->background_psql('elsewhere');
$elsewhere->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
twins(
	'redropped',
	'(k int4 PRIMARY KEY, v text)',
	"a rewrite carries no end for a dropped reinsert of a recently dead row's key, as heap's rewrite keeps none",
	'@older BEGIN; SELECT txid_current()',
	'@open BEGIN; SELECT txid_current()',
	"INSERT INTO % VALUES (1, 'b'), (2, 'live')",
	'DELETE FROM % WHERE k = 1',
	"INSERT INTO % VALUES (1, 'again')",
	'@older DELETE FROM % WHERE k = 1; COMMIT',
	'rewrite',
	'rows',
	'@open COMMIT',
	'@older quit',
	'@open quit');
is($node->safe_psql('postgres', "SELECT doltgres_test_changes('redropped')"),
	'put delete; put', "a rewrite carries the recently dead row and its own delete, and no delete by the reinsert's writer");
$elsewhere->query_safe('COMMIT');
$elsewhere->quit;

# The fold text's draft 12 (Differences row 54, narrowed): a hot standby's
# REPEATABLE READ snapshot taken before a table's rows replayed may not read the
# base a rewrite then installed, so the rewrite's base record carries that base's
# maxFolded as its conflict horizon (BASE_ADVANCED field 11), and its replay
# cancels each such session with Postgres' snapshot conflict, counted as
# Postgres counts one; a newer snapshot reads the rewritten table by a serial and
# a parallel scan. On the primary, an older snapshot taken after the rows' fold
# reads every row by a parallel scan after the rewrite.
my $conflicting = PostgreSQL::Test::Cluster->new('conflicting_standby');
$conflicting->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$conflicting->append_conf('postgresql.conf', 'max_standby_streaming_delay = 0');
$conflicting->start;
$node->safe_psql('postgres', 'CREATE TABLE conflicted (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres', 'CREATE TABLE conflict_other (k int4 PRIMARY KEY)');
$node->safe_psql('postgres', written('conflict_other', 'INSERT INTO % VALUES (1)'));
$node->wait_for_catchup($conflicting);
my @conflict_snapshots = map { $conflicting->background_psql('postgres', on_error_stop => 0) } 1 .. 2;
$_->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1') for @conflict_snapshots;
my $primary_snapshot = $node->background_psql('postgres');
$node->safe_psql('postgres', written('conflicted', "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('conflicted')");
$primary_snapshot->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres', written('conflict_other', 'INSERT INTO % VALUES (2)'));
my $conflict_offset = -s $conflicting->logfile;
$node->safe_psql('postgres', rewritten('conflicted', undef, 'false'));
$node->wait_for_catchup($conflicting);
my $parallel = q{SELECT string_agg(substring(r from '^\S+ \S+ (.*)$'), ', ' ORDER BY r) FROM doltgres_test_parallel('conflicted', 2, false, 0, NULL, NULL) r};
is($primary_snapshot->query_safe($parallel), '1 a, 2 b',
	"an older primary snapshot's parallel scan after a rewrite reads every row");
$primary_snapshot->query_safe('COMMIT');
$primary_snapshot->quit;
ok($conflicting->wait_for_log(qr/terminating connection due to conflict with recovery/, $conflict_offset),
	"the replay of a rewrite cancels a standby session whose snapshot may not read its base");
like(slurp_file($conflicting->logfile, $conflict_offset),
	qr/User query might have needed to see row versions that must be removed\./,
	"and the cancel carries Postgres' snapshot conflict's detail");
eval { $_->quit } for @conflict_snapshots;
$conflicting->poll_query_until('postgres',
	"SELECT confl_snapshot >= 2 FROM pg_stat_database_conflicts WHERE datname = 'postgres'")
  or die 'the conflicts were never counted';
pass("the standby counts both conflicts in pg_stat_database_conflicts");
is($conflicting->safe_psql('postgres', rows('conflicted', 'SELECT count(*) FROM %')),
	'2', "a newer standby snapshot reads the rewritten table");
is($conflicting->safe_psql('postgres', $parallel), '1 a, 2 b',
	"a newer standby snapshot's parallel scan reads every row of the rewritten table");
$conflicting->stop;

# CommPF 8's ruling on concurrent built bases: two tables of the same rows under
# other stamps, rewritten at once, log Chunk records of one address with other
# stamps, interleaved, and a standby's rebuild of each finds its own, so each
# reads on the standby with its rows' tids and xmins as on the primary.
my $concurrent = PostgreSQL::Test::Cluster->new('concurrent_standby');
$concurrent->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$concurrent->start;
for my $table ('twin_first', 'twin_second')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 PRIMARY KEY, v text)");
	$node->safe_psql('postgres', written($table, "INSERT INTO % SELECT g, 't' || g FROM generate_series(1, 2000) g"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
}
my $first = $node->background_psql('postgres');
$first->query_safe('SELECT injection_points_set_local()');
$first->query_safe("SELECT injection_points_attach('doltgres-built-logged', 'wait')");
$first->query_until(qr/start/, "\\echo start\n" . rewritten('twin_first', '%_pkey', 'false') . ";\n");
$node->wait_for_event('client backend', 'doltgres-built-logged');
$node->safe_psql('postgres', rewritten('twin_second', '%_pkey', 'false'));
$node->safe_psql('postgres', "SELECT injection_points_wakeup('doltgres-built-logged')");
$first->query_safe("SELECT injection_points_detach('doltgres-built-logged')");
$first->quit;
$node->wait_for_catchup($concurrent);
my $stamped = sub { rows($_[0], "SELECT count(*) || ' ' || min(xmin::text) || ' ' || max(xmin::text) FROM %") };
for my $table ('twin_first', 'twin_second')
{
	is($concurrent->safe_psql('postgres', $stamped->($table)),
		$node->safe_psql('postgres', $stamped->($table)),
		"a standby rebuilds $table from its own Chunk records when another table's of the same rows are logged between them");
	is($concurrent->safe_psql('postgres', rows($table)),
		$node->safe_psql('postgres', rows($table)), "and reads $table with the primary's tids");
}
$concurrent->stop;

# Bugs row 122: after a rewrite, a fork table's string of tids, xmins, and keys
# reads its rows, as heap's does.
$node->safe_psql('postgres', 'CREATE TABLE twin (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres', written('twin', "INSERT INTO % SELECT g, 't' || g FROM generate_series(1, 2000) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('twin')");
$node->safe_psql('postgres', rewritten('twin', '%_pkey', 'false'));
my $listed = sub { rows($_[0], "SELECT count(ctid::text || ' ' || xmin::text || ' ' || k) || ' ' || count(*) FROM %") };
is($node->safe_psql('postgres', $listed->('twin')), '2000 2000',
	"a rewritten table's string of tids, xmins, and keys reads its rows");

# Bugs row 119's built nodes that no base record installs leave the working
# state's delta: a running writer's stay through a running transactions record;
# after the primary is killed with its rewrite between its Chunk records and its
# base record, its crash recovery drops them at the end of redo, so a full
# collection completes, and a streaming standby drops them at the next running
# transactions record; and a standby that replays an aborted rewrite drops them
# at the abort.
my $pruning = PostgreSQL::Test::Cluster->new('pruning_standby');
$pruning->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$pruning->start;
# given_up has no TOAST relation, so its rewrite's first built base, the one
# that fails, is its own and logs its nodes.
for my $table ('cut_short', 'given_up')
{
	my $value = $table eq 'cut_short' ? 'text' : 'int4';
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 PRIMARY KEY, v $value)");
	$node->safe_psql('postgres', written($table, "INSERT INTO % SELECT g, g FROM generate_series(1, 200) g"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
}
$node->wait_for_catchup($pruning);
my $built_nodes = 'SELECT doltgres_test_built_nodes()';
my $cutter = $node->background_psql('postgres', on_error_stop => 0);
$cutter->query_safe('SELECT injection_points_set_local()');
$cutter->query_safe("SELECT injection_points_attach('doltgres-built-logged', 'wait')");
$cutter->query_until(qr/start/, "\\echo start\n" . rewritten('cut_short', '%_pkey', 'false') . ";\n");
$node->wait_for_event('client backend', 'doltgres-built-logged');
# A running transactions record isn't flushed at once, so each wait is for the
# standby's replay of the record itself.
$node->wait_for_catchup($pruning, 'replay', $node->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()'));
isnt($pruning->safe_psql('postgres', $built_nodes), '0',
	"a running rewrite's built nodes stay through a running transactions record that lists their writer");
$node->stop('immediate');
eval { $cutter->quit };
$node->start;
is($node->safe_psql('postgres', $built_nodes), '0',
	"a crash between a built base's chunk records and its base record leaves no built node in the delta");
$node->restart;
is($node->safe_psql('postgres', $built_nodes), '0',
	"nor in the runs the end-of-recovery checkpoint wrote, which the next start loads");
like($node->safe_psql('postgres', 'SELECT doltgres_test_collect()'), qr/^\d+ \d+ \d+$/,
	"and a full collection after it completes");
$node->wait_for_catchup($pruning, 'replay', $node->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()'));
is($pruning->safe_psql('postgres', $built_nodes), '0',
	"after the next running transactions record the standby holds none of the crashed rewrite's built nodes");
# The rewrite fails in a transaction whose xid the standby's recovery targets,
# so its replay pauses just before the abort's record and then just after it,
# with no running transactions record replayed after the abort: only the
# abort's replay, which drops the rewrite's new storage, takes its nodes.
my $giver = $node->background_psql('postgres', on_error_stop => 0);
$giver->query_safe('SELECT injection_points_set_local()');
$giver->query_safe("SELECT injection_points_attach('doltgres-built-logged', 'error')");
my $given = $giver->query_safe('BEGIN; SELECT txid_current()');
$pruning->append_conf('postgresql.conf',
	"recovery_target_xid = '$given'\nrecovery_target_inclusive = false\nrecovery_target_action = 'pause'");
$pruning->restart;
$giver->query(rewritten('given_up', '%_pkey', 'false'));
like($giver->{stderr}, qr/error triggered for injection point doltgres-built-logged/,
	"a rewrite fails after its chunk records");
$giver->query('ROLLBACK');
$giver->quit;
my $paused = "SELECT pg_get_wal_replay_pause_state() = 'paused'";
$pruning->poll_query_until('postgres', $paused) or die "the standby's replay never paused before the abort";
isnt($pruning->safe_psql('postgres', $built_nodes), '0', "the failed rewrite's nodes reach the standby before its abort");
$pruning->adjust_conf('postgresql.conf', 'recovery_target_inclusive', 'true');
$pruning->restart;
$pruning->poll_query_until('postgres', $paused) or die "the standby's replay never paused after the abort";
is($pruning->safe_psql('postgres', $built_nodes), '0',
	"a standby that replays an aborted rewrite holds none of its built nodes");
$pruning->adjust_conf('postgresql.conf', $_, undef)
  for ('recovery_target_xid', 'recovery_target_inclusive', 'recovery_target_action');
$pruning->restart;
$node->wait_for_catchup($pruning);
is($pruning->safe_psql('postgres', rows('given_up', "SELECT count(*) FROM %")), '200',
	"and reads the table's rows from before the rewrite");

# PRevPF 8's pieces: a keyless table's 6000 equal rows, each its own
# transaction's, are one entry whose stamps alone pass a block of the delta's
# region, so its rewrite's leaf is held in pieces on the standby, which rebuilds
# it whole: it reads every row with the primary's tids and xmins, and holds none
# of its nodes after.
$node->safe_psql('postgres', 'CREATE TABLE narrow (v int4)');
$node->safe_psql('postgres',
	"DO \$\$ BEGIN FOR i IN 1..6000 LOOP PERFORM doltgres_test_write('narrow', true, 'INSERT INTO narrow VALUES (1)'); COMMIT; END LOOP; END \$\$"
);
$node->safe_psql('postgres', "SELECT doltgres_test_fold('narrow')");
$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pg_walinspect');
my $before_narrow = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->safe_psql('postgres', rewritten('narrow', undef, 'false'));
cmp_ok(
	$node->safe_psql('postgres',
		"SELECT max(record_length) FROM pg_get_wal_records_info('$before_narrow', pg_current_wal_flush_lsn()) WHERE record_type = 'Chunk'"
	),
	'>', 64 << 10,
	"the rewrite logs a built node larger than a block of the delta's region");
$node->wait_for_catchup($pruning, 'replay', $node->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()'));
for my $read (rows('narrow'), $stamped->('narrow'))
{
	is($pruning->safe_psql('postgres', $read), $node->safe_psql('postgres', $read),
		"a standby rebuilds a rewritten leaf held in pieces and reads it as the primary does");
}
is($pruning->safe_psql('postgres', $built_nodes), '0', "and holds none of its nodes after");
$pruning->stop;

# SwitchPF 1's carried end: a row still in the delta, which a held snapshot keeps
# through VACUUM FULL, is carried at its tid, and once its delete is folded and
# VACUUM runs, its tid is free, so the next insert takes it as heap's does.
twins(
	'carried_end',
	'(k int4 PRIMARY KEY, v text)',
	"a carried row's tid goes back to the free space map once its end is folded, and the next insert takes it as heap's",
	"INSERT INTO % VALUES (1, 'a'), (2, 'b')",
	'hold',
	'DELETE FROM % WHERE k = 1',
	'rewrite',
	'rows',
	'release',
	"fork: SELECT doltgres_test_fold('%')",
	"fork: SELECT doltgres_test_vacuum('%', 'auto')",
	'heap: VACUUM %',
	"INSERT INTO % VALUES (3, 'c')",
	'rows');

# The carried place's take replays: a standby's redo of a carried row takes its
# place as the primary's rewrite did, so both free space maps read alike, and a
# crash between the rewrite and the fold leaves the place taken by the restart's
# redo, so after the fold VACUUM frees it and the next insert takes it, as
# heap's VACUUM frees its dead row's.
my $carrying = PostgreSQL::Test::Cluster->new('carrying_standby');
$carrying->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$carrying->start;
my @carry = ('crashed_carry', 'heap_crashed_carry');
for my $table (@carry)
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 PRIMARY KEY, v text)");
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
}
my $carry_holder = $node->background_psql('postgres');
$carry_holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
for my $table (@carry)
{
	$node->safe_psql('postgres', written($table, 'DELETE FROM % WHERE k = 1'));
	$node->safe_psql('postgres', rewritten($table, undef, 'false'));
}
my $carry_free = "SELECT doltgres_test_free_space('crashed_carry', 0)";
my $carried_free = $node->safe_psql('postgres', $carry_free);
$node->wait_for_catchup($carrying);
is($carrying->safe_psql('postgres', $carry_free), $carried_free,
	"a standby's redo of a carried row takes its place as the primary's rewrite did");
$node->stop('immediate');
eval { $carry_holder->quit };
$node->start;
is($node->safe_psql('postgres', $carry_free), $carried_free,
	"a crash between a rewrite and its fold leaves the carried row's place taken by the restart's redo");
$node->safe_psql('postgres',
	"SELECT doltgres_test_fold('crashed_carry'); SELECT doltgres_test_vacuum('crashed_carry', 'auto'); VACUUM heap_crashed_carry"
);
$node->safe_psql('postgres', written($_, "INSERT INTO % VALUES (3, 'c')")) for @carry;
is($node->safe_psql('postgres', rows('crashed_carry')), $node->safe_psql('postgres', rows('heap_crashed_carry')) =~ s/heap_//gr,
	"after the crash, the fold, and VACUUM, the next insert takes the carried row's tid as heap's");
$node->wait_for_catchup($carrying);
is($carrying->safe_psql('postgres', $carry_free), $node->safe_psql('postgres', $carry_free),
	"and the standby's free space map reads as the primary's");
$carrying->stop;

# Section 5's SET TABLESPACE: a table with base rows and delta rows, an older
# REPEATABLE READ snapshot holding an update chain, a delete, an aborted insert,
# and a TOAST value keeps every row version with its ctid, xmin, xmax, cmin, and
# cmax, as heap's page copy does, for that snapshot and for a new one; a standby
# and a crash after it read the same.
my $versions =
  "SELECT string_agg(ctid::text || ' ' || xmin || ' ' || xmax || ' ' || cmin || ' ' || cmax || ' ' || k || ' ' || md5(v), ', ' ORDER BY ctid) FROM %";
$node->safe_psql('postgres',
	"CREATE EXTENSION IF NOT EXISTS pg_walinspect; SET allow_in_place_tablespaces = on; CREATE TABLESPACE moved_to LOCATION ''; CREATE TABLESPACE moved_on LOCATION ''"
);
$node->safe_psql('postgres', 'CREATE TABLE moving (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres',
	written('moving', "INSERT INTO % SELECT g, repeat('m', g * 20) FROM generate_series(1, 40) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('moving')");
my $snapshot = $node->background_psql('postgres');
my $exported = $snapshot->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_export_snapshot()');
my $exported_read = "BEGIN ISOLATION LEVEL REPEATABLE READ; SET TRANSACTION SNAPSHOT '$exported'; " . rows('moving', $versions) . '; COMMIT';
$node->safe_psql('postgres', written('moving', "UPDATE % SET v = v || 'u' WHERE k <= 5"));
$node->safe_psql('postgres', written('moving', "UPDATE % SET v = v || 'w' WHERE k <= 3"));
$node->safe_psql('postgres', written('moving', 'DELETE FROM % WHERE k BETWEEN 6 AND 8'));
$node->safe_psql('postgres', 'BEGIN; ' . written('moving', "INSERT INTO % VALUES (41, 'aborted')") . '; ROLLBACK');
$node->safe_psql('postgres', written('moving', "INSERT INTO % VALUES (50, '$noise')"));
my @unmoved = ($node->safe_psql('postgres', rows('moving', $versions)), $node->safe_psql('postgres', $exported_read));
$node->safe_psql('postgres', "SELECT doltgres_test_move('moving', 'moved_to')");
my @moved = ($node->safe_psql('postgres', rows('moving', $versions)), $node->safe_psql('postgres', $exported_read));
is_deeply(\@moved, \@unmoved,
	"SET TABLESPACE keeps every row version with its ctid, xmin, xmax, cmin, and cmax, for an older snapshot too");
$snapshot->query_safe('COMMIT');
$snapshot->quit;
my $moving = PostgreSQL::Test::Cluster->new('move_standby');
$moving->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$moving->start;
$node->wait_for_catchup($moving);
is($moving->safe_psql('postgres', rows('moving', $versions)), $moved[0],
	"a standby reads a moved table's row versions and TOAST value as the primary does");
$moving->stop;
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', rows('moving', $versions)), $moved[0],
	"a crash replays a move to the same row versions and TOAST value");

# Section 5's free space map versions (CommPF 8's case): rows whose takes are in
# the delta, deleted in one transaction over a moved table and its unmoved twin,
# are freed by VACUUM after the move with the twin's horizon and bytes, and the
# next inserts take the twin's tids.
for my $table ('spaced', 'unspaced')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 PRIMARY KEY, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % SELECT g, repeat('f', 200) FROM generate_series(1, 20) g"));
}
$node->safe_psql('postgres',
	'BEGIN; ' . join('; ', map { written($_, 'DELETE FROM % WHERE k <= 5') } ('spaced', 'unspaced')) . '; COMMIT');
$node->safe_psql('postgres', "SELECT doltgres_test_fold('$_')") for ('spaced', 'unspaced');
$node->safe_psql('postgres', "SELECT doltgres_test_move('spaced', 'moved_on')");
my %freed;
for my $table ('spaced', 'unspaced')
{
	my $start = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$node->safe_psql('postgres', "SELECT doltgres_test_vacuum('$table', 'on')");
	$freed{$table} = [
		$node->safe_psql('postgres',
			"DO \$\$BEGIN PERFORM pg_logical_emit_message(false, 'freed', '', true); END\$\$; SELECT string_agg(regexp_replace(description, '^rel [0-9/]+; ', ''), ' | ' ORDER BY start_lsn) FROM pg_get_wal_records_info('$start', pg_current_wal_flush_lsn()) WHERE record_type = 'TidsFreed'"
		),
		$node->safe_psql('postgres', "SELECT doltgres_test_free_space('$table', 0)"),
		$node->safe_psql('postgres',
			written($table, "INSERT INTO % SELECT g, repeat('n', 200) FROM generate_series(21, 25) g") . '; '
			  . rows($table, "SELECT string_agg(ctid::text, ' ' ORDER BY ctid) FROM %"))
	];
}
isnt($freed{unspaced}[0], '', "the unmoved twin's VACUUM freed tuple ids");
is_deeply($freed{spaced}, $freed{unspaced},
	"VACUUM after a move frees with the unmoved twin's horizon and bytes, and the next inserts take its tids");

# Section 5's unlogged table (PRevPF 8's S3): a moved unlogged table reads its
# rows, and after a crash reads empty, as heap's copy of its init fork leaves it.
$node->safe_psql('postgres', 'CREATE UNLOGGED TABLE unlogged_moving (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres',
	written('unlogged_moving', "INSERT INTO % SELECT g, repeat('l', g) FROM generate_series(1, 20) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('unlogged_moving')");
$node->safe_psql('postgres', written('unlogged_moving', 'DELETE FROM % WHERE k > 15'));
$node->safe_psql('postgres', "SELECT doltgres_test_move('unlogged_moving', 'moved_on')");
is($node->safe_psql('postgres', rows('unlogged_moving', 'SELECT count(*) FROM %')),
	'15', 'a moved unlogged table reads its rows');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', rows('unlogged_moving', 'SELECT count(*) FROM %')),
	'0', 'a moved unlogged table reads empty after a crash');

# Bugs row 95: every fold writes its packs into its storage's own tablespace, for
# a table made in a tablespace and for one moved there, and none into the
# database's own directory; the tablespace refuses to drop while they hold data.
my $database = $node->safe_psql('postgres', "SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $placed_on = $node->safe_psql('postgres', "SELECT oid FROM pg_tablespace WHERE spcname = 'moved_on'");
my ($placed_dir) = glob($node->data_dir . "/pg_tblspc/$placed_on/PG_*");
my %packs = (home => $node->data_dir . "/pg_versioned/$database", there => "$placed_dir/pg_versioned/$database");
$node->safe_psql('postgres', 'CREATE TABLE placed (k int4 PRIMARY KEY, v text) TABLESPACE moved_on');
$node->safe_psql('postgres', 'CREATE TABLE relocated (k int4 PRIMARY KEY, v text)');
for my $table ('placed', 'relocated')
{
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % SELECT g, repeat('p', g) FROM generate_series(1, 30) g"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
}
$node->safe_psql('postgres', "SELECT doltgres_test_move('relocated', 'moved_on')");
my %before = map { $_ => join(' ', sort glob("$packs{$_}/*.pack")) } keys %packs;
for my $table ('placed', 'relocated')
{
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % SELECT g, repeat('q', g) FROM generate_series(31, 60) g"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
}
my %after = map { $_ => join(' ', sort glob("$packs{$_}/*.pack")) } keys %packs;
my %home_before = map { $_ => 1 } split / /, $before{home};
is(join(' ', grep { !$home_before{$_} } split / /, $after{home}),
	'', "folds of tables in a tablespace write no pack into the database's own directory");
isnt($after{there}, $before{there}, "folds of tables in a tablespace write their packs there");
is($node->safe_psql('postgres', rows('relocated', "SELECT count(*) || ' ' || sum(length(v)) FROM %")),
	'60 1830', 'a table moved into a tablespace and folded there reads every row');
my (undef, undef, $undropped) = $node->psql('postgres', "\\set VERBOSITY verbose\nDROP TABLESPACE moved_on");
like($undropped, qr/ERROR:  55000: tablespace "moved_on" is not empty/,
	'a tablespace holding tables the fork folded there refuses to drop');

# Base field 13 (bugs row 93) under a move: rows folded into the base, then
# deleted and folded again, leave their tuple ids pending in the base's pending
# tree, which the move copies with the other trees, so VACUUM after the move frees
# them with the unmoved twin's horizon and bytes; a server restored from a backup
# taken before the move rebuilds the copied tree from the move's records and,
# once promoted, frees what the primary freed.
sub vacuumed
{
	my ($on, $table) = @_;
	my $start = $on->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$on->safe_psql('postgres', "SELECT doltgres_test_vacuum('$table', 'on')");
	return [
		$on->safe_psql('postgres',
			"DO \$\$BEGIN PERFORM pg_logical_emit_message(false, 'freed', '', true); END\$\$; SELECT string_agg(regexp_replace(description, '^rel [0-9/]+; ', ''), ' | ' ORDER BY start_lsn) FROM pg_get_wal_records_info('$start', pg_current_wal_flush_lsn()) WHERE record_type = 'TidsFreed'"
		),
		$on->safe_psql('postgres', "SELECT doltgres_test_free_space('$table', 0)")
	];
}
for my $table ('pended', 'unpended')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 PRIMARY KEY, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % SELECT g, repeat('p', 200) FROM generate_series(1, 20) g"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
}
$node->safe_psql('postgres',
	'BEGIN; ' . join('; ', map { written($_, 'DELETE FROM % WHERE k <= 5') } ('pended', 'unpended')) . '; COMMIT');
$node->safe_psql('postgres', "SELECT doltgres_test_fold('$_')") for ('pended', 'unpended');
$node->safe_psql('postgres', "SELECT doltgres_test_move('pended', 'moved_to')");
my $pended = PostgreSQL::Test::Cluster->new('pended_restored');
$pended->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$pended->start;
$node->wait_for_catchup($pended);
$pended->promote;
my $unpended = vacuumed($node, 'unpended');
isnt($unpended->[0], '', "the unmoved twin's VACUUM frees its pending tuple ids");
my $moved_pended = vacuumed($node, 'pended');
is_deeply($moved_pended, $unpended,
	"VACUUM after a move frees the moved table's pending tuple ids as its unmoved twin's");
is_deeply(vacuumed($pended, 'pended'), $moved_pended,
	"a server that rebuilt a moved base from its records frees its pending tuple ids as the primary does");
$pended->stop;

# Section 5's TOAST value folded into its TOAST storage's base (the maps' fold):
# values out of line, the table and its TOAST storage folded until the value
# map's delta is drained, then moved with the table, read the same, and a server
# restored from a backup taken before them reads the same once promoted.
$node->safe_psql('postgres', 'CREATE TABLE toasted_move (k int4 PRIMARY KEY, v text STORAGE EXTERNAL)');
$node->safe_psql('postgres',
	written('toasted_move', "INSERT INTO % SELECT g, string_agg(md5((g * 1000 + h)::text), '') FROM generate_series(1, 3) g, generate_series(1, 2000) h GROUP BY g"));
my $toast_of = $node->safe_psql('postgres',
	"SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'toasted_move'");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('toasted_move')");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('$toast_of')") for 1 .. 3;
my $toast_read = rows('toasted_move', "SELECT string_agg(k || ' ' || length(v) || ' ' || md5(v), ', ' ORDER BY k) FROM %");
my $folded_toast = $node->safe_psql('postgres', $toast_read);
$node->safe_psql('postgres', "SELECT doltgres_test_move('toasted_move', 'moved_on')");
is($node->safe_psql('postgres', $toast_read), $folded_toast,
	"a value folded into its TOAST storage's base reads the same after the move");
my $toast_restored = PostgreSQL::Test::Cluster->new('toast_restored');
$toast_restored->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$toast_restored->start;
$node->wait_for_catchup($toast_restored);
$toast_restored->promote;
is($toast_restored->safe_psql('postgres', $toast_read), $folded_toast,
	"a server that rebuilt a moved TOAST storage's base from its records reads its folded values");
$toast_restored->stop;

# Section 5's index (PRevPF 8's X1): ALTER INDEX SET TABLESPACE moves a fork
# index's storage beside Postgres' copy of its file, through index_copy_data's
# call site, so a btree of a keyed table and one of a keyless table, whose
# entries count their rows' copies, keep every entry version, on the primary,
# after a crash, and on a server restored from a backup taken before them once
# promoted.
my %entry_moves = (indexed_move => 'indexed_move_v', keyless_move => 'keyless_move_k');
$node->safe_psql('postgres',
	'CREATE TABLE indexed_move (k int4 PRIMARY KEY, v text) USING heap; CREATE INDEX indexed_move_v ON indexed_move (v)');
$node->safe_psql('postgres',
	'CREATE TABLE keyless_move (k int4, v text) USING heap; CREATE INDEX keyless_move_k ON keyless_move (k)');
for my $table (sort keys %entry_moves)
{
	my $given = "SELECT doltgres_test_index_routine('$entry_moves{$table}'); ";
	$node->safe_psql('postgres',
		$given . written($table, "INSERT INTO % SELECT g, 'v' || mod(g, 5) FROM generate_series(1, 30) g"));
	$node->safe_psql('postgres', $given . written($table, "INSERT INTO % VALUES (31, 'v1'), (31, 'v1')"))
	  if $table eq 'keyless_move';
	$node->safe_psql('postgres', $given . written($table, "UPDATE % SET v = v || 'u' WHERE k <= 3"));
	$node->safe_psql('postgres', $given . written($table, 'DELETE FROM % WHERE k BETWEEN 6 AND 8'));
	$node->safe_psql('postgres',
		"BEGIN; $given" . written($table, "INSERT INTO % VALUES (40, 'aborted')") . '; ROLLBACK');
}
my $index_entries = sub {
	my ($on) = @_;
	return join(' | ',
		map { $on->safe_psql('postgres', "SELECT doltgres_test_index_entries('$_')") } sort values %entry_moves);
};
my $unmoved_entries = $index_entries->($node);
unlike($unmoved_entries, qr/(^| )none( |$)/, 'both indexes hold entry versions before the move');
$node->safe_psql('postgres',
	"SELECT doltgres_test_index_routine('$_'); ALTER INDEX $_ SET TABLESPACE moved_to")
  for sort values %entry_moves;
my $moved_to = $node->safe_psql('postgres', "SELECT oid FROM pg_tablespace WHERE spcname = 'moved_to'");
is( $node->safe_psql('postgres',
		"SELECT string_agg(reltablespace::text, ' ') FROM pg_class WHERE relname IN ('indexed_move_v', 'keyless_move_k')"),
	"$moved_to $moved_to",
	'both indexes are in the new tablespace');
is($index_entries->($node), $unmoved_entries,
	"ALTER INDEX SET TABLESPACE keeps a fork index's entries, for an index of a keyless table too");
$node->stop('immediate');
$node->start;
is($index_entries->($node), $unmoved_entries, "a crash replays an index's move to the same entries");
my $entries_restored = PostgreSQL::Test::Cluster->new('entries_restored');
$entries_restored->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$entries_restored->start;
$node->wait_for_catchup($entries_restored);
$entries_restored->promote;
is($index_entries->($entries_restored), $unmoved_entries,
	"a server that replayed an index's move from its records reads its entries as the primary does");
$entries_restored->stop;

# Section 5's index move with an older snapshot (PRevPF 8, CommPF 8's ruling
# (a)): an index folded, a REPEATABLE READ snapshot taken, rows added and the
# index folded again, so the snapshot may read only the predecessor; ALTER INDEX
# SET TABLESPACE copies both bases as they are and carries the entries the older
# doesn't apply.
$node->safe_psql('postgres',
	'CREATE TABLE folded_move (k int4 PRIMARY KEY, v text) USING heap; CREATE INDEX folded_move_v ON folded_move (v)');
my $index_given = "SELECT doltgres_test_index_routine('folded_move_v'); ";
$node->safe_psql('postgres',
	$index_given . written('folded_move', "INSERT INTO % SELECT g, 'f' || g FROM generate_series(1, 5) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_index_fold('folded_move_v')");
my $first_base = $node->safe_psql('postgres', "SELECT doltgres_test_index_base('folded_move_v')");
my $index_holder = $node->background_psql('postgres');
$index_holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres',
	$index_given . written('folded_move', "INSERT INTO % SELECT g, 'f' || g FROM generate_series(6, 8) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_index_fold('folded_move_v')");
my $second_base = $node->safe_psql('postgres', "SELECT doltgres_test_index_base('folded_move_v')");
is($second_base, $first_base, "a fold under an older snapshot applies nothing it counts as running");
my $index_delta = $node->safe_psql('postgres', "SELECT doltgres_test_index_entries('folded_move_v')");
$node->safe_psql('postgres', $index_given . 'ALTER INDEX folded_move_v SET TABLESPACE moved_to');
is($node->safe_psql('postgres', "SELECT doltgres_test_index_base('folded_move_v')"),
	$second_base, "an index's move under an older snapshot keeps its newest base");
is($node->safe_psql('postgres', "SELECT doltgres_test_index_entries('folded_move_v')"),
	$index_delta, "and carries every entry its predecessor doesn't apply");
$index_holder->query_safe('COMMIT');
$index_holder->quit;

# The fold text's draft 12: a move relocates a storage's newest base, which a
# primary snapshot holds back to its horizon, with the delta's versions past it,
# so an older snapshot reads the moved table as heap's twin on the primary and,
# with that hold, on a hot standby; with no primary snapshot holding the fold, the
# move's record carries its base's maxFolded as its conflict horizon (field 11),
# whose replay cancels an older standby snapshot (Differences row 54). The tables'
# bases have inner nodes and a TOAST value, and a row updated to its own value
# keeps its stamps.
my $relocating = PostgreSQL::Test::Cluster->new('relocating_standby');
$relocating->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$relocating->append_conf('postgresql.conf', 'max_standby_streaming_delay = 0');
$relocating->start;
my $relocated_read = sub {
	rows($_[0], "SELECT count(*) || ' ' || sum(k) || ' ' || md5(string_agg(k || ':' || v, ',' ORDER BY k)) FROM %");
};
for my $table ('relocated_held', 'relocated_standby')
{
	my $holds = $table eq 'relocated_held';
	for my $each ($table, "heap_$table")
	{
		$node->safe_psql('postgres', "CREATE TABLE $each (k int4 PRIMARY KEY, v text)");
		$node->safe_psql('postgres',
			written($each,
				"INSERT INTO % SELECT g, CASE WHEN g = 2500 THEN (SELECT string_agg(md5(i::text), '') FROM generate_series(1, 400) i) ELSE 'r' || g END FROM generate_series(1, 5000) g"
			));
	}
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	$node->wait_for_catchup($relocating);
	my $standby_older = $relocating->background_psql('postgres', on_error_stop => 0);
	$standby_older->query_safe("\\set VERBOSITY verbose\nBEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1");
	my $primary_older = $holds ? $node->background_psql('postgres') : undef;
	$primary_older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1') if $holds;
	for my $each ($table, "heap_$table")
	{
		$node->safe_psql('postgres',
			written($each, "INSERT INTO % SELECT g, 'r' || g FROM generate_series(5001, 5003) g"));
		$node->safe_psql('postgres', written($each, "UPDATE % SET v = v WHERE k = 10"));
	}
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	my $stamp_read = rows($table, "SELECT xmin FROM % WHERE k = 10");
	my $stamp = $node->safe_psql('postgres', $stamp_read);
	my $move_offset = -s $relocating->logfile;
	$node->safe_psql('postgres', "SELECT doltgres_test_move('$table', 'moved_to')");
	$node->wait_for_catchup($relocating);
	if ($holds)
	{
		is($primary_older->query_safe($relocated_read->($table)),
			$primary_older->query_safe($relocated_read->("heap_$table")),
			"an older primary snapshot reads a moved table as heap's twin, by the versions the move carried");
		$primary_older->query_safe('COMMIT');
		$primary_older->quit;
		my $standby_twin = $standby_older->query_safe($relocated_read->("heap_$table"));
		my $standby_read = $standby_older->query($relocated_read->($table));
		unlike($standby_older->{stderr}, qr/ERROR/,
			"an older standby snapshot reads $table with no recovery conflict");
		is($standby_read, $standby_twin, "an older standby snapshot reads $table as heap's twin");
	}
	else
	{
		ok($relocating->wait_for_log(qr/terminating connection due to conflict with recovery/, $move_offset),
			"the replay of ${table}'s move cancels an older standby snapshot that may not read its newest base");
	}
	is($node->safe_psql('postgres', $relocated_read->($table)),
		$node->safe_psql('postgres', $relocated_read->("heap_$table")),
		"a new snapshot reads every row of $table");
	is($relocating->safe_psql('postgres', $relocated_read->($table)),
		$relocating->safe_psql('postgres', $relocated_read->("heap_$table")),
		"a new standby snapshot reads $table as heap's twin");
	is($node->safe_psql('postgres', $stamp_read),
		$stamp, "the move keeps the stamps of a row of $table updated to its own value");
	eval { $standby_older->query('COMMIT'); $standby_older->quit };
}
$relocating->stop;

# Section 6's key change: ADD PRIMARY KEY on a keyless table with rows in its
# base and its delta writes the table once under the new key, in a new
# relfilenode for it and its TOAST relation (Differences row 31), every row
# keeping its ctid, xmin, and values, read in the new key's order; dropping the
# key writes it keyless again; a NULL key column raises the reference's 23502;
# a key change rolled back to a savepoint writes nothing; a standby and a crash
# read the key change's rows as the primary does.
my $keyed_rows =
  "SELECT string_agg(ctid::text || ' ' || xmin || ' ' || k || ' ' || md5(v), ', ' ORDER BY ctid) FROM %";
my $filenodes = sub {
	my ($table) = @_;
	return $node->safe_psql('postgres',
		"SELECT c.relfilenode || ' ' || t.relfilenode FROM pg_class c JOIN pg_class t ON t.oid = c.reltoastrelid WHERE c.relname = '$table'"
	);
};
for my $table ('rekeyed', 'heap_rekeyed')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % SELECT g, repeat('r', g * 30) FROM generate_series(40, 1, -1) g"));
}
$node->safe_psql('postgres', "SELECT doltgres_test_fold('rekeyed')");
$node->safe_psql('postgres', written($_, "UPDATE % SET v = v || 'u' WHERE mod(k, 3) = 0")) for ('rekeyed', 'heap_rekeyed');
$node->safe_psql('postgres', written($_, 'DELETE FROM % WHERE k > 35')) for ('rekeyed', 'heap_rekeyed');
my $unkeyed = $node->safe_psql('postgres', rows('rekeyed', $keyed_rows));
my $twin_rows = "SELECT string_agg(k || ' ' || md5(v), ', ' ORDER BY k) FROM %";
is($node->safe_psql('postgres', rows('rekeyed', $twin_rows)),
	$node->safe_psql('postgres', rows('heap_rekeyed', $twin_rows)),
	"a keyless table's rows read as heap's twin's");
my $files_before = $filenodes->('rekeyed');
$node->safe_psql('postgres', 'BEGIN; SAVEPOINT s; ALTER TABLE rekeyed ADD PRIMARY KEY (k); ROLLBACK TO s; COMMIT');
is($filenodes->('rekeyed'), $files_before, 'a key change rolled back to a savepoint writes nothing');
$node->safe_psql('postgres', 'ALTER TABLE rekeyed ADD PRIMARY KEY (k)');
my $files_after = $filenodes->('rekeyed');
my ($table_before, $toast_before) = split / /, $files_before;
my ($table_after, $toast_after) = split / /, $files_after;
ok($table_after != $table_before && $toast_after != $toast_before,
	'ADD PRIMARY KEY gives the table and its TOAST relation new relfilenodes');
is($node->safe_psql('postgres', rows('rekeyed', $keyed_rows)), $unkeyed,
	'ADD PRIMARY KEY writes a table with rows once under its new key');
is( $node->safe_psql('postgres', rows('rekeyed', "SELECT string_agg(k::text, ' ') FROM %")),
	$node->safe_psql('postgres', "SELECT string_agg(k::text, ' ' ORDER BY k) FROM heap_rekeyed"),
	"the rekeyed table's scan gives its rows in the new key's order");
$node->safe_psql('postgres', 'ALTER TABLE rekeyed DROP CONSTRAINT rekeyed_pkey');
isnt($filenodes->('rekeyed'), $files_after, 'dropping the primary key writes the table again');
is($node->safe_psql('postgres', rows('rekeyed', $keyed_rows)), $unkeyed, 'a table made keyless again reads the same rows');
$node->safe_psql('postgres', 'CREATE TABLE null_keyed (k int4, v text) USING heap');
$node->safe_psql('postgres', written('null_keyed', "INSERT INTO % VALUES (1, 'a'), (NULL, 'b')"));
my (undef, undef, $null_error) =
  $node->psql('postgres', "\\set VERBOSITY verbose\nALTER TABLE null_keyed ADD PRIMARY KEY (k)");
like($null_error, qr/ERROR:  23502: column "k" of relation "null_keyed" contains null values/,
	'ADD PRIMARY KEY over a NULL raises 23502 as the reference does');
$node->safe_psql('postgres', 'ALTER TABLE rekeyed ADD PRIMARY KEY (k)');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', rows('rekeyed', $keyed_rows)), $unkeyed, "a crash replays a key change's rows");
my $rekeyed_standby = PostgreSQL::Test::Cluster->new('rekeyed_standby');
$rekeyed_standby->init_from_backup($node, 'rewrite_backup', has_streaming => 1);
$rekeyed_standby->start;
$node->wait_for_catchup($rekeyed_standby);
is($rekeyed_standby->safe_psql('postgres', rows('rekeyed', $keyed_rows)), $unkeyed,
	"a standby replays a key change's rows as the primary reads them");
$rekeyed_standby->stop;

# Section 6's overlapping key (its §8 to §10, bugs row 114, Differences row 31),
# logged and unlogged: rows (1, 'a') and (2, 'a') folded into the base, a
# REPEATABLE READ snapshot taken, row 1 deleted, then ADD PRIMARY KEY (v), which
# succeeds as the reference's build skips the recently dead row; the snapshot
# still reads both rows, by a scan and by the key, the base rows moved into the
# delta showing xmin 2; once it ends a fold installs the survivor, which a new
# snapshot reads alone. The fold's refusal of two live rows (bugs row 114) is
# fold.rs's unit, since a fold point falls below only commits still in flight.
for my $table ('overlapped', 'unlogged_overlapped')
{
	my $unlogged = $table =~ /^unlogged/ ? 'UNLOGGED ' : '';
	$node->safe_psql('postgres', "CREATE ${unlogged}TABLE $table (k int4, v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES (1, 'a'), (2, 'a'), (3, 'b')"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	my $holder = $node->background_psql('postgres');
	my $held = $holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_export_snapshot()');
	my $read_held = "BEGIN ISOLATION LEVEL REPEATABLE READ; SET TRANSACTION SNAPSHOT '$held'; "
	  . rows($table, "SELECT string_agg(ctid::text || ' ' || k || ' ' || v, ', ' ORDER BY ctid) FROM %") . '; COMMIT';
	my $xmins_held = "BEGIN ISOLATION LEVEL REPEATABLE READ; SET TRANSACTION SNAPSHOT '$held'; "
	  . rows($table, "SELECT string_agg(xmin::text, ' ' ORDER BY k) FROM % WHERE k <= 2") . '; COMMIT';
	my $found_held = "BEGIN ISOLATION LEVEL REPEATABLE READ; SET TRANSACTION SNAPSHOT '$held'; "
	  . rows($table, "SELECT count(*) FROM % WHERE v || '' = 'a'") . '; COMMIT';
	my $before_key = $node->safe_psql('postgres', $read_held);
	$node->safe_psql('postgres', written($table, 'DELETE FROM % WHERE k = 1'));
	$node->safe_psql('postgres', "ALTER TABLE $table ADD PRIMARY KEY (v)");
	is($node->safe_psql('postgres', $read_held), $before_key,
		"$table: an older snapshot reads both rows of an overlapping new key after the key change");
	is($node->safe_psql('postgres', $xmins_held), '2 2',
		"$table: the base rows an overlapping key moved show xmin 2 (Differences row 31)");
	is($node->safe_psql('postgres', $found_held), '2', "$table: the older snapshot finds both rows by the new key");
	is($node->safe_psql('postgres', rows($table, "SELECT count(*) FROM % WHERE v || '' = 'a'")),
		'1', "$table: a new snapshot finds the survivor by the new key");
	$holder->query_safe('COMMIT');
	$holder->quit;
	is($node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')"), 't',
		"$table: once the older snapshot ends a fold installs the key's survivor");
	is($node->safe_psql('postgres', rows($table, "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k) FROM %")),
		'2 a, 3 b', "$table: a new snapshot reads the survivor alone");
}

# Section 6's settle at a utility statement's end (K1 to K3): DROP DOMAIN ...
# CASCADE taking a key column leaves a keyless table written again, which reads
# its rows and takes a duplicate as the reference does; a DO block whose
# EXCEPTION clause catches a failed ADD PRIMARY KEY, then adds it, writes the
# table once, the failed attempt's level unwound with its subtransaction.
my $filenode = sub {
	my ($table) = @_;
	return $node->safe_psql('postgres', "SELECT relfilenode FROM pg_class WHERE relname = '$table'");
};
$node->safe_psql('postgres', 'CREATE DOMAIN key_domain AS int4');
$node->safe_psql('postgres', 'CREATE TABLE domained (k key_domain PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('domained', "INSERT INTO % SELECT g, 'd' || g FROM generate_series(1, 5) g"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('domained')");
my $domained = $filenode->('domained');
my $domain_holder = $node->background_psql('postgres');
my $domain_held = $domain_holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_export_snapshot()');
$node->safe_psql('postgres', 'DROP DOMAIN key_domain CASCADE');
is( $node->safe_psql('postgres',
		"BEGIN ISOLATION LEVEL REPEATABLE READ; SET TRANSACTION SNAPSHOT '$domain_held'; "
		  . rows('domained', "SELECT string_agg(v, ' ' ORDER BY v) FROM %") . '; COMMIT'),
	'd1 d2 d3 d4 d5', 'an older snapshot reads the rows keyed by the dropped column after the drop');
$domain_holder->query_safe('COMMIT');
$domain_holder->quit;
isnt($filenode->('domained'), $domained, 'DROP DOMAIN ... CASCADE taking the key column writes the table keyless');
$node->safe_psql('postgres', written('domained', "INSERT INTO % VALUES ('d1')"));
is($node->safe_psql('postgres', rows('domained', "SELECT string_agg(v, ' ' ORDER BY v) FROM %")),
	'd1 d1 d2 d3 d4 d5', 'the table made keyless by the drop reads its rows and takes a duplicate');
$node->safe_psql('postgres', 'CREATE TABLE unwound (k int4, v text) USING heap');
$node->safe_psql('postgres', written('unwound', "INSERT INTO % VALUES (1, 'a'), (NULL, 'b'), (2, 'c')"));
my $unwound = $filenode->('unwound');
$node->safe_psql('postgres', q{DO $$BEGIN
	BEGIN
		ALTER TABLE unwound ADD PRIMARY KEY (k);
	EXCEPTION WHEN not_null_violation THEN
		PERFORM doltgres_test_write('unwound', true, 'DELETE FROM unwound WHERE k IS NULL');
	END;
	ALTER TABLE unwound ADD PRIMARY KEY (k);
END$$});
isnt($filenode->('unwound'), $unwound, 'a key change after a caught failed one writes the table');
is($node->safe_psql('postgres', rows('unwound', "SELECT string_agg(k || ' ' || v, ', ') FROM %")),
	'1 a, 2 c', 'the table the DO block keyed reads its rows in key order');

# Section 6's K1 and K2: a sql_drop event trigger running a utility statement
# during DROP DOMAIN ... CASCADE settles only its own statement's level, and the
# outer statement writes the table keyless at its end; a primary key dropped
# through performDeletion outside any utility statement, as a C extension drops
# it, is settled before the commit.
$node->safe_psql('postgres', q{CREATE FUNCTION seen_drop() RETURNS event_trigger LANGUAGE plpgsql AS $$BEGIN
	CREATE TABLE IF NOT EXISTS drop_seen (n int4);
END$$});
$node->safe_psql('postgres', 'CREATE EVENT TRIGGER seen_drop ON sql_drop EXECUTE FUNCTION seen_drop()');
$node->safe_psql('postgres', 'CREATE DOMAIN triggered_domain AS int4');
$node->safe_psql('postgres', 'CREATE TABLE triggered (k triggered_domain PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('triggered', "INSERT INTO % SELECT g, 't' || g FROM generate_series(1, 3) g"));
my $triggered = $filenode->('triggered');
$node->safe_psql('postgres', 'DROP DOMAIN triggered_domain CASCADE');
isnt($filenode->('triggered'), $triggered, 'a drop whose event trigger runs a utility statement writes the table keyless');
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_class WHERE relname = 'drop_seen'"), '1',
	"the event trigger's statement ran");
is($node->safe_psql('postgres', rows('triggered', "SELECT string_agg(v, ' ' ORDER BY v) FROM %")),
	't1 t2 t3', 'the table the drop made keyless reads its rows');
$node->safe_psql('postgres', 'DROP EVENT TRIGGER seen_drop');
$node->safe_psql('postgres', 'CREATE TABLE deleted_key (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('deleted_key', "INSERT INTO % SELECT g, 'p' || g FROM generate_series(1, 3) g"));
my $deleted_key = $filenode->('deleted_key');
$node->safe_psql('postgres', "SELECT doltgres_test_drop_constraint('deleted_key', 'deleted_key_pkey')");
isnt($filenode->('deleted_key'), $deleted_key, 'a primary key dropped through performDeletion is settled by commit');
$node->safe_psql('postgres', written('deleted_key', "INSERT INTO % VALUES (1, 'p1')"));
is($node->safe_psql('postgres', rows('deleted_key', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k, v) FROM %")),
	'1 p1, 1 p1, 2 p2, 3 p3', 'the table made keyless by commit takes a duplicate');

# Section 6's live rows (PRevPF 8): in the transaction that adds the key, an
# update of a row leaves its old version deleted by that transaction, which
# isn't live, and its new version inserted by it, which is; the key is added and
# the rows read as heap's twin's.
for my $table ('own_updated', 'heap_own_updated')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
	$node->safe_psql('postgres',
		'BEGIN; ' . written($table, 'UPDATE % SET k = 3 WHERE k = 1') . "; ALTER TABLE $table ADD PRIMARY KEY (k); COMMIT");
}
is( $node->safe_psql('postgres', rows('own_updated', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k + 0) FROM %")),
	$node->safe_psql('postgres', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k + 0) FROM heap_own_updated"),
	"a key added after the same transaction's update reads its rows as heap's twin's");

# Section 6's ordering (PRevPF 8's base rows first): a base row and a delta row
# that share the new key, the delta row's writer ending the base row by an
# update, are one chain in that order, so an older snapshot reads the base row
# and a new one the update's.
$node->safe_psql('postgres', 'CREATE TABLE chain_keyed (k int4, v text) USING heap');
$node->safe_psql('postgres', written('chain_keyed', "INSERT INTO % VALUES (1, 'x'), (2, 'y')"));
$node->safe_psql('postgres', "SELECT doltgres_test_fold('chain_keyed')");
my $chain_holder = $node->background_psql('postgres');
my $chain_held = $chain_holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_export_snapshot()');
$node->safe_psql('postgres', written('chain_keyed', 'UPDATE % SET k = 9 WHERE k = 1'));
$node->safe_psql('postgres', 'ALTER TABLE chain_keyed ADD PRIMARY KEY (v)');
my $chain_read = rows('chain_keyed', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY v || '') FROM %");
is( $node->safe_psql('postgres',
		"BEGIN ISOLATION LEVEL REPEATABLE READ; SET TRANSACTION SNAPSHOT '$chain_held'; $chain_read; COMMIT"),
	'1 x, 2 y', 'an older snapshot reads the base row a delta row of its new key ended');
is($node->safe_psql('postgres', $chain_read), '9 x, 2 y', "a new snapshot reads the update that ended it");
$chain_holder->query_safe('COMMIT');
$chain_holder->quit;

# Bugs row 114 on a server (PRevPF 8's staging): a commit waiting on a standby
# that never connects stays in the proc array with an early end, which lowers a
# fold's point below a later delete's commit. With the overlapping key's base
# rows moved as frozen AddRows, an unlogged table's fold at that point would keep
# two rows of the key and is refused, while a logged table's AddRows, whose
# records end past the point, stay unapplied; both read as their twins do, and
# once the waiting commit is cancelled out of the proc array the unlogged table's
# fold installs the survivor.
$node->safe_psql('postgres', 'CREATE TABLE sync_other (k int4)');
for my $table ('sync_overlapped', 'unlogged_sync_overlapped')
{
	my $unlogged = $table =~ /^unlogged/ ? 'UNLOGGED ' : '';
	$node->safe_psql('postgres', "CREATE ${unlogged}TABLE $table (k int4, v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES (1, 'a'), (2, 'a'), (3, 'b')"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	$node->safe_psql('postgres', "ALTER SYSTEM SET synchronous_standby_names = 'absent'");
	$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
	my $waiting = $node->background_psql('postgres');
	$waiting->query_until(qr/started/,
		"SET synchronous_commit = on;\n\\echo started\n" . written('sync_other', 'INSERT INTO % VALUES (1)') . ";\n");
	$node->poll_query_until('postgres', "SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'SyncRep'");
	$node->safe_psql('postgres',
		"SET synchronous_commit = local; " . written($table, 'DELETE FROM % WHERE k = 1') . "; ALTER TABLE $table ADD PRIMARY KEY (v)");
	my $folded = $node->safe_psql('postgres', "SET synchronous_commit = local; SELECT doltgres_test_fold('$table')");
	is($folded, 'f', "$table: a fold whose point a waiting commit lowered below the delete installs no base")
	  if $unlogged;
	my $read = rows($table, "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k) FROM %");
	is($node->safe_psql('postgres', $read), '2 a, 3 b', "$table: it reads the key change's rows while the commit waits");
	$node->safe_psql('postgres', "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'SyncRep'");
	$waiting->quit;
	$node->safe_psql('postgres', 'SET synchronous_commit = local; ALTER SYSTEM RESET synchronous_standby_names');
	$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
	is($node->safe_psql('postgres', "SET synchronous_commit = local; SELECT doltgres_test_fold('$table')"),
		't', "$table: once the waiting commit leaves the proc array a fold installs the key's survivor")
	  if $unlogged;
	is($node->safe_psql('postgres', $read), '2 a, 3 b', "$table: it reads the survivor after that fold");
}

# CommPF 8's ruling on frozen versions: a frozen insert's rows, as COPY FREEZE
# writes them, whose records lie past a fold's point that a waiting commit
# lowered, stay in the delta and the fold applies nothing; once the commit
# leaves the proc array a fold takes them into the base; the rows read the same
# throughout.
$node->safe_psql('postgres', "CREATE TABLE frozen_source AS SELECT g AS k, 'f' || g AS v FROM generate_series(1, 3) g");
$node->safe_psql('postgres', 'CREATE TABLE frozen_folded (LIKE frozen_source)');
$node->safe_psql('postgres', "ALTER SYSTEM SET synchronous_standby_names = 'absent'");
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
my $frozen_waiting = $node->background_psql('postgres');
$frozen_waiting->query_until(qr/started/,
	"SET synchronous_commit = on;\n\\echo started\n" . written('sync_other', 'INSERT INTO % VALUES (2)') . ";\n");
$node->poll_query_until('postgres', "SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'SyncRep'");
$node->safe_psql('postgres',
	"SET synchronous_commit = local; SELECT doltgres_test_insert('frozen_folded', 'frozen_source', true, 4)");
my $frozen_read = rows('frozen_folded', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k) FROM %");
is($node->safe_psql('postgres', "SET synchronous_commit = local; SELECT doltgres_test_fold('frozen_folded')"),
	'f', "a fold below a frozen insert's records applies none of its rows");
is($node->safe_psql('postgres', $frozen_read), '1 f1, 2 f2, 3 f3', 'the frozen rows read from the delta');
$node->safe_psql('postgres', "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'SyncRep'");
$frozen_waiting->quit;
$node->safe_psql('postgres', 'SET synchronous_commit = local; ALTER SYSTEM RESET synchronous_standby_names');
$node->safe_psql('postgres', 'SELECT pg_reload_conf()');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('frozen_folded')"),
	't', "a fold past a frozen insert's records takes its rows into the base");
is($node->safe_psql('postgres', $frozen_read), '1 f1, 2 f2, 3 f3', 'the frozen rows read from the base');

# Section 6's key change through ADD PRIMARY KEY USING INDEX (PRevPF 8): a
# unique index promoted to a keyless table's primary key by
# index_constraint_create, one of the key's index.c sites, writes the table once
# under the promoted index's columns, every row keeping its ctid, xmin, and
# values, read in the new key's order.
for my $table ('promoted', 'heap_promoted')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % SELECT g, repeat('p', g * 30) FROM generate_series(30, 1, -1) g"));
}
$node->safe_psql('postgres', "SELECT doltgres_test_fold('promoted')");
$node->safe_psql('postgres', written($_, "UPDATE % SET v = v || 'u' WHERE mod(k, 4) = 0")) for ('promoted', 'heap_promoted');
my $promoted_rows = $node->safe_psql('postgres', rows('promoted', $keyed_rows));
my $promoted_files = $filenodes->('promoted');
$node->safe_psql('postgres',
	'CREATE UNIQUE INDEX promoted_k ON promoted (k); ALTER TABLE promoted ADD PRIMARY KEY USING INDEX promoted_k');
isnt($filenodes->('promoted'), $promoted_files, 'ADD PRIMARY KEY USING INDEX writes the table again');
is($node->safe_psql('postgres', rows('promoted', $keyed_rows)), $promoted_rows,
	'ADD PRIMARY KEY USING INDEX keeps every row with its ctid, xmin, and values');
is( $node->safe_psql('postgres', rows('promoted', "SELECT string_agg(k::text, ' ') FROM %")),
	$node->safe_psql('postgres', "SELECT string_agg(k::text, ' ' ORDER BY k) FROM heap_promoted"),
	"the table's scan gives its rows in the promoted index's order");

# Section 6's settle of a drop made outside any utility statement (PRevPF 8's
# K2): a test function that drops a keyed table's primary key through
# performDeletion, as a C extension does, leaves the table's key to the
# transaction's pre-commit settle, which writes it keyless by commit with every
# row kept.
$node->safe_psql('postgres', 'CREATE TABLE performed_key (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('performed_key', "INSERT INTO % SELECT g, repeat('d', g * 30) FROM generate_series(1, 20) g"));
my $deleted_rows = $node->safe_psql('postgres', rows('performed_key', $keyed_rows));
my $deleted_files = $filenodes->('performed_key');
$node->safe_psql('postgres', "BEGIN; SELECT doltgres_test_drop_constraint('performed_key', 'performed_key_pkey'); COMMIT");
isnt($filenodes->('performed_key'), $deleted_files,
	"a drop through performDeletion from a test function has its table's key settled by commit");
is($node->safe_psql('postgres', rows('performed_key', $keyed_rows)), $deleted_rows,
	'the table settled keyless by commit reads the same rows');

# Section 6's duplicate key: a key change whose new key two live rows share
# raises the reference's 23505 for the key's index, with its detail, and
# changes nothing.
$node->safe_psql('postgres', 'CREATE TABLE duplicate_keyed (k int4, v text) USING heap');
$node->safe_psql('postgres', written('duplicate_keyed', "INSERT INTO % VALUES (1, 'a'), (1, 'b'), (2, 'c')"));
my $dup_files = $filenodes->('duplicate_keyed');
my (undef, undef, $dup_error) =
  $node->psql('postgres', "\\set VERBOSITY verbose\nALTER TABLE duplicate_keyed ADD PRIMARY KEY (k)");
like(
	$dup_error,
	qr/ERROR:  23505: could not create unique index "duplicate_keyed_pkey"\nDETAIL:  Key \(k\)=\(1\) is duplicated\./,
	'a key change over two live rows with one new key raises 23505 as the reference does');
is($filenodes->('duplicate_keyed'), $dup_files, 'the failed key change writes nothing');

# Section 6's duplicate key under a class (PRevPF 8's P1): citext's class calls
# 'a' and 'A' one key though their bytes differ, so a key change over both live,
# in the delta, in the base, or one in each, raises the reference's 23505 naming
# the earlier row and changes nothing; with 'A' deleted first, the key is added
# and the rows read as heap's twin's.
$node->safe_psql('postgres', 'CREATE EXTENSION citext');
my $classed_rows = "SELECT string_agg(ctid::text || ' ' || xmin || ' ' || c || ' ' || n, ', ' ORDER BY ctid) FROM %";
for my $table ('classed_delta', 'classed_base', 'classed_mixed')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (c citext, n int4)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES ('a', 1)"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')") if $table eq 'classed_mixed';
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES ('A', 2)"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')") if $table eq 'classed_base';
	my $rows = $node->safe_psql('postgres', rows($table, $classed_rows));
	my $files = $filenodes->($table);
	my (undef, undef, $error) =
	  $node->psql('postgres', "\\set VERBOSITY verbose\nALTER TABLE $table ADD PRIMARY KEY (c)");
	like(
		$error,
		qr/ERROR:  23505: could not create unique index "${table}_pkey"\nDETAIL:  Key \(c\)=\(a\) is duplicated\./,
		"$table: a key change over two live rows its class calls one key raises 23505 as the reference does");
	is($filenodes->($table), $files, "$table: the failed key change writes nothing");
	is($node->safe_psql('postgres', rows($table, $classed_rows)), $rows, "$table: its rows are unchanged");
}
for my $table ('classed_deleted', 'heap_classed_deleted')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (c citext, n int4)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES ('a', 1), ('A', 2), ('b', 3)"));
	$node->safe_psql('postgres', written($table, 'DELETE FROM % WHERE n = 2'));
	$node->safe_psql('postgres', "ALTER TABLE $table ADD PRIMARY KEY (c)");
}
is( $node->safe_psql('postgres', rows('classed_deleted', "SELECT string_agg(c || ' ' || n, ', ' ORDER BY n) FROM %")),
	$node->safe_psql('postgres', "SELECT string_agg(c || ' ' || n, ', ' ORDER BY n) FROM heap_classed_deleted"),
	"a key its class calls one, its other row deleted first, is added and reads as heap's twin");

# Section 6's design cases (PRevPF 8's P2). With a utility hook that calls no
# earlier hook, as an extension's may, DROP DOMAIN ... CASCADE taking the key
# column still writes the table keyless, since the settle's site is in
# ProcessUtility itself.
$node->safe_psql('postgres', 'CREATE DOMAIN unchained_domain AS int4');
$node->safe_psql('postgres', 'CREATE TABLE unchained (k unchained_domain PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('unchained', "INSERT INTO % SELECT g, 'u' || g FROM generate_series(1, 3) g"));
my $unchained = $filenode->('unchained');
$node->safe_psql('postgres', 'SELECT doltgres_test_unchain(); DROP DOMAIN unchained_domain CASCADE');
isnt($filenode->('unchained'), $unchained,
	'with a utility hook that calls no earlier hook, DROP DOMAIN ... CASCADE writes the table keyless');
$node->safe_psql('postgres', written('unchained', "INSERT INTO % VALUES ('u1')"));
is($node->safe_psql('postgres', rows('unchained', "SELECT string_agg(v, ' ' ORDER BY v) FROM %")),
	'u1 u1 u2 u3', 'the table the unchained drop made keyless reads its rows and takes a duplicate');

# REINDEX of a primary key and REINDEX CONCURRENTLY of an empty table's key
# leave the key as it is and write nothing to the table.
$node->safe_psql('postgres', 'CREATE TABLE reindexed_key (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('reindexed_key', "INSERT INTO % SELECT g, 'r' || g FROM generate_series(1, 5) g"));
$node->safe_psql('postgres', 'CREATE TABLE reindexed_empty (k int4 PRIMARY KEY) USING heap');
my $reindexed = $filenodes->('reindexed_key') . ' ' . $filenode->('reindexed_empty');
$node->safe_psql('postgres', 'REINDEX INDEX reindexed_key_pkey');
$node->safe_psql('postgres', 'REINDEX INDEX CONCURRENTLY reindexed_empty_pkey');
is($filenodes->('reindexed_key') . ' ' . $filenode->('reindexed_empty'), $reindexed,
	"REINDEX of a key and REINDEX CONCURRENTLY of an empty table's key write nothing to their tables");
is($node->safe_psql('postgres', "SELECT indisprimary FROM pg_index WHERE indexrelid = 'reindexed_empty_pkey'::regclass"),
	't', "the empty table's rebuilt index is still its primary key");

# REINDEX CONCURRENTLY of a key while a transaction inserts rows, before and
# after the reindex waits for it, writes nothing to the table, and every row
# reads after.
$node->safe_psql('postgres', 'CREATE TABLE reindexed_live (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('reindexed_live', "INSERT INTO % SELECT g, 'l' || g FROM generate_series(1, 3) g"));
my $reindexed_live = $filenodes->('reindexed_live');
my $live_inserter = $node->background_psql('postgres');
$live_inserter->query_safe('BEGIN; ' . written('reindexed_live', "INSERT INTO % VALUES (4, 'l4')"));
my $live_reindexer = $node->background_psql('postgres');
$live_reindexer->query_until(qr/started/, "\\echo started\nREINDEX INDEX CONCURRENTLY reindexed_live_pkey;\n");
$node->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE 'REINDEX INDEX CONCURRENTLY%'");
$live_inserter->query_safe(written('reindexed_live', "INSERT INTO % VALUES (5, 'l5')"));
$live_inserter->query_safe('COMMIT');
$live_inserter->quit;
$live_reindexer->quit;
is($filenodes->('reindexed_live'), $reindexed_live, 'REINDEX CONCURRENTLY of a key under inserts writes nothing to the table');
is($node->safe_psql('postgres', rows('reindexed_live', "SELECT string_agg(k || ' ' || v, ', ' ORDER BY k + 0) FROM %")),
	'1 l1, 2 l2, 3 l3, 4 l4, 5 l5', 'every row reads after the concurrent reindex');

# VACUUM FULL's and CLUSTER's copy of a folded keyed table writes it once, one
# built base, under the key, which still refuses a duplicate with the reference's 23505.
$node->safe_psql('postgres', 'CREATE TABLE rewritten_once (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('rewritten_once', "INSERT INTO % SELECT g, 'o' || g FROM generate_series(1, 5) g"));
for my $index (undef, 'rewritten_once_pkey')
{
	my $how = defined $index ? 'CLUSTER' : 'VACUUM FULL';
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('rewritten_once')");
	my $before = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$node->safe_psql('postgres', rewritten('rewritten_once', $index, defined $index ? 'true' : 'false'));
	is( $node->safe_psql('postgres',
			"SELECT count(*) FROM pg_get_wal_records_info('$before', pg_current_wal_flush_lsn()) WHERE record_type = 'BaseAdvanced'"
		),
		'1',
		"$how\'s copy of a keyed table logs one built base");
}
my (undef, undef, $once_error) = $node->psql('postgres',
	"\\set VERBOSITY verbose\n" . written('rewritten_once', "INSERT INTO % VALUES (1, 'again')"));
like(
	$once_error,
	qr/ERROR:  23505: duplicate key value violates unique constraint "rewritten_once_pkey"\nDETAIL:  Key \(k\)=\(1\) already exists\./,
	'the rewritten table keeps its key, which refuses a duplicate as the reference does');

# DROP TABLE of a keyed table writes nothing to it before the drop.
$node->safe_psql('postgres', 'CREATE TABLE dropped_whole (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('dropped_whole', "INSERT INTO % SELECT g, 'w' || g FROM generate_series(1, 5) g"));
my $before_drop = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->safe_psql('postgres', 'DROP TABLE dropped_whole');
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_get_wal_records_info('$before_drop', pg_current_wal_flush_lsn()) WHERE record_type IN ('BaseAdvanced', 'RowChange', 'Chunk')"
	),
	'0',
	'DROP TABLE of a keyed table writes nothing to it');

# ADD PRIMARY KEY on a table whose foreign key references its own column
# writes the table under the key, its rows read as heap's twin's, and the
# foreign key still refuses a row whose parent is missing with the reference's
# 23503.
for my $table ('self_referenced', 'heap_self_referenced')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 UNIQUE, p int4 REFERENCES $table (k), v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', written($table, "INSERT INTO % VALUES (1, NULL, 'a'), (2, 1, 'b'), (3, 2, 'c')"));
}
my $self_files = $filenodes->('self_referenced');
$node->safe_psql('postgres', "ALTER TABLE $_ ADD PRIMARY KEY (k)") for ('self_referenced', 'heap_self_referenced');
isnt($filenodes->('self_referenced'), $self_files,
	'ADD PRIMARY KEY on a table with a self-referencing foreign key writes it under the key');
is( $node->safe_psql('postgres', rows('self_referenced', "SELECT string_agg(k || ' ' || coalesce(p, 0) || ' ' || v, ', ') FROM %")),
	$node->safe_psql('postgres', "SELECT string_agg(k || ' ' || coalesce(p, 0) || ' ' || v, ', ' ORDER BY k) FROM heap_self_referenced"),
	"its rows read in the key's order as heap's twin's");
my (undef, undef, $self_error) = $node->psql('postgres',
	"\\set VERBOSITY verbose\n" . written('self_referenced', "INSERT INTO % VALUES (4, 9, 'd')"));
like(
	$self_error,
	qr/ERROR:  23503: insert or update on table "self_referenced" violates foreign key constraint "self_referenced_p_fkey"\nDETAIL:  Key \(p\)=\(9\) is not present in table "self_referenced"\./,
	'the foreign key refuses a missing parent after the key change as the reference does');

# A unique index made on a dropped key's columns writes nothing to the table,
# which stays keyless.
$node->safe_psql('postgres', 'CREATE TABLE unkeyed_unique (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', written('unkeyed_unique', "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
$node->safe_psql('postgres', 'ALTER TABLE unkeyed_unique DROP CONSTRAINT unkeyed_unique_pkey');
my $unkeyed_files = $filenodes->('unkeyed_unique');
$node->safe_psql('postgres', 'CREATE UNIQUE INDEX unkeyed_unique_k ON unkeyed_unique (k)');
is($filenodes->('unkeyed_unique'), $unkeyed_files, "a unique index on a dropped key's columns writes nothing to the table");

# Section 6's freeze horizon: a key change's new relfilenode keeps the table's
# own relfrozenxid, at or below every carried version's writer, where a new
# relfilenode would otherwise take the current horizon; the switch's relcache
# routine reaches it.
$node->safe_psql('postgres', 'CREATE TABLE refrozen_key (k int4, v text) USING heap');
$node->safe_psql('postgres', written('refrozen_key', "INSERT INTO % SELECT g, 'f' || g FROM generate_series(1, 5) g"));
$node->safe_psql('postgres', 'SELECT pg_current_xact_id()') for 1 .. 3;
my $frozen_before = $node->safe_psql('postgres', "SELECT relfrozenxid FROM pg_class WHERE relname = 'refrozen_key'");
my $frozen_files = $filenodes->('refrozen_key');
$node->safe_psql('postgres', 'ALTER TABLE refrozen_key ADD PRIMARY KEY (k)');
isnt($filenodes->('refrozen_key'), $frozen_files, 'the key change writes the table into a new relfilenode');
is($node->safe_psql('postgres', "SELECT relfrozenxid FROM pg_class WHERE relname = 'refrozen_key'"),
	$frozen_before, "a key change's new relfilenode keeps the table's relfrozenxid");

# Section 6 under the switch (CommPF 10's cases): on fork tables with rows,
# ADD PRIMARY KEY, DROP CONSTRAINT of the key and then a duplicate's insert,
# DROP COLUMN of the key and then a write, and the key moved to another column
# each leave the rows equal to heap's twin's, after a fold, and after a restart.
my %key_changes = (
	switch_added => [ '(k int4, v text)', 'ALTER TABLE % ADD PRIMARY KEY (k)', "INSERT INTO % VALUES (21, 'v21')" ],
	switch_dropped =>
	  [ '(k int4 PRIMARY KEY, v text)', 'ALTER TABLE % DROP CONSTRAINT %_pkey', "INSERT INTO % VALUES (1, 'again')" ],
	switch_column_dropped =>
	  [ '(k int4 PRIMARY KEY, v text)', 'ALTER TABLE % DROP COLUMN k', "INSERT INTO % VALUES ('v21')" ],
	switch_moved => [
		'(k int4 PRIMARY KEY, v text)', 'ALTER TABLE % DROP CONSTRAINT %_pkey, ADD PRIMARY KEY (v)',
		"INSERT INTO % VALUES (1, 'v21')"
	],);
my $changed_rows = sub {
	my ($table) = @_;
	return $node->safe_psql('postgres', "SELECT string_agg(t::text, ', ' ORDER BY t::text) FROM $table t");
};
for my $name (sort keys %key_changes)
{
	my ($definition, $change, $write) = @{ $key_changes{$name} };
	for my $table ($name, "heap_$name")
	{
		my $using = $table eq $name ? ' USING heap' : '';
		$node->safe_psql('postgres', "CREATE TABLE $table $definition$using");
		$node->safe_psql('postgres', "INSERT INTO $table SELECT g, 'v' || g FROM generate_series(1, 20) g");
		$node->safe_psql('postgres', $change =~ s/%/$table/gr);
		$node->safe_psql('postgres', $write =~ s/%/$table/gr);
	}
	is($changed_rows->($name), $changed_rows->("heap_$name"), "$name: the key change and a write read as heap's twin's");
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$name')");
	is($changed_rows->($name), $changed_rows->("heap_$name"), "$name: and after a fold");
}
$node->restart;
is($changed_rows->($_), $changed_rows->("heap_$_"), "$_: and after a restart") for sort keys %key_changes;
is($changed_rows->('own_updated'), $changed_rows->('heap_own_updated'),
	"a key added after the same transaction's update keeps its new storage's tuple id counter past the update's, so a restart loads its checkpoint's runs"
);

# Section 6's switch-keyed cases. ALTER COLUMN TYPE of a key column, of one
# column of a two-column key, and of another column rewrites the table under
# the catalog's primary key, reading the old storage's keys by the types they
# were written with after some rows were updated: its rows read as heap's
# twin's, and a duplicate key is refused with the reference's 23505.
for my $typed (
	[ 'typed_key', 'k int4 PRIMARY KEY', 'k TYPE int8', '(1, 1, 1, \'again\')', 'k', '1' ],
	[ 'typed_pair', 'k int4, j int4, PRIMARY KEY (k, j)', 'k TYPE int8', '(1, 1, 1, \'again\')', 'k, j', '1, 1' ],
	[ 'typed_other', 'k int4, j int4, PRIMARY KEY (k, j)', 'n TYPE int8', '(1, 1, 1, \'again\')', 'k, j', '1, 1' ])
{
	my ($name, $key, $alter, $duplicate, $columns, $values) = @$typed;
	for my $table ($name, "heap_$name")
	{
		my $using = $table eq $name ? ' USING heap' : '';
		my $j = $key =~ /\bj\b/ ? '' : ', j int4';
		$node->safe_psql('postgres', "CREATE TABLE $table ($key$j, n int4, v text)$using");
		$node->safe_psql('postgres', "INSERT INTO $table SELECT g, g % 4, g * 2, 'v' || g FROM generate_series(1, 20) g");
		$node->safe_psql('postgres', "UPDATE $table SET v = v || 'u' WHERE k % 3 = 0");
		$node->safe_psql('postgres', "ALTER TABLE $table ALTER COLUMN $alter");
	}
	is($changed_rows->($name), $changed_rows->("heap_$name"), "$name: ALTER COLUMN TYPE's rewrite reads as heap's twin's");
	my (undef, undef, $refused) =
	  $node->psql('postgres', "\\set VERBOSITY verbose\nINSERT INTO $name VALUES $duplicate");
	like(
		$refused,
		qr/ERROR:  23505: duplicate key value violates unique constraint "${name}_pkey"\nDETAIL:  Key \(\Q$columns\E\)=\(\Q$values\E\) already exists\./,
		"$name: the rewritten table's key refuses a duplicate as the reference does");
}

# A REINDEX of a primary key while another session's cursor is open over the
# table waits for the cursor's transaction, whose plan locked the index, while
# the cursor reads the rest of the table's rows; once it commits, the rebuilt
# key finds every row, as on heap.
my %cursored;
for my $table ('cursored', 'heap_cursored')
{
	my $using = $table eq 'cursored' ? ' USING heap' : '';
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4 PRIMARY KEY, v text)$using");
	$node->safe_psql('postgres', "INSERT INTO $table SELECT g, 'v' || g FROM generate_series(1, 20) g");
	my $reader = $node->background_psql('postgres');
	my $reindexer = $node->background_psql('postgres');
	$reader->query_safe("BEGIN; DECLARE c CURSOR FOR SELECT k || ' ' || v FROM $table");
	my $first = $reader->query_safe('FETCH 5 FROM c');
	$reindexer->query_until(qr/started/, "\\echo started\nREINDEX INDEX ${table}_pkey;\n");
	$node->poll_query_until('postgres',
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE 'REINDEX INDEX ${table}_pkey%'")
	  or die 'the REINDEX never waited';
	my $rest = $reader->query_safe('FETCH ALL FROM c');
	$reader->query_safe('COMMIT');
	$reindexer->query_safe('SELECT 1');
	$_->quit for $reader, $reindexer;
	my $found = $node->safe_psql('postgres',
		"SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM $table WHERE k > 0");
	$cursored{$table} = join(', ', sort split /\n/, "$first\n$rest") . "; $found by the key";
}
is($cursored{cursored}, $cursored{heap_cursored},
	"a cursor open across a waiting REINDEX of the key reads the table's rows, and the rebuilt key finds them, as on heap");

# A unique index made after the key's drop refuses a duplicate with the
# reference's 23505.
$node->safe_psql('postgres', 'CREATE TABLE unkeyed_switch (k int4 PRIMARY KEY, v text) USING heap');
$node->safe_psql('postgres', "INSERT INTO unkeyed_switch SELECT g, 'v' || g FROM generate_series(1, 5) g");
$node->safe_psql('postgres', 'ALTER TABLE unkeyed_switch DROP CONSTRAINT unkeyed_switch_pkey');
$node->safe_psql('postgres', 'CREATE UNIQUE INDEX unkeyed_switch_k ON unkeyed_switch (k)');
my (undef, undef, $unkeyed_refused) =
  $node->psql('postgres', "\\set VERBOSITY verbose\nINSERT INTO unkeyed_switch VALUES (1, 'again')");
like(
	$unkeyed_refused,
	qr/ERROR:  23505: duplicate key value violates unique constraint "unkeyed_switch_k"\nDETAIL:  Key \(k\)=\(1\) already exists\./,
	"a unique index on a dropped key's columns refuses a duplicate as the reference does");

# ADD PRIMARY KEY USING INDEX of a fork btree rebuilds it into a new
# relfilenode under the new key, which finds every row, and unlinks its old one.
$node->safe_psql('postgres', 'CREATE TABLE promoted_switch (k int4, v text) USING heap');
$node->safe_psql('postgres', "INSERT INTO promoted_switch SELECT g, 'v' || g FROM generate_series(1, 20) g");
$node->safe_psql('postgres', 'CREATE UNIQUE INDEX promoted_switch_k ON promoted_switch (k)');
my $promoted_old = $node->safe_psql('postgres', "SELECT pg_relation_filepath('promoted_switch_k')");
$node->safe_psql('postgres', 'ALTER TABLE promoted_switch ADD PRIMARY KEY USING INDEX promoted_switch_k');
isnt($node->safe_psql('postgres', "SELECT pg_relation_filepath('promoted_switch_k')"),
	$promoted_old, 'ADD PRIMARY KEY USING INDEX of a fork btree rebuilds it into a new relfilenode');
is( $node->safe_psql('postgres',
		'SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM promoted_switch WHERE k > 0'),
	'20', 'the rebuilt index finds every row');
$node->safe_psql('postgres', 'CHECKPOINT');
ok(!-e $node->data_dir . "/$promoted_old", "and the index's old relfilenode's file is unlinked");

# Section 6's duplicate key in a rewrite (RevA's S2): ADD PRIMARY KEY beside a
# subcommand that rewrites the table copies its rows into a transient table
# under the new key, where a second row of one key, by its bytes or by its
# class (citext's 'a' and 'A'), raises the reference's 23505 for the key's
# index, with its detail naming the earlier row, and the table and its key's
# names, as the index's rebuild raises it, and changes nothing.
for my $case (
	[ 'int4', "(1, 1), (1, 2)", '1', 'ADD COLUMN x int4 DEFAULT (random() * 10)::int4' ],
	[ 'int4', "(1, 1), (1, 2)", '1', 'ALTER COLUMN n TYPE int8' ],
	[ 'int4', "(1, 1), (1, 2)", '1', 'ALTER COLUMN k TYPE int8' ],
	[ 'int4', "(1, 1), (1, 2)", '1', 'SET UNLOGGED' ],
	[ 'citext', "('a', 1), ('A', 2)", 'a', 'ADD COLUMN x int4 DEFAULT (random() * 10)::int4' ],
	[ 'citext', "('a', 1), ('A', 2)", 'a', 'ALTER COLUMN n TYPE int8' ],
	[ 'citext', "('a', 1), ('A', 2)", 'a', 'SET UNLOGGED' ])
{
	my ($type, $rows, $named, $subcommand) = @$case;
	$node->safe_psql('postgres',
		"DROP TABLE IF EXISTS rebuilt; CREATE TABLE rebuilt (k $type, n int4) USING heap; INSERT INTO rebuilt VALUES $rows");
	my (undef, undef, $error) = $node->psql('postgres',
		"\\set VERBOSITY verbose\nALTER TABLE rebuilt ADD PRIMARY KEY (k), $subcommand");
	like(
		$error,
		qr/ERROR:  23505: could not create unique index "rebuilt_pkey"\nDETAIL:  Key \(k\)=\(\Q$named\E\) is duplicated\.\nSCHEMA NAME:  public\nTABLE NAME:  rebuilt\nCONSTRAINT NAME:  rebuilt_pkey\n/,
		"$type: ADD PRIMARY KEY with $subcommand over a duplicate key raises 23505 as the reference does");
	is( $node->safe_psql(
			'postgres',
			"SELECT relpersistence::text || ' ' || (SELECT count(*) FROM rebuilt) || ' ' || (SELECT count(*) FROM pg_index WHERE indrelid = 'rebuilt'::regclass) FROM pg_class WHERE relname = 'rebuilt'"
		),
		'p 2 0',
		"$type: the refused ADD PRIMARY KEY with $subcommand changes nothing");
}

$node->stop;
done_testing();
