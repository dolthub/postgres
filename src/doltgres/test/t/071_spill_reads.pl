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

# Reads held across a fold that applies a committed transaction's spilled
# versions and drops them from the delta (bugs rows 240, 241, and 242): each
# read, held after it read its storage's spill slots, moves to the fold's base
# at its next read and reads every row once. The node's delta_memory is 4 MB,
# so a batch of a few thousand rows spills and stays inside the budget.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('spill_reads');
$node->init;
$node->append_conf('postgresql.conf',
	"delta_memory = 4MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nmax_parallel_workers_per_gather = 0\nautovacuum = off"
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
});

# launcher_held holds the fold launcher at its injection point and waits for
# its fold workers to end, so no fold but a case's own installs a base under
# the case's held read before that fold; launcher_freed lets the launcher go
# on. The node runs without autovacuum, whose workers' snapshots would hold
# the fold horizon below a case's batch.
sub launcher_held
{
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
	$node->poll_query_until('postgres',
		q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'}
	) or die 'the fold launcher never waited';
	$node->poll_query_until('postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'fold worker'}
	) or die 'a fold worker kept running';
}

sub launcher_freed
{
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
	);
}

# Bugs row 242: an UPDATE ... FROM through a nested loop whose outer scan reads
# a source whose rows its writer spilled, held at its first read of the
# source's spills while a fold of the source installs a base and drops the
# spills, updates every row, as heap's twin's does.
my %joined;
for my $am ('', 'USING doltgres_twin')
{
	my $p = $am eq '' ? '' : 'heap_';
	launcher_held() if $am eq '';
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE ${p}joined_source (k int4, v text) $am;
INSERT INTO ${p}joined_source SELECT g, repeat('s', 400) FROM generate_series(1, 3000) g;
CREATE TABLE ${p}joined_target (k int4 PRIMARY KEY, v text) $am;
INSERT INTO ${p}joined_target SELECT g, 't' FROM generate_series(1, 3000) g;
});
	my $plan =
	  "SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;";
	my $update =
	  "UPDATE ${p}joined_target t SET v = s.v || 'u' FROM ${p}joined_source s WHERE t.k = s.k";
	if ($am eq '')
	{
		my $updater = $node->background_psql('postgres');
		$updater->query_safe(
			"$plan SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
		);
		$updater->query_until(qr/started/, "\\echo started\n$update;\n");
		$node->wait_for_event('client backend', 'doltgres-spill-slots-read');
		is( $node->safe_psql(
				'postgres', "SELECT doltgres_test_fold('joined_source')"),
			't',
			"a fold under the held UPDATE ... FROM's source scan installs a base"
		);
		launcher_freed();
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
		"SELECT count(*) || ' ' || md5(string_agg(k || v, ',' ORDER BY k)) FROM ${p}joined_target"
	);
}
is($joined{''}, $joined{heap_},
	"an UPDATE ... FROM whose source scan a fold's drop passes under updates every row as heap's twin's"
);

# Bugs rows 240 and 241: a scan held after it read its storage's spill slots,
# while a fold installs a base that applies a committed transaction's spilled
# rows and drops them from the delta, moves to that base at its next read and
# reads every row once.
$node->safe_psql(
	'postgres', q{
CREATE TABLE read_source (v text) USING doltgres_twin;
INSERT INTO read_source SELECT 'r' || i || repeat('x', 400) FROM generate_series(1, 3000) i;
CREATE TABLE read_rows (v text);
});
my $read_count =
  q{SELECT count(*) FROM doltgres_test_scan('read_rows', false, -1)};

# held_scan starts a scan of read_rows in a session of its own: a cursor
# that has fetched its first 1,000 rows, held where its next fetch reads the
# storage's spill slots, and returns the session.
sub held_scan
{
	my $scan = $node->background_psql('postgres');
	$scan->query_safe('BEGIN; DECLARE c CURSOR FOR SELECT v FROM read_rows');
	$scan->query_safe('FETCH 1000 FROM c');
	$scan->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
	);
	$scan->query_until(qr/held/, "\\echo held\nFETCH ALL FROM c;\n");
	$node->wait_for_event('client backend', 'doltgres-spill-slots-read');
	return $scan;
}

# released lets a held scan go on and returns the count of the rows it read,
# its first 1,000 with the rest.
sub released
{
	my ($scan) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-spill-slots-read'); SELECT injection_points_wakeup('doltgres-spill-slots-read')"
	);
	my @rest = split /\n/, $scan->query_safe('SELECT 1');
	$scan->query_safe('COMMIT');
	$scan->quit;
	return 1000 + @rest - 1;
}
launcher_held();
$node->safe_psql('postgres',
	q{SELECT count(*) FROM doltgres_test_insert('read_rows', 'read_source', true, 0)}
);
my $scan = held_scan();
is($node->safe_psql('postgres', q{SELECT doltgres_test_fold('read_rows')}),
	't', 'a fold under the held scan installs a base');
launcher_freed();
is(released($scan), 3000,
	'a scan held across a fold that applied its spilled rows reads every row'
);
is($node->safe_psql('postgres', $read_count),
	'3000', 'and a scan after it reads every row');

# Bugs rows 240 and 241's point reads: a fetch of a spilled row by its tuple id,
# held inside its read of the delta between its tuple id's lookup and its
# versions' read, and an index fetch of it by its row key, held inside its
# read of the delta, each while a fold installs a base holding the row and
# drops its spill, read the row, as the same fetch after the fold does. Each
# held case holds the fold launcher from before its batch's writes, so no fold
# worker the writes ask for applies the batch, or holds the horizon below it,
# before the case's own fold.
$node->safe_psql(
	'postgres', q{
CREATE TABLE point_source USING doltgres_twin AS SELECT 'p' || i || repeat('x', 400) AS v FROM generate_series(1, 3000) i;
CREATE TABLE point_rows (v text);
CREATE TABLE point_one AS SELECT 'p1' || repeat('x', 400) AS v;
});
# copied writes the point table's batch of spilled rows and returns its first
# row's tuple id, which its first spill holds, and that id as a number.
sub copied
{
	my $tid = $node->safe_psql('postgres',
		q{SELECT min(t) FROM doltgres_test_insert('point_rows', 'point_source', true, 0) t}
	);
	return (
		$tid,
		$node->safe_psql(
			'postgres',
			"SELECT ((('$tid'::tid::text::point)[0])::int8 << 16) | (('$tid'::tid::text::point)[1])::int8"
		));
}
for my $name ('by tuple id', 'by row key')
{
	launcher_held();
	my ($tid, $copy) = copied();
	my $fetch =
	  $name eq 'by tuple id'
	  ? "SELECT doltgres_test_fetch('point_rows', '$tid')"
	  : "SELECT string_agg(r, ' ') FROM doltgres_test_index_fetch_key('point_rows', 'point_one', $copy) r";
	my $at =
	  $name eq 'by tuple id'
	  ? 'doltgres-fetch-keyed'
	  : 'doltgres-spill-slots-read';
	my $point = $node->background_psql('postgres');
	$point->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('$at', 'wait')"
	);
	$point->query_until(qr/held/, "\\echo held\n$fetch;\n");
	$node->wait_for_event('client backend', $at);
	is( $node->safe_psql(
			'postgres', q{SELECT doltgres_test_fold('point_rows')}),
		't',
		"a fold under the held fetch $name installs a base");
	launcher_freed();
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('$at'); SELECT injection_points_wakeup('$at')"
	);
	my $held = $point->query_safe('SELECT 1');
	$point->quit;
	$held =~ s/\n1$//;
	my $after = $node->safe_psql('postgres', $fetch);
	like(
		$after,
		qr/^\(\d+,\d+\) \d+ p1x/,
		"a fetch $name reads the row after the fold");
	is($held, $after,
		"a fetch $name held across a fold that applied its spilled row reads it"
	);
}

# Bugs rows 240 and 241's position kinds: each kind of read the test module
# reaches, held after it read its storage's spill slots while a fold installs a
# base that applies a committed transaction's spilled rows and drops them from
# the delta, moves to that base at its next read and reads every row once, as a
# read after the fold does and as many rows as heap's twin holds: a scan forward
# and backward over keyless copies, a tid range read forward and backward, a
# parallel scan's workers, and a scan of a table whose key a collation compares.
$node->safe_psql(
	'postgres', q{
CREATE TABLE kinds_source (v text) USING doltgres_twin;
INSERT INTO kinds_source SELECT repeat('r', 400) || (i % 500) FROM generate_series(1, 3000) i;
CREATE TABLE kinds (v text);
CREATE TABLE heap_kinds (v text) USING doltgres_twin;
CREATE TABLE compared (k text COLLATE "und-x-icu" PRIMARY KEY, v text);
CREATE TABLE heap_compared (k text COLLATE "und-x-icu" PRIMARY KEY, v text) USING doltgres_twin;
});

# held_kind starts a read in a session of its own, held at the point where it
# reads its storage's spill slots, and returns the session.
sub held_kind
{
	my ($query) = @_;
	my $read = $node->background_psql('postgres');
	$read->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
	);
	$read->query_until(qr/held/, "\\echo held\n$query;\n");
	$node->wait_for_event('client backend', 'doltgres-spill-slots-read');
	return $read;
}

# across_fold writes a batch of a table's spilled rows, holds a read of them,
# folds the table, lets the read go on, and returns its answer with the same
# read's answer after the fold, the twin's count, and whether the fold
# installed a base.
sub across_fold
{
	my ($table, $batch, $query, $twin) = @_;
	launcher_held();
	$node->safe_psql('postgres', $batch);
	my $read = held_kind($query);
	my $folded =
	  $node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	launcher_freed();
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-spill-slots-read'); SELECT injection_points_wakeup('doltgres-spill-slots-read')"
	);
	my $held = $read->query_safe('SELECT 1');
	$read->quit;
	$held =~ s/\n1$//;
	return (
		$held,
		$node->safe_psql('postgres', $query),
		$node->safe_psql('postgres', $twin), $folded);
}

my $insert =
  q{SELECT count(*) FROM doltgres_test_insert('kinds', 'kinds_source', true, 0);
	INSERT INTO heap_kinds SELECT * FROM kinds_source};
my $twin = q{SELECT count(*) FROM heap_kinds};
my %kind = (
	forward =>
	  q{SELECT count(*) || ' ' || md5(string_agg(substring(r from '^\S+ \S+ (.*)$'), ',' ORDER BY r)) FROM doltgres_test_scan('kinds', false, -1) r},
	backward =>
	  q{SELECT count(*) || ' ' || md5(string_agg(substring(r from '^\S+ \S+ (.*)$'), ',' ORDER BY r)) FROM doltgres_test_scan('kinds', true, -1) r},
	'tid range forward' =>
	  q{SELECT count(*) || ' ' || md5(string_agg(substring(r from '^\S+ \S+ (.*)$'), ',' ORDER BY r)) FROM doltgres_test_tidrange('kinds', '(0,1)', '(4294967294,0)', false) r},
	'tid range backward' =>
	  q{SELECT count(*) || ' ' || md5(string_agg(substring(r from '^\S+ \S+ (.*)$'), ',' ORDER BY r)) FROM doltgres_test_tidrange('kinds', '(0,1)', '(4294967294,0)', true) r},
	parallel =>
	  q{SELECT count(*) || ' ' || md5(string_agg(substring(r from '^\S+ \S+ (.*)$'), ',' ORDER BY r)) FROM doltgres_test_parallel('kinds', 3, false, 0, NULL, NULL) r},
);
for my $name (
	'forward', 'backward',
	'tid range forward', 'tid range backward',
	'parallel')
{
	my ($held, $after, $count, $folded) =
	  across_fold('kinds', $insert, $kind{$name}, $twin);
	is($folded, 't', "a fold under the held $name read installs a base");
	is($held, $after, "a $name read held across a fold reads every row once");
	like($held, qr/^$count /, "and as many rows as heap's twin holds");
}
my @compared = across_fold(
	'compared',
	q{SELECT doltgres_test_write('compared', true, 'INSERT INTO compared SELECT ''k'' || i, repeat(''v'', 400) FROM generate_series(1, 3000) i');
		INSERT INTO heap_compared SELECT 'k' || i, repeat('v', 400) FROM generate_series(1, 3000) i},
	q{SELECT count(*) || ' ' || md5(string_agg(substring(r from '^\S+ \S+ (.*)$'), ',' ORDER BY r)) FROM doltgres_test_scan('compared', false, -1) r},
	q{SELECT count(*) FROM heap_compared});
is($compared[3], 't',
	"a fold under the held read of a table whose key a collation compares installs a base"
);
is($compared[0], $compared[1],
	"a read of a table whose key a collation compares, held across a fold, reads every row once"
);
like(
	$compared[0],
	qr/^$compared[2] /,
	"and as many rows as heap's twin holds");

# Bugs rows 240 and 241's position kinds through a fork btree index: an index
# scan forward and backward, an index-only scan, and a bitmap scan, each held
# after it read a storage's spill slots while a fold of the index installs a
# base that applies a committed transaction's spilled entries, read every row
# once, as heap's twin gives them, with many rows of one key spanning the fold.
$node->safe_psql(
	'postgres', q{
CREATE TABLE ix (id int4, t text);
CREATE INDEX ix_t ON ix (t);
CREATE TABLE heap_ix (id int4, t text) USING doltgres_twin;
CREATE INDEX heap_ix_t ON heap_ix (t);
CREATE TABLE ix_source USING doltgres_twin AS SELECT i AS id, repeat('v', 200) || (i % 41) AS t FROM generate_series(1, 2000) i;
INSERT INTO heap_ix SELECT * FROM ix_source;
SELECT doltgres_test_insert('ix', 'ix_source', false, 0);
SELECT doltgres_test_fold('ix');
SELECT doltgres_test_index_build('ix', 'ix_t');
ANALYZE heap_ix;
});
my $routine =
  "DO \$\$ BEGIN PERFORM doltgres_test_index_routine('ix_t'); END \$\$;";
my $indexes =
  'SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off;';
my %plans = (
	'index forward' =>
	  [ $indexes, q{SELECT id FROM %T WHERE t >= 'v' ORDER BY t} ],
	'index backward' =>
	  [ $indexes, q{SELECT id FROM %T WHERE t >= 'v' ORDER BY t DESC} ],
	'index-only' => [
		'SET enable_seqscan = off; SET enable_bitmapscan = off;',
		q{SELECT t FROM %T WHERE t >= 'v' ORDER BY t}
	],
	bitmap => [
		'SET enable_seqscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = off;',
		q{SELECT id FROM %T WHERE t >= 'v'}
	],);
my $next = 2001;
for my $name ('index forward', 'index backward', 'index-only', 'bitmap')
{
	my ($settings, $query) = @{ $plans{$name} };
	my ($from, $to) = ($next, $next + 3999);
	$next = $to + 1;
	launcher_held();
	$node->safe_psql(
		'postgres',
		"$routine SELECT doltgres_test_write('ix', true, 'INSERT INTO ix SELECT i, repeat(''v'', 200) || (i % 41) FROM generate_series($from, $to) i');
		INSERT INTO heap_ix SELECT i, repeat('v', 200) || (i % 41) FROM generate_series($from, $to) i"
	);
	(my $fork = $query) =~ s/%T/ix/g;
	$fork =~ s/'/''/g;
	(my $heap = $query) =~ s/%T/heap_ix/g;
	my $read = $node->background_psql('postgres');
	$read->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
	);
	$read->query_until(qr/held/,
		"\\echo held\n$settings $routine SELECT doltgres_test_standing('ix', '$fork');\n"
	);
	$node->wait_for_event('client backend', 'doltgres-spill-slots-read');
	my $entries =
	  $node->safe_psql('postgres', "SELECT doltgres_test_index_base('ix_t')");
	isnt(
		$node->safe_psql(
			'postgres', "SELECT doltgres_test_index_fold('ix_t')"),
		$entries,
		"a fold of the index under the held $name read installs a base");
	is($node->safe_psql('postgres', "SELECT doltgres_test_fold('ix')"),
		't', "a fold of the table under the held $name read installs a base");
	launcher_freed();
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-spill-slots-read'); SELECT injection_points_wakeup('doltgres-spill-slots-read')"
	);
	my $held = $read->query_safe('SELECT 1');
	$read->quit;
	$held =~ s/\n1$//;
	my $twin = $node->safe_psql('postgres', "$settings $heap");
	is( join(',', sort split /\n/, $held),
		join(',', sort split /\n/, $twin),
		"a fork btree's $name read held across a fold of its index reads every row once"
	);
}

# Bugs rows 240 and 315: a change by tuple id, held where it looks up its
# row's key, before a fold installs a base holding the row and drops the
# row's versions from the delta, finds the row's key and changes it, as the
# same change does with no fold: an UPDATE after an INSERT in its own
# transaction, a DELETE, and a row lock.
my %changes = (
	update => [
		q{BEGIN; INSERT INTO %T SELECT g, 'y' FROM generate_series(2001, 5000) g},
		q{WITH c AS (UPDATE %T SET v = 'z' WHERE id <= 1000 RETURNING 1) SELECT count(*) FROM c},
		'COMMIT'
	],
	delete => [
		'BEGIN',
		'WITH c AS (DELETE FROM %T WHERE id <= 1000 RETURNING 1) SELECT count(*) FROM c',
		'COMMIT'
	],
	lock => [
		'BEGIN',
		'SELECT count(*) FROM (SELECT id FROM %T WHERE id <= 1000 FOR UPDATE) l',
		'COMMIT'
	],);
my %change_named = (
	update => 'an UPDATE',
	delete => 'a DELETE',
	lock => 'a row lock');
for my $name ('update', 'delete', 'lock')
{
	my ($before, $change, $after) = @{ $changes{$name} };
	my $named = $change_named{$name};
	my $table = "changed_$name";
	s/%T/$table/g for ($before, $change, $after);
	launcher_held();
	$node->safe_psql('postgres',
		"CREATE TABLE $table (id int4 PRIMARY KEY, v text); INSERT INTO $table SELECT g, repeat('x', 400) FROM generate_series(1, 2000) g"
	);
	my $changer = $node->background_psql('postgres', on_error_stop => 0);
	$changer->query_safe($before);
	$changer->query_safe(
		"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-tuple-keyed', 'wait')"
	);
	$changer->query_until(qr/held/, "\\echo held\n$change;\n");
	$node->wait_for_event('client backend', 'doltgres-tuple-keyed');
	is($node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')"),
		't', "a fold under $named by tuple id, held, installs a base");
	launcher_freed();
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-tuple-keyed'); SELECT injection_points_wakeup('doltgres-tuple-keyed')"
	);
	my $changed = $changer->query('SELECT 1');
	$changed =~ s/\n1$//;
	$changed .= " $changer->{stderr}" if $changer->{stderr} =~ /ERROR/;
	$changer->query($after);
	$changer->quit;
	is($changed, '1000',
		"$named by tuple id held across a fold that applied its rows changes every row"
	);
}
$node->stop;

done_testing();
