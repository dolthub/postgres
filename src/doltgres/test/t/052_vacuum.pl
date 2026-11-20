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


# VACUUM's freeing of tuple ids before the fold (PLAN item 2's Freeing; bugs
# rows 44 and 73): the fork's relation_vacuum frees the tuple ids a take of the
# free space map took that no row holds once their takers have ended, after
# heap's index pass over the table's indexes, so an insert after a VACUUM takes
# them again where heap's insert reuses its line pointers. Each case runs the
# same steps over a fork table, through the test module's routine, and over a
# heap table of the same definition, as 046 does, so heap's answers are the
# expectations, and states the ctid heap gives on the reference.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('vacuum');
$node->init(allows_streaming => 1);

# Every case runs its own VACUUM, and an automatic one running beside a fold
# would keep the fold below the rows its snapshot may still read.
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test; CALL doltgres_test_twins(); CREATE EXTENSION pageinspect'
);

# A standby made now follows every case's records.
$node->backup('vacuum_backup');
my $standby = PostgreSQL::Test::Cluster->new('vacuum_standby');
$standby->init_from_backup($node, 'vacuum_backup', has_streaming => 1);
$standby->start;

# made makes a fork table and a heap table of a definition, each with the
# statements given over "%", run through the fork's routine for the fork table.
sub made
{
	my ($name, $definition, @statements) = @_;
	for my $table ($name, "heap_$name")
	{
		$node->safe_psql('postgres', "CREATE TABLE $table $definition");
		$node->safe_psql('postgres', written($table, $_)) for @statements;
	}
}

# written returns the SQL that runs a statement over a table, with "%" in the
# statement standing for the table's name: an insert, an update, a delete, or a
# select through the fork's routine for a fork table and through heap's for a
# heap table; "vacuum" and "vacuum <cleanup>" as VACUUM, with INDEX_CLEANUP auto
# unless named, through the fork's relation_vacuum for a fork table; any other
# statement as it is; and statements apart by "; " each so, in one script.
sub written
{
	my ($table, $statement) = @_;
	return join('; ', map { written($table, $_) } split /; /, $statement)
	  if $statement =~ /; /;
	my $fork = $table =~ /^heap_/ ? 'false' : 'true';
	if ($statement =~ /^vacuum ?(\w*)$/)
	{
		my $cleanup = $1 || 'auto';
		return $fork eq 'true'
		  ? "SELECT doltgres_test_vacuum('$table', '$cleanup')"
		  : "VACUUM (INDEX_CLEANUP $cleanup) $table";
	}
	(my $sql = $statement) =~ s/%/$table/g;
	return $sql if $statement !~ /^(INSERT|UPDATE|DELETE|SELECT) /;
	$sql =~ s/'/''/g;
	return "SELECT doltgres_test_write('$table', $fork, '$sql')";
}

# reading returns the SQL that reads a table's rows as text in their order, with
# their ctids, or their ctids alone when bare, through the fork's routine for a
# fork table.
sub reading
{
	my ($table, $bare) = @_;
	my $query =
	  $bare
	  ? "SELECT string_agg(ctid::text, ' ' ORDER BY ctid) FROM $table"
	  : "SELECT ctid::text || ' ' || ${table}::text FROM $table ORDER BY ctid";
	return $table =~ /^heap_/
	  ? $query
	  : "SELECT doltgres_test_standing('$table', '"
	  . ($query =~ s/'/''/gr) . "')";
}

# hashed returns the SQL that lists the ctids a hash index's bucket and
# overflow pages hold, in ctid order.
sub hashed
{
	my ($index) = @_;
	return
	  "SELECT string_agg(ctid::text, ' ' ORDER BY ctid) FROM generate_series(1, "
	  . "pg_relation_size('$index') / current_setting('block_size')::int - 1) AS page, "
	  . "LATERAL hash_page_items(get_raw_page('$index', page::int)) "
	  . "WHERE hash_page_type(get_raw_page('$index', page::int)) IN ('bucket', 'overflow')";
}

# latest returns the SQL that gives the newest tid of a row a tid names, by the
# fork's tuple_get_latest_tid for a fork table and by heap's currtid2 for a
# heap table.
sub latest
{
	my ($table, $tid) = @_;
	return $table =~ /^heap_/
	  ? "SELECT currtid2('$table', '$tid')"
	  : "SELECT doltgres_test_latest('$table', '$tid')";
}

# step runs a statement and returns its output, or the first line of the error
# it raised.
sub step
{
	my ($sql) = @_;
	my ($code, $output, $error) = $node->psql('postgres', $sql);
	my ($first) = $error =~ /(ERROR:[^\n]*)/;
	return $first // $output;
}

# both runs steps over a fork table and over its heap table, each a statement
# over "%", a read of "%" for "read", of its ctids alone for "ctids", of a
# tid's newest for "latest <tid>", or a read of an index's ctids for
# "hashed <index>", "%" again standing for the table's name, checks that each
# gives what heap's gives, and returns the fork's outputs.
sub both
{
	my ($name, $case, @steps) = @_;
	my %outputs;
	for my $table ($name, "heap_$name")
	{
		for my $statement (@steps)
		{
			my $sql =
				$statement eq 'read'          ? reading($table)
			  : $statement eq 'ctids'         ? reading($table, 1)
			  : $statement =~ /^latest (.*)$/ ? latest($table, $1)
			  : $statement =~ /^hashed (.*)$/ ? hashed($1 =~ s/%/$table/gr)
			  :                                 written($table, $statement);
			push @{ $outputs{$table} }, step($sql) =~ s/heap_//gr;
		}
	}
	is_deeply($outputs{$name}, $outputs{"heap_$name"}, $case);
	return $outputs{$name};
}

# Bugs row 44's first case: on the reference, a heap table with no index frees
# an aborted insert's line pointer at VACUUM, and the next insert takes (0,3).
made('aborted', '(k int4, v text)');
my $outputs = both(
	'aborted',
	"an aborted insert's tuple id is handed out again after a VACUUM, as heap's",
	"INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
	"BEGIN; INSERT INTO % VALUES (3, 'v3'); ROLLBACK",
	'vacuum',
	"INSERT INTO % VALUES (4, 'v4')",
	'read');
is( $outputs->[-1],
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,3) (4,v4)",
	"the aborted insert's (0,3) is taken by the next insert");

# PRevPF 7's case: on the reference, VACUUM's index pass drops a hash index's
# entry for an aborted insert's line pointer before the line pointer is reused,
# so the index holds (0,1), (0,2), and the new row's (0,3) once each.
made('hashed', '(k int4, v text)', 'CREATE INDEX ON % USING hash (k)');
$outputs = both(
	'hashed',
	"a hash index's entry for an aborted insert's tuple id is gone before the tuple id is handed out again",
	"INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
	"BEGIN; INSERT INTO % VALUES (3, 'v3'); ROLLBACK",
	'hashed %_k_idx',
	'vacuum',
	'hashed %_k_idx',
	"INSERT INTO % VALUES (4, 'v4')",
	'hashed %_k_idx',
	'read');
is_deeply(
	[ @$outputs[ 2, 4, 6 ] ],
	[ '(0,1) (0,2) (0,3)', '(0,1) (0,2)', '(0,1) (0,2) (0,3)' ],
	"the hash index's entries before the VACUUM, after it, and after the next insert"
);

# On the reference, VACUUM with INDEX_CLEANUP off leaves a heap table's dead
# line pointer unused by the next insert when the table has an index, which
# takes (0,4).
made('kept', '(k int4, v text)', 'CREATE INDEX ON % USING hash (k)');
$outputs = both(
	'kept',
	"VACUUM with INDEX_CLEANUP off leaves an aborted insert's tuple id taken on a table with an index, as heap's",
	"INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
	"BEGIN; INSERT INTO % VALUES (3, 'v3'); ROLLBACK",
	'vacuum off',
	"INSERT INTO % VALUES (4, 'v4')",
	'read');
is( $outputs->[-1],
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,4) (4,v4)",
	'the next insert takes (0,4)');

# Bugs row 73: on the reference, a refused insert's dead tuple at (0,3) is
# freed by VACUUM, and the next insert takes (0,3).
made('refused', '(k int4 PRIMARY KEY, v text)');
$outputs = both(
	'refused',
	"a refused insert's spent tuple id is freed by a VACUUM and taken again, as heap's",
	"INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
	"INSERT INTO % VALUES (1, 'again')",
	'vacuum',
	"INSERT INTO % VALUES (3, 'v3')",
	'read');
is( $outputs->[-1],
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,3) (3,v3)",
	"the refused insert's (0,3) is taken by the next insert");

# D4's window case: a take still in its insert's window, paused after its row
# change record and before its version, is left taken by a VACUUM that runs
# meanwhile, so no other insert is handed its tuple id.
made('windowed', '(k int4, v text)', "INSERT INTO % VALUES (1, 'v1')");
my $writer = $node->background_psql('postgres');
$writer->query_safe('SELECT injection_points_set_local()');
$writer->query_safe(
	"SELECT injection_points_attach('doltgres-insert-logged', 'wait')");
$writer->query_until(qr/start/,
		"\\echo start\n"
	  . written('windowed', "INSERT INTO % VALUES (2, 'v2')")
	  . ";\n");
$node->wait_for_event('client backend', 'doltgres-insert-logged');
$node->safe_psql('postgres', written('windowed', 'vacuum'));
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-insert-logged')");
$writer->query_safe(
	"SELECT injection_points_detach('doltgres-insert-logged')");
$writer->quit;
$node->safe_psql('postgres',
	written('windowed', "INSERT INTO % VALUES (3, 'v3')"));
is( $node->safe_psql('postgres', reading('windowed')),
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,3) (3,v3)",
	"a take in its window is left taken by a concurrent VACUUM, and no tuple id is handed out twice"
);

# D4's standby case: the free record replayed on a hot standby leaves the
# freed block's free space entry as the primary's, its bytes and its offsets.
made(
	'followed',
	'(k int4, v text)',
	"INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
	"BEGIN; INSERT INTO % VALUES (3, 'v3'); ROLLBACK", 'vacuum');
$node->wait_for_replay_catchup($standby);
my $primary = $node->safe_psql('postgres',
	"SELECT doltgres_test_free_space('followed', 0)");
like(
	$primary,
	qr/^\d+ \{3\}$/,
	"the primary's entry holds the freed offset 3");
is( $standby->safe_psql(
		'postgres', "SELECT doltgres_test_free_space('followed', 0)"),
	$primary,
	"a standby's free space entry for a freed block equals the primary's, bytes and offsets, after VACUUM"
);

# D4's crash case: on the reference, a crash after VACUUM's free and before the
# next checkpoint leaves the freed line pointer free after recovery, and the
# next insert takes it, (0,3), with no tuple id handed out twice. VACUUM's
# commit has no xid and flushes nothing, so a commit that writes WAL flushes its
# records before the crash.
$node->safe_psql('postgres', 'CHECKPOINT');
made(
	'crashed',
	'(k int4, v text)',
	"INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
	"BEGIN; INSERT INTO % VALUES (3, 'v3'); ROLLBACK", 'vacuum');
$node->safe_psql('postgres', 'CREATE TABLE crash_flush ()');
$node->stop('immediate');
$node->start;
$outputs = both(
	'crashed',
	'after a crash between a free and the next checkpoint the freed tuple id is free and none is handed out twice',
	"INSERT INTO % VALUES (4, 'v4'), (5, 'v5')",
	'read');
is( $outputs->[-1],
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,3) (4,v4)\n(0,4) (5,v5)",
	'the next inserts take (0,3) and (0,4)');

# D5: on the reference, VACUUM with INDEX_CLEANUP off on a table with an index
# reclaims an aborted insert's space and leaves its line pointer dead, so the
# next 1 kB row fits page 0 at a new pointer, (0,8), and a hash index still
# holds the dead pointer's entry; a later VACUUM with its index pass drops the
# entry and frees the pointer, and the next small row takes (0,7).
made('dead', '(k int4, v text)', 'CREATE INDEX ON % USING hash (k)');
$outputs = both(
	'dead',
	"VACUUM with INDEX_CLEANUP off gives back an aborted insert's space and keeps its tuple id taken, as heap's LP_DEAD",
	"INSERT INTO % SELECT k, repeat('x', 1000) FROM generate_series(1, 6) k",
	"BEGIN; INSERT INTO % VALUES (7, repeat('x', 1000)); ROLLBACK",
	'vacuum off',
	"INSERT INTO % VALUES (8, repeat('x', 1000))",
	'ctids',
	'hashed %_k_idx',
	'vacuum on',
	'hashed %_k_idx',
	"INSERT INTO % VALUES (9, 'v9')",
	'ctids');
is_deeply(
	[ @$outputs[ 4, 5, 7, 9 ] ],
	[
		'(0,1) (0,2) (0,3) (0,4) (0,5) (0,6) (0,8)',
		'(0,1) (0,2) (0,3) (0,4) (0,5) (0,6) (0,7) (0,8)',
		'(0,1) (0,2) (0,3) (0,4) (0,5) (0,6) (0,8)',
		'(0,1) (0,2) (0,3) (0,4) (0,5) (0,6) (0,7) (0,8)'
	],
	'the 1 kB row takes (0,8) in the reclaimed space, and after the index pass the small row takes (0,7)'
);
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql(
		'postgres', "SELECT doltgres_test_free_space('dead', 0)"),
	$node->safe_psql(
		'postgres', "SELECT doltgres_test_free_space('dead', 0)"),
	"a standby's free space entry after a dead tuple id's give-back and its later free equals the primary's"
);

# On the reference, VACUUM in INDEX_CLEANUP auto skips its index pass only when
# dead line pointers are on fewer pages than 2% of the table's, truncated to
# whole pages: on 58 pages that is 1, so one dead page doesn't skip it, the
# aborted insert's pointer is freed, and the next row takes (57,2).
made(
	'unbypassed',
	'(k int4, v text)',
	'CREATE INDEX ON % USING hash (k)',
	"INSERT INTO % SELECT k, repeat('x', 1000) FROM generate_series(1, 400) k"
);
$outputs = both(
	'unbypassed',
	"VACUUM's index pass runs for one dead page of 58, as heap's, so an aborted insert's tuple id is freed",
	"BEGIN; INSERT INTO % VALUES (401, repeat('x', 1000)); ROLLBACK",
	'vacuum',
	"INSERT INTO % VALUES (402, repeat('x', 1000))",
	'ctids');
like($outputs->[-1], qr/ \(57,1\) \(57,2\)$/, 'the next row takes (57,2)');

# D5: on 115 pages the threshold is 2, so one dead page makes VACUUM skip its
# index pass, leaving the aborted insert's pointer dead with its space
# reclaimed, and the next row takes (114,4), not (114,3).
made(
	'bypassed',
	'(k int4, v text)',
	'CREATE INDEX ON % USING hash (k)',
	"INSERT INTO % SELECT k, repeat('x', 1000) FROM generate_series(1, 800) k"
);
$outputs = both(
	'bypassed',
	"VACUUM's skip of its index pass for one dead page of 115 keeps an aborted insert's tuple id taken, as heap's",
	"BEGIN; INSERT INTO % VALUES (801, repeat('x', 1000)); ROLLBACK",
	'vacuum',
	"INSERT INTO % VALUES (802, repeat('x', 1000))",
	'ctids');
like($outputs->[-1], qr/ \(114,2\) \(114,4\)$/, 'the next row takes (114,4)');

# D4's follow case (CommPF 7): on the reference, an aborted update's new tuple at
# (0,2) is freed by VACUUM and taken by the next insert, and currtid2 of the
# updated row's (0,1) still gives (0,1), since the follow checks the new
# tuple's writer against the old one's ender.
made('latest', '(k int4 PRIMARY KEY, v text)');
$outputs = both(
	'latest',
	"a get_latest_tid on a row whose aborted update's tuple id was freed and reused gives what heap gives",
	"INSERT INTO % VALUES (1, 'v1')",
	"BEGIN; UPDATE % SET v = 'w' WHERE k = 1; ROLLBACK",
	'vacuum',
	"INSERT INTO % VALUES (2, 'v2')",
	'ctids',
	'latest (0,1)');
is_deeply(
	[ @$outputs[ 4, 5 ] ],
	[ '(0,1) (0,2)', '(0,1)' ],
	"the new row takes (0,2), and (0,1)'s newest is itself");

# CommPF 7's follow case: a key share lock taken while a keyless table's update
# runs follows the update to the row it made, as heap_lock_updated_tuple does;
# held at the follow's injection point after it read the update's chain, the
# lock outlasts the update's abort, a VACUUM that frees the update's tuple id,
# and an insert that takes it, then finds that tuple id's row written by another
# transaction and locks nothing there, as heap's check of the newer version's
# xmin against the older one's xmax refuses it, so a delete of the inserted row,
# which a key share lock would hold back, goes through. Heap's lock, with no such point, follows the running update
# at once; each ends as the other does.
my %followed;
for my $table ('following', 'heap_following')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (1, 'v1'), (2, 'v2')"));
	my $fork = $table !~ /^heap_/;
	my $updater = $node->background_psql('postgres');
	$updater->query_safe('BEGIN');
	$updater->query_safe(written($table, "UPDATE % SET v = 'x' WHERE k = 1"));
	my $locker = $node->background_psql('postgres');
	$locker->query_safe('SELECT injection_points_set_local()');
	$locker->query_safe(
		"SELECT injection_points_attach('doltgres-lock-following', 'wait')")
	  if $fork;
	$locker->query_safe('BEGIN');
	$locker->query_until(qr/start/,
			"\\echo start\n"
		  . written($table, 'SELECT k FROM % WHERE k = 1 FOR KEY SHARE')
		  . ";\n");
	$node->wait_for_event('client backend', 'doltgres-lock-following')
	  if $fork;
	$updater->query_safe('ROLLBACK');
	$updater->quit;
	$node->safe_psql('postgres', written($table, 'vacuum'));
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (3, 'v3')"));

	if ($fork)
	{
		$node->safe_psql('postgres',
			"SELECT injection_points_wakeup('doltgres-lock-following')");
		$locker->query_safe(
			"SELECT injection_points_detach('doltgres-lock-following')");
	}
	push @{ $followed{$table} }, step(reading($table)) =~ s/heap_//gr;
	push @{ $followed{$table} },
	  step('SET lock_timeout = 200; '
		  . written($table, 'DELETE FROM % WHERE k = 3'));
	$locker->query_safe('COMMIT');
	$locker->quit;
	push @{ $followed{$table} }, step(reading($table)) =~ s/heap_//gr;
}
is_deeply($followed{following}, $followed{heap_following},
	"a lock following a running update finds its freed and reused tuple id's row another's, as heap's xmin check does"
);
is_deeply(
	$followed{following},
	[
		"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,3) (3,v3)",
		'1',
		"(0,1) (1,v1)\n(0,2) (2,v2)"
	],
	"the inserted row took the update's (0,3), and its delete wasn't held back by the key share lock"
);

# Bugs row 75: on the reference, an update whose new row doesn't fit its page
# goes elsewhere and marks the page full, so the next update of a row there
# prunes the page first (heap_page_prune_opt), reclaiming the moved row's dead
# version, and its new version stays on the page at (0,8). The fork prunes at
# the take of the block, from its free space entry's full flag (R1, R2).
my $rows =
  "INSERT INTO % SELECT k, repeat('x', 1000) FROM generate_series(1, 14) k";
made('pruned', '(k int4 PRIMARY KEY, v text)', $rows);
$outputs = both(
	'pruned',
	"an update after another moved a row off a full page prunes the page and stays on it, as heap's on-access prune does",
	"UPDATE % SET v = repeat('y', 1000) WHERE k = 1",
	'ctids',
	"UPDATE % SET v = repeat('z', 1000) WHERE k = 3",
	'ctids');
like(
	$outputs->[-1],
	qr/^\(0,2\) \(0,4\) .*\(0,8\) /,
	"the second update's new row takes (0,8)");

# Bugs row 75's held-back case: on the reference, a snapshot taken before the
# first update keeps its row's old version, so the page isn't freed and the
# second update's new row goes to (2,2).
made('held', '(k int4 PRIMARY KEY, v text)', $rows);
my $holder = $node->background_psql('postgres');
$holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$holder->query_safe('SELECT 1');
$outputs = both(
	'held',
	"a snapshot that may see an updated row's old version keeps it from a prune, as heap's",
	"UPDATE % SET v = repeat('y', 1000) WHERE k = 1",
	"UPDATE % SET v = repeat('z', 1000) WHERE k = 3",
	'ctids');
$holder->query_safe('COMMIT');
$holder->quit;
like(
	$outputs->[-1],
	qr/\(2,1\) \(2,2\)$/,
	"the second update's new row takes (2,2)");

# Differences row 43: on the reference, a row updated on its own page is a HOT
# chain whose root line pointer, once the next update's read prunes the page,
# stays as LP_REDIRECT for the chain's life, so after VACUUM the next insert
# takes a new pointer, (0,10); the fork has no such chain, its next update's
# prune and its VACUUM free the old version's tuple id, and the next insert
# takes it, (0,1).
made(
	'rooted',
	'(k int4, v text)',
	"INSERT INTO % SELECT k, repeat('x', 1000) FROM generate_series(1, 7) k");
$outputs = both(
	'rooted',
	"an update on its own page and an update that prunes it give heap's ctids before VACUUM",
	"UPDATE % SET v = repeat('s', 100) WHERE k = 1",
	"UPDATE % SET v = repeat('s', 100) WHERE k = 2",
	'ctids',
	'vacuum');
my %rooted;
for my $table ('rooted', 'heap_rooted')
{
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (9, 'b')"));
	my $query = "SELECT ctid::text FROM $table WHERE k = 9";
	$rooted{$table} = $node->safe_psql('postgres',
		  $table =~ /^heap_/
		? $query
		: "SELECT doltgres_test_standing('$table', '"
		  . ($query =~ s/'/''/gr)
		  . "')");
}
is_deeply(
	[ $rooted{rooted}, $rooted{heap_rooted} ],
	[ '(0,1)', '(0,10)' ],
	"after VACUUM the fork hands out the HOT root's (0,1) where heap's insert takes (0,10) (Differences row 43)"
);

# Differences row 45 and bugs row 75's correction: on the reference, an insert
# never prunes, so after a delete on a page past heap_page_prune_opt's
# condition the next 1076-byte row goes to (1,1) on both.
my $wide =
  "INSERT INTO % SELECT k, repeat('x', 1040) FROM generate_series(1, 7) k";
made('unread', '(k int4, v text)', $wide);
$outputs = both(
	'unread',
	"an insert after a delete doesn't prune, as heap's doesn't",
	'DELETE FROM % WHERE k = 3',
	"INSERT INTO % VALUES (8, repeat('x', 1040))",
	'ctids');
like($outputs->[-1], qr/ \(1,1\)$/, 'the new row takes (1,1)');

# Differences row 45: a read between the delete and the insert prunes heap's page,
# so heap's row takes (0,8), while the fork prunes only at an update's take and
# its row takes (1,1).
my %read;
for my $table ('reread', 'heap_reread')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres', written($table, $_))
	  for ($wide, 'DELETE FROM % WHERE k = 3');
	step(reading($table, 1));
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (8, repeat('x', 1040))"));
	$read{$table} = step(reading($table, 1));
}
is_deeply(
	[ map { $_ =~ /(\S+)$/ } @read{ 'reread', 'heap_reread' } ],
	[ '(1,1)', '(0,8)' ],
	"a read between a delete and an insert prunes heap's page, not the fork's (Differences row 45)"
);

# PRevPF 7's R2: a standby and a recovery replay a prune's removal of the dead
# version with its give-back, so after the prune, a VACUUM, and an insert, each
# holds the primary's free space entry and ctids.
made(
	'replayed',
	'(k int4 PRIMARY KEY, v text)',
	$rows,
	"UPDATE % SET v = repeat('y', 1000) WHERE k = 1",
	"UPDATE % SET v = repeat('z', 1000) WHERE k = 3",
	'vacuum',
	"INSERT INTO % VALUES (15, 'v15')");
$node->wait_for_replay_catchup($standby);
my @replayed = map { $node->safe_psql('postgres', $_) }
  ("SELECT doltgres_test_free_space('replayed', 0)", reading('replayed', 1));
is_deeply(
	[
		map { $standby->safe_psql('postgres', $_) } (
			"SELECT doltgres_test_free_space('replayed', 0)",
			reading('replayed', 1))
	],
	\@replayed,
	"a standby's entry and ctids after a prune, a VACUUM, and an insert equal the primary's"
);
$node->safe_psql('postgres', 'CREATE TABLE replay_flush ()');
$node->stop('immediate');
$node->start;
is_deeply(
	[
		map { $node->safe_psql('postgres', $_) } (
			"SELECT doltgres_test_free_space('replayed', 0)",
			reading('replayed', 1))
	],
	\@replayed,
	"after a crash the entry and ctids after a prune, a VACUUM, and an insert are the primary's before it"
);

# PRevPF 7's R1: a prune held between its marker and its removal while a VACUUM
# runs and another update takes the block: the VACUUM leaves the tuple id, still
# mapped, the bytes come back once, so the held update's new row no longer fits
# block 0 and goes to (2,2), and a later VACUUM frees the offset alone. An ANALYZE
# beside the held prune doesn't wait.
made('claimed', '(k int4 PRIMARY KEY, v text)',
	$rows, "UPDATE % SET v = repeat('y', 1000) WHERE k = 1");
my $pruner = $node->background_psql('postgres');
$pruner->query_safe('SELECT injection_points_set_local()');
$pruner->query_safe(
	"SELECT injection_points_attach('doltgres-prune-marked', 'wait')");
$pruner->query_until(qr/start/,
		"\\echo start\n"
	  . written('claimed', "UPDATE % SET v = repeat('z', 1000) WHERE k = 3")
	  . ";\n");
$node->wait_for_event('client backend', 'doltgres-prune-marked');
$node->safe_psql('postgres', written('claimed', 'vacuum'));
unlike(
	$node->safe_psql(
		'postgres', "SELECT doltgres_test_free_space('claimed', 0)"),
	qr/\{1,/,
	"a VACUUM during the prune leaves the claimed tuple id taken");
$node->safe_psql('postgres',
	written('claimed', "UPDATE % SET v = repeat('w', 1000) WHERE k = 4"));
is($node->safe_psql('postgres', 'SET lock_timeout = 200; ANALYZE claimed'),
	'', 'an ANALYZE beside a pruning update doesn\'t wait');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-prune-marked')");
$pruner->query_safe(
	"SELECT injection_points_detach('doltgres-prune-marked')");
$pruner->quit;
is( step(
		"SELECT doltgres_test_standing('claimed', 'SELECT ctid::text FROM claimed WHERE k IN (3, 4) ORDER BY k')"
	),
	"(2,2)\n(0,8)",
	"the bytes came back once: the other update took (0,8) and the held one's row went to (2,2)"
);
$node->safe_psql('postgres', written('claimed', 'vacuum'));
like(
	$node->safe_psql(
		'postgres', "SELECT doltgres_test_free_space('claimed', 0)"),
	qr/\{1,/,
	'a later VACUUM frees the offset alone');

# PRevPF 7's R1b: a VACUUM held after it read a block's takes and before it
# asks whether each tuple id is mapped, while an update prunes the block,
# finds the pruned version's tuple id unmapped and frees its offset alone, since
# it reads the takes again and meets the prune's marker: the bytes come back
# once, so block 0's entry keeps 916 free bytes after the update took 1036.
made('raced', '(k int4 PRIMARY KEY, v text)',
	$rows, "UPDATE % SET v = repeat('y', 1000) WHERE k = 1");
my $vacuumer = $node->background_psql('postgres');
$vacuumer->query_safe('SELECT injection_points_set_local()');
$vacuumer->query_safe(
	"SELECT injection_points_attach('doltgres-vacuum-found', 'wait')");
$vacuumer->query_until(qr/start/,
	"\\echo start\n" . written('raced', 'vacuum') . ";\n");
$node->wait_for_event('client backend', 'doltgres-vacuum-found');
$node->safe_psql('postgres',
	written('raced', "UPDATE % SET v = repeat('z', 1000) WHERE k = 3"));
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-vacuum-found')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-vacuum-found')");
$vacuumer->quit;
like(
	$node->safe_psql(
		'postgres', "SELECT doltgres_test_free_space('raced', 0)"),
	qr/^916 \{1,9,/,
	"a VACUUM that met a prune between its read of the takes and its mapped checks frees the offset alone"
);

# PRevPF 7's V3: on the reference, VACUUM with maintenance_work_mem at its least
# runs its index pass in cycles once its dead items pass that memory, and the
# fork runs as many cycles, freeing the same tuple ids.
made(
	'cycled',
	'(k int4, v text) WITH (fillfactor = 10)',
	'CREATE INDEX ON % USING hash (k)',
	"BEGIN; INSERT INTO % SELECT k, 'v' || k FROM generate_series(1, 20000) k; ROLLBACK"
);
my $scans = $node->psql(
	'postgres',
	'SET maintenance_work_mem = 64; VACUUM (VERBOSE, INDEX_CLEANUP on) heap_cycled',
	stderr => \my $verbose);
my ($heap_scans) = $verbose =~ /index scans: (\d+)/;
$node->psql(
	'postgres',
	"SET maintenance_work_mem = 64; SET client_min_messages = debug2; SELECT doltgres_test_vacuum('cycled', 'on')",
	stderr => \my $debug);
my $fork_scans = () = $debug =~ /scanned index "cycled_k_idx"/g;
ok($heap_scans >= 2, "heap's VACUUM ran its index pass in cycles");
is($fork_scans, $heap_scans,
	"VACUUM's index pass cycles at its memory bound, as heap's does, and its tuple ids are freed as heap's are"
);
# Bugs row 75 and field 30: a standby that replays an update that moved a row off
# its page holds the page's free space entry with its full flag set, as the
# primary does.
made('flagged', '(k int4 PRIMARY KEY, v text)',
	$rows, "UPDATE % SET v = repeat('y', 1000) WHERE k = 1");
$node->wait_for_replay_catchup($standby);
$primary = $node->safe_psql('postgres',
	"SELECT doltgres_test_free_space('flagged', 0)");
like($primary, qr/ full$/,
	"the primary's entry for the page an update moved off is full");
is( $standby->safe_psql(
		'postgres', "SELECT doltgres_test_free_space('flagged', 0)"),
	$primary,
	"a standby's entry for a page an update moved off has its full flag after replay"
);

# Differences row 44: a promoted standby's next update of a row on the page its
# primary marked full prunes it and takes (0,8), as the primary would have;
# heap's standby has PD_PAGE_FULL only when a full page image carried it, so
# heap's twin isn't compared.
made('promoted', '(k int4 PRIMARY KEY, v text)',
	$rows, "UPDATE % SET v = repeat('y', 1000) WHERE k = 1");
$node->wait_for_replay_catchup($standby);
$standby->promote;
$standby->safe_psql('postgres',
	written('promoted', "UPDATE % SET v = repeat('z', 1000) WHERE k = 3"));
like(
	$standby->safe_psql('postgres', reading('promoted', 1)),
	qr/^\(0,2\) \(0,4\) .*\(0,8\) /,
	"a promoted standby's next update prunes the page its primary marked full and takes (0,8) (Differences row 44)"
);

# Bugs row 77 and row 75's fill factor case: on the reference, a table whose
# fill factor keeps half of each page free keeps an update's new row on its
# page, as heap_update ignores the fill factor there, and prunes only on
# heap_page_prune_opt's condition, its free bytes below the larger of that
# reserve and a tenth of a page.
made(
	'sparse',
	'(k int4 PRIMARY KEY, v text) WITH (fillfactor = 50)',
	"INSERT INTO % SELECT k, repeat('x', 1000) FROM generate_series(1, 6) k");
both(
	'sparse',
	"a table at fill factor 50 keeps an update on its page and prunes on heap's condition, as heap's",
	"UPDATE % SET v = repeat('y', 1000) WHERE k = 1",
	'ctids',
	"UPDATE % SET v = repeat('z', 1000) WHERE k = 2",
	'ctids',
	"UPDATE % SET v = repeat('w', 1000) WHERE k = 3",
	'ctids');

# The fold's drop's D2 (CommPF 7): a standby made now, which cancels a query in
# conflict at once, follows the cases below.
$node->backup('ended_backup');
my $replica = PostgreSQL::Test::Cluster->new('ended_standby');
$replica->init_from_backup($node, 'ended_backup', has_streaming => 1);
$replica->append_conf('postgresql.conf', 'max_standby_streaming_delay = 0');
$replica->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');

# horizons returns the conflict horizons of the records a table's VACUUM logged
# from an LSN on that free its rows: heap's prune records and the fork's frees.
sub horizons
{
	my ($table, $from) = @_;
	my ($filter, $pattern) =
	  $table =~ /^heap_/
	  ? (
		"resource_manager = 'Heap2' AND record_type LIKE 'PRUNE%'",
		'snapshotConflictHorizon: (\d+)')
	  : ("record_type = 'TidsFreed'", 'horizon (\d+)');
	my $records = $node->safe_psql('postgres',
		"SELECT description FROM pg_get_wal_records_info('$from', pg_current_wal_flush_lsn()) "
		  . "WHERE $filter AND description || coalesce(block_ref, '') ~ ('/' || pg_relation_filenode('$table') || '\\M')"
	);
	return [ map { /$pattern/ ? $1 : $_ } split /\n/, $records ];
}

# folded_on waits until a standby's own fold of a fork table has installed the
# base its primary's fold made, so the free it replays next finds the fold's
# point reached and does its take-outs at once.
sub folded_on
{
	my ($standby, $table) = @_;
	my $sizes = $node->safe_psql('postgres',
		"SELECT doltgres_test_base_sizes('$table')");
	$node->wait_for_replay_catchup($standby);
	$standby->poll_query_until('postgres',
		"SELECT doltgres_test_base_sizes('$table')", $sizes)
	  or die "the standby's fold of $table installed no base";
}

# ended runs a D2 case over a table: rows inserted by one transaction, a
# snapshot held on the replica, a change by the statements given in another
# transaction, a fold of a fork table, which the replica's own fold reaches,
# and VACUUM. It returns the horizons VACUUM logged, each named "ender" when it
# is the change's xid and "inserter" when it is the insert's, and whether the
# replica kept or cancelled the held snapshot's session.
sub ended
{
	my ($table, $insert, @change) = @_;
	my $xid = sub {
		my ($sql) = @_;
		my @lines = split /\n/,
		  $node->safe_psql('postgres',
			"BEGIN; $sql; SELECT pg_current_xact_id(); COMMIT");
		return $lines[-1];
	};
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	my $inserter = $xid->(written($table, $insert));
	$node->wait_for_replay_catchup($replica);
	my $held = $replica->background_psql('postgres', on_error_stop => 0);
	$held->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
	my $ender = $xid->(join('; ', map { written($table, $_) } @change));
	if ($table !~ /^heap_/)
	{
		$node->poll_query_until('postgres',
			"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'fold worker'"
		) or die 'a fold worker still runs';
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
		folded_on($replica, $table);
	}
	my $from =
	  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$node->safe_psql('postgres', written($table, 'vacuum'));
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->wait_for_replay_catchup($replica);
	my $kept =
	  eval { $held->query_safe('SELECT 1'); 1 } ? 'kept' : 'cancelled';
	eval { $held->quit };
	my %named = ($ender => 'ender', $inserter => 'inserter');
	return [ (map { $named{$_} // $_ } @{ horizons($table, $from) }), $kept ];
}

# The fold launcher is held from here until the width case's fold below, and
# each fork case's fold waits until no fold worker runs, since a worker's
# snapshot taken before a change keeps the fold below it.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'"
) or die 'the fold launcher is not held';

# D2's standby case: on the reference, VACUUM removes a row another
# transaction deleted, or the old version of one it updated, with a prune
# record whose conflict horizon is that ender's xid, cancelling a standby
# session whose snapshot could still see the row. The fork's fold applies the
# change, its drop takes the row out of the delta with the ender's xid on its
# take, and VACUUM's free logs it, so the same session is cancelled; the
# replica's free space entry and rows then agree with the primary's, and its
# replay of the free has taken out the row version holding the freed (0,1),
# which the replica's own drop had kept for the cancelled snapshot, so no
# version maps a tuple id its free space map holds free (PRevPF 8's F1).
for my $case (
	[ 'ended', 'DELETE FROM % WHERE k = 1' ],
	[ 'moved', "UPDATE % SET v = 'z' WHERE k = 1" ])
{
	my ($name, $change) = @$case;
	my %outputs = map {
		$_ => ended($_, "INSERT INTO % VALUES (1, 'a'), (2, 'b')", $change)
	} ($name, "heap_$name");
	is_deeply(
		$outputs{"heap_$name"},
		[ 'ender', 'cancelled' ],
		"heap's prune of a row another transaction ended ($name) logs its ender and cancels the standby's snapshot"
	);
	is_deeply($outputs{$name}, $outputs{"heap_$name"},
		"the fork's free of a dropped row's tuple id ($name) logs its ender and cancels the standby's snapshot, as heap's"
	);
	is_deeply(
		[
			map { $replica->safe_psql('postgres', $_) } (
				"SELECT doltgres_test_free_space('$name', 0)",
				reading($name, 1))
		],
		[
			map { $node->safe_psql('postgres', $_) } (
				"SELECT doltgres_test_free_space('$name', 0)",
				reading($name, 1))
		],
		"the replica's free space entry and rows agree with the primary's after the free ($name)"
	);
}
is( $replica->safe_psql(
		'postgres', "SELECT doltgres_test_held('ended', '(0,1)')"),
	'f',
	"a standby's replay of a free takes out the row version its own drop kept for the freed tuple id"
);
is( $replica->safe_psql(
		'postgres', "SELECT doltgres_test_held('moved', '(0,1)')"),
	'f',
	"a standby's replay of a free takes out the old row version its own drop kept for an update's freed tuple id"
);

# D2's same-transaction case: on the reference, a row its own transaction
# inserted and deleted gives VACUUM's prune record no horizon
# (HeapTupleHeaderAdvanceConflictHorizon), and nothing is cancelled; heap's
# record still carries the kept row's inserter, 790 on the reference, from
# setting the page all-visible (vm_flags), which the fork, with no visibility
# map, has no part of, so its free logs 0 (Differences row 47).
my %own;
for my $table ('own', 'heap_own')
{
	$own{$table} = ended(
		$table,
		"INSERT INTO % VALUES (2, 'b')",
		"INSERT INTO % VALUES (1, 'a')",
		'DELETE FROM % WHERE k = 1');
}
is_deeply(
	$own{heap_own},
	[ 'inserter', 'kept' ],
	"heap's prune of a row its own transaction inserted and deleted logs only its visibility cutoff and cancels nothing (Differences row 47)"
);
is_deeply(
	$own{own},
	[ '0', 'kept' ],
	"the fork's free of a row its own transaction inserted and deleted logs no horizon and cancels nothing"
);
$replica->stop;

# D2's width (CommPF 8): a free's horizon is a full xid, which a standby's
# conflict resolution skips when it lies more than 2^31 before the next xid, as
# btree's page reuse does, since no snapshot can still need it. A row is
# deleted and folded, and the primary is moved to the next epoch with its next
# xid set back below the deleter's, which pg_resetwal does; VACUUM then logs the
# deleter's full xid from the epoch before, and a new standby's snapshot, whose
# xmin lies below the deleter's 32 bits, is kept, where a 32-bit horizon would
# read as newer than the xmin and cancel it.
$node->safe_psql('postgres', 'CREATE TABLE aged (k int4, v text)');
$node->safe_psql('postgres',
	written('aged', "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
$node->safe_psql('postgres',
	join('; ', ('SELECT pg_current_xact_id()') x 100));
my @aged = split /\n/,
  $node->safe_psql('postgres',
		'BEGIN; '
	  . written('aged', 'DELETE FROM % WHERE k = 1')
	  . '; SELECT pg_current_xact_id(); COMMIT');
my $aged_ender = $aged[-1];
$node->safe_psql('postgres', "SELECT doltgres_test_fold('aged')");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
);
$node->stop;
command_ok(
	[ 'pg_resetwal', '-e', '1', '-x', $aged_ender - 50, $node->data_dir ],
	'pg_resetwal moves the primary to the next epoch below the deleter');
$node->start;
$node->backup('aged_backup');
my $aged_standby = PostgreSQL::Test::Cluster->new('aged_standby');
$aged_standby->init_from_backup($node, 'aged_backup', has_streaming => 1);
$aged_standby->append_conf('postgresql.conf',
	'max_standby_streaming_delay = 0');
$aged_standby->start;
my $aged_held =
  $aged_standby->background_psql('postgres', on_error_stop => 0);
$aged_held->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
my $aged_xmin = $aged_held->query_safe(
	'SELECT backend_xmin FROM pg_stat_activity WHERE pid = pg_backend_pid()');
cmp_ok($aged_xmin, '<', $aged_ender,
	"the standby's snapshot's xmin lies below the deleter's 32 bits");
my $aged_from =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$node->safe_psql('postgres', written('aged', 'vacuum'));
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($aged_standby);
is_deeply(horizons('aged', $aged_from),
	[$aged_ender],
	"VACUUM's free logs the deleter's full xid from the epoch before");
ok( eval { $aged_held->query_safe('SELECT 1'); 1 },
	"a standby keeps a snapshot a horizon more than 2^31 before the next xid would conflict with in 32 bits"
);
$aged_held->quit;

# PLAN item 7's pruning when a fold changes no entry: an insert and its delete
# change no base entry, and a snapshot taken before them counts their writer as
# running, so design A's bound (the fold text's draft 12) holds the fold below
# their commit and it installs nothing, keeping both versions, where before A it
# installed them and kept them for the snapshot. Once the snapshot goes, the next
# fold installs and drops them. A rolled-back
# insert's row version goes at its abort's take-back, so a fold that applies
# nothing installs nothing; the drop of a version the take-back leaves, past its
# list's cap of 64K keys, is the fold's unit's
# (a_fold_that_changes_no_entry_still_drops_what_it_is_done_with).
$node->safe_psql('postgres', 'CREATE TABLE netted (k int4, v text)');
my $netted_holder = $node->background_psql('postgres');
$netted_holder->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql(
	'postgres',
	written(
		'netted', "INSERT INTO % VALUES (1, 'a'); DELETE FROM % WHERE k = 1")
);
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('netted')"),
	'f', 'a fold under a snapshot taken before an insert and its delete installs nothing');
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_held('netted', '(0,1)')"),
	't',
	'a snapshot taken before the insert keeps its row version');
$netted_holder->quit;
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('netted')"),
	't', 'the next fold installs once the snapshot goes');
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_held('netted', '(0,1)')"),
	'f',
	"the next fold drops the insert's row version");
$node->safe_psql('postgres',
	written('netted', "BEGIN; INSERT INTO % VALUES (2, 'b'); ROLLBACK"));
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_held('netted', '(0,2)')"),
	'f',
	"a rolled-back insert's row version goes at its abort's take-back");
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('netted')"),
	'f', 'a fold that applies nothing installs nothing');
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_standing('netted', 'SELECT count(*) FROM netted')"
	),
	'0',
	'the table reads empty');

# Bugs row 93: a row folded long before its delete. The fold that applies the
# delete makes its tuple id pending in the base, where a TID read finds no row,
# and VACUUM frees it: on the reference, heap's prune logs the deleter as its
# conflict horizon, cancelling a standby snapshot that could still see the row,
# and gives the row's space back, so the free space entry's bytes are heap's
# page's free bytes and the next insert takes the freed tuple id. Heap's twin
# reads no TID before its VACUUM, since heap's read would prune the page
# itself.
my $settle = sub {
	my ($table) = @_;
	my $xid = sub {
		my @lines = split /\n/,
		  $node->safe_psql('postgres',
			"BEGIN; $_[0]; SELECT pg_current_xact_id(); COMMIT");
		return $lines[-1];
	};
	my $fork = $table !~ /^heap_/;
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $fork;
	$node->wait_for_replay_catchup($aged_standby);
	my $held = $aged_standby->background_psql('postgres', on_error_stop => 0);
	$held->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
	my $ender = $xid->(written($table, 'DELETE FROM % WHERE k = 2'));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $fork;
	my $read = "SELECT count(*) FROM $table WHERE ctid = ''(0,2)''";
	my $dead =
	  $fork
	  ? $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('$table', '$read')")
	  : '0';
	my $from =
	  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$node->safe_psql('postgres', written($table, 'vacuum'));
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->wait_for_replay_catchup($aged_standby);
	my $kept =
	  eval { $held->query_safe('SELECT 1'); 1 } ? 'kept' : 'cancelled';
	eval { $held->quit };
	my @horizons =
	  map { $_ == $ender || $_ == $ender % 2**32 ? 'ender' : $_ }
	  @{ horizons($table, $from) };
	my $free = $node->safe_psql('postgres',
		$fork
		? "SELECT doltgres_test_free_space('$table', 0)"
		: "SELECT upper - lower FROM page_header(get_raw_page('$table', 0))");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (3, 'c')"));
	return [
		$dead, @horizons, $kept,
		$free =~ s/ .*//r,
		$node->safe_psql('postgres', reading($table, 1))
	];
};
my ($settled, $heap_settled) =
  map { $settle->($_) } ('settled', 'heap_settled');
is_deeply(
	$heap_settled,
	[ '0', 'ender', 'cancelled', $heap_settled->[3], '(0,1) (0,2)' ],
	"heap's VACUUM frees a long-lived row's tuple id, logs its deleter, and the next insert takes it"
);
is_deeply($settled, $heap_settled,
	"the fork's VACUUM frees a long-folded row's pending tuple id as heap's twin's does: no TID read finds it, it logs the deleter and cancels the standby's snapshot, gives the bytes back, and the next insert takes it"
);

# Bugs row 93's crash: a crash after the checkpoint that saves the base a fold
# made a tuple id pending in, and before VACUUM, keeps it pending, since the
# saved base holds it, and VACUUM after the restart frees it, so the next insert
# takes it as heap's twin's does. (A crash before the fold's packs are listed
# leaves the fold's point for the server's own next fold to reach, which makes
# the tuple id pending again.)
for my $table ('pended', 'heap_pended')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $table !~ /^heap_/;
	$node->safe_psql('postgres',
		written($table, 'DELETE FROM % WHERE k = 2'));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $table !~ /^heap_/;
}
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
for my $table ('pended', 'heap_pended')
{
	$node->safe_psql('postgres', written($table, 'vacuum'));
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (3, 'c')"));
}
is( $node->safe_psql('postgres', reading('pended', 1)),
	$node->safe_psql('postgres', reading('heap_pended', 1)),
	"VACUUM after a crash frees a tuple id a fold made pending before it, and the next insert takes it as heap's twin's does"
);

# Bugs row 93's churn: rows inserted, folded, deleted, folded, and vacuumed in
# rounds beside a row that stays, so heap's VACUUM truncates nothing; each
# round's rows take the tuple ids the round before freed, so the table stays
# the blocks heap's twin does.
for my $table ('churned', 'heap_churned')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (0, 'stays')"));
	for my $round (1 .. 5)
	{
		$node->safe_psql(
			'postgres',
			written(
				$table,
				"INSERT INTO % SELECT g, repeat('r', 300) FROM generate_series(1, 10) g"
			));
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
		  if $table !~ /^heap_/;
		$node->safe_psql('postgres',
			written($table, 'DELETE FROM % WHERE k > 0'));
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
		  if $table !~ /^heap_/;
		$node->safe_psql('postgres', written($table, 'vacuum'));
	}
}
is( $node->safe_psql('postgres', "SELECT doltgres_test_blocks('churned')"),
	$node->safe_psql(
		'postgres', "SELECT pg_relation_size('heap_churned') / 8192"),
	"a table that churns its folded rows keeps heap's twin's blocks");

# Bugs row 101: a standby with hot_standby_feedback holds a snapshot taken
# before a delete, so the primary's horizon stays below the deleter. Neither a
# fold, which applies nothing past that horizon (the fold text's draft 12), nor
# VACUUM cancels the snapshot or frees the row's tuple id, so the next insert
# takes another, as heap's twin's does; once the snapshot ends and the standby's
# feedback passes the deleter, the next fold applies the delete, dropping the
# row from the delta or making its tuple id pending in the base, VACUUM frees
# it, and the insert after takes it. A row one transaction inserted and deleted after the
# snapshot waits the same way, since heap's prune holds a tuple whose xmax
# equals its xmin until the horizon passes that xid, though its record logs no
# conflict horizon for it (PRevPF 8's R101a).
$node->backup('fed_backup');
my $fed = PostgreSQL::Test::Cluster->new('fed_standby');
$fed->init_from_backup($node, 'fed_backup', has_streaming => 1);
$fed->append_conf('postgresql.conf',
	"hot_standby_feedback = on\nwal_receiver_status_interval = 1\nmax_standby_streaming_delay = 0"
);
$fed->start;
my $feed = sub {
	my ($table, $mode) = @_;
	my $fork = $table !~ /^heap_/;
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql(
		'postgres',
		written(
			$table,
			$mode eq 'own'
			? "INSERT INTO % VALUES (1, 'a')"
			: "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $fork && $mode eq 'pending';
	$node->wait_for_replay_catchup($fed);
	my $held = $fed->background_psql('postgres', on_error_stop => 0);
	$held->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
	my $xmin = $held->query_safe(
		'SELECT backend_xmin FROM pg_stat_activity WHERE pid = pg_backend_pid()'
	);
	$node->poll_query_until('postgres',
		"SELECT backend_xmin::text::bigint <= $xmin FROM pg_stat_replication WHERE application_name = 'fed_standby'"
	) or die "the standby's feedback didn't reach the primary";
	my @lines = split /\n/,
	  $node->safe_psql(
		'postgres',
		'BEGIN; '
		  . written(
			$table,
			($mode eq 'own' ? "INSERT INTO % VALUES (2, 'b'); " : '')
			  . 'DELETE FROM % WHERE k = 2')
		  . '; SELECT pg_current_xact_id(); COMMIT');
	my $ender = $lines[-1] % 2**32;
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $fork;
	$node->safe_psql('postgres', written($table, 'vacuum'));
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (3, 'c')"));
	$node->wait_for_replay_catchup($fed);
	my $kept =
	  eval { $held->query_safe('SELECT 1'); 1 } ? 'kept' : 'cancelled';
	my $before = $node->safe_psql('postgres', reading($table, 1));
	$held->quit;
	$node->poll_query_until('postgres',
		"SELECT coalesce(backend_xmin::text::bigint > $ender, true) FROM pg_stat_replication WHERE application_name = 'fed_standby'"
	) or die "the standby's feedback didn't pass the deleter";
	# The fold VACUUM is to run before its free (PLAN item 2's Freeing, the
	# fold's piece), where heap's VACUUM prunes.
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $fork;
	$node->safe_psql('postgres', written($table, 'vacuum'));
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (4, 'd')"));
	return [ $kept, $before,
		$node->safe_psql('postgres', reading($table, 1)) ];
};
for my $mode ('dropped', 'pending', 'own')
{
	my $name = "fed_$mode";
	my ($fork, $heap) =
	  map { $feed->($_, $mode) } ($name, "heap_$name");
	is_deeply(
		$heap,
		[ 'kept', '(0,1) (0,3)', '(0,1) (0,2) (0,3)' ],
		"heap's VACUUM keeps a row a standby's feedback holds, and frees it once the feedback passes its deleter"
	);
	is_deeply($fork, $heap,
		"the fork's fold and VACUUM keep a $name row a standby's feedback holds, cancel nothing, and free it once the feedback passes its deleter, as heap's twin's do"
	);
}

# Bugs row 102: on the reference, VACUUM writes a btree index's cleanup
# statistics to its pg_class row when its bulk delete ran
# (update_relstats_all_indexes), so an index made on an empty table shows its
# pages and live entries after a delete and VACUUM, where CREATE INDEX left one
# page and none. The fed standby stops first, since its feedback would hold the
# horizon below the delete and leave heap's twin no bulk delete to write.
$fed->stop;
for my $table ('relstats', 'heap_relstats')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres', "CREATE INDEX ${table}_k ON $table (k)");
	$node->safe_psql('postgres', written($table, $_))
	  for ("INSERT INTO % SELECT g, 'r' FROM generate_series(1, 4) g",
		'DELETE FROM % WHERE k = 4');
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')")
	  if $table !~ /^heap_/;
	$node->safe_psql('postgres', written($table, 'vacuum'));
}
my $relstats = sub {
	return $node->safe_psql('postgres',
		"SELECT relpages || ' ' || reltuples FROM pg_class WHERE oid = '$_[0]_k'::regclass"
	);
};
is($relstats->('heap_relstats'),
	'2 3', "heap's VACUUM writes a btree index's pages and live entries");
is($relstats->('relstats'), $relstats->('heap_relstats'),
	"the fork's VACUUM writes a btree index's cleanup statistics to pg_class as heap's twin's does"
);

# The unknown methods' text, section 4: an index the fork stores names no tuple
# id, so VACUUM runs no bulk delete on it, and places tuple ids as heap does:
# with INDEX_CLEANUP on it frees an aborted insert's tuple id, which the next
# insert takes, as heap's twin frees its dead item after vacuuming its btree, and
# with INDEX_CLEANUP off a table with an index keeps the tuple id taken, as heap's
# twin keeps its dead item, whether the index is the fork's or another method's
# (dummy_index_am's).
$node->safe_psql('postgres', 'CREATE EXTENSION dummy_index_am');
my %placed;
for my $table (map { ($_, "heap_$_") } qw(ported_on ported_off unported_off))
{
	my $cleanup = $table =~ /_on$/ ? 'on' : 'off';
	my $routine =
	  $table =~ /^heap_/ ? '' : "SELECT doltgres_test_index_routine('${table}_k'); ";
	$node->safe_psql('postgres',
		"CREATE TABLE $table (k int4, v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', "CREATE INDEX ${table}_k ON $table (k)");
	$node->safe_psql('postgres',
		"CREATE INDEX ${table}_dummy ON $table USING dummy_index_am (k)")
	  if $table =~ /unported/;
	$node->safe_psql('postgres', $routine . written($table, $_))
	  for ("INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
		"BEGIN; INSERT INTO % VALUES (3, 'v3'); ROLLBACK",
		"vacuum $cleanup", "INSERT INTO % VALUES (4, 'v4')");
	$placed{$table} = $node->safe_psql('postgres', reading($table));
}
is( "$placed{ported_on} / $placed{heap_ported_on}",
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,3) (4,v4) / (0,1) (1,v1)\n(0,2) (2,v2)\n(0,3) (4,v4)",
	"VACUUM runs no bulk delete on an index the fork stores and frees an aborted insert's tuple id as heap's twin frees its dead item, so the next insert takes it"
);
is( "$placed{ported_off} / $placed{heap_ported_off}",
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,4) (4,v4) / (0,1) (1,v1)\n(0,2) (2,v2)\n(0,4) (4,v4)",
	"VACUUM with INDEX_CLEANUP off keeps an aborted insert's tuple id taken on a table with an index the fork stores, as heap's twin keeps its dead item"
);
is( "$placed{unported_off} / $placed{heap_unported_off}",
	"(0,1) (1,v1)\n(0,2) (2,v2)\n(0,4) (4,v4) / (0,1) (1,v1)\n(0,2) (2,v2)\n(0,4) (4,v4)",
	"VACUUM with INDEX_CLEANUP off leaves an aborted insert's tuple id taken beside an index the fork doesn't store, as heap's"
);

# Bugs row 102's case for an index the fork stores: VACUUM runs its cleanup
# with no bulk delete, which writes its tree's counts to pg_class as heap's
# twin's btree writes its pages and live entries.
for my $table ('ported_stats', 'heap_ported_stats')
{
	$node->safe_psql('postgres',
		"CREATE TABLE $table (k int4, v text)" . ($table =~ /^heap_/ ? '' : ' USING heap'));
	$node->safe_psql('postgres', "CREATE INDEX ${table}_k ON $table (k)");
	$node->safe_psql('postgres', written($table, $_))
	  for ("INSERT INTO % SELECT g, 'r' FROM generate_series(1, 4) g",
		'DELETE FROM % WHERE k = 4');
}
$node->safe_psql('postgres', q{
	SELECT doltgres_test_fold('ported_stats');
	SELECT doltgres_test_index_build('ported_stats', 'ported_stats_k');
});
$node->safe_psql('postgres',
	"SELECT doltgres_test_index_routine('ported_stats_k'); " . written('ported_stats', 'vacuum'));
$node->safe_psql('postgres', written('heap_ported_stats', 'vacuum'));
is($relstats->('ported_stats'), $relstats->('heap_ported_stats'),
	"the fork's VACUUM writes the cleanup statistics of an index the fork stores to pg_class as heap's twin's btree does"
);

# The unknown methods' text, section 4: VACUUM never calls the bulk delete of an
# index the fork stores, whose entries name no tuple id, so a VACUUM that frees
# an aborted insert's tuple id completes while that bulk delete would raise.
$node->safe_psql('postgres', 'CREATE TABLE unbulked (k int4, v text) USING heap');
$node->safe_psql('postgres', 'CREATE INDEX unbulked_k ON unbulked (k)');
$node->safe_psql('postgres',
	"SELECT doltgres_test_index_routine('unbulked_k'); " . written('unbulked', $_))
  for ("INSERT INTO % VALUES (1, 'v1'), (2, 'v2')",
	"BEGIN; INSERT INTO % VALUES (3, 'v3'); ROLLBACK");
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-index-bulkdelete', 'error')");
my ($unbulked_ret, $unbulked_out, $unbulked_err) = $node->psql('postgres',
	"SELECT doltgres_test_index_routine('unbulked_k'); " . written('unbulked', 'vacuum on'));
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-index-bulkdelete')");
is( "$unbulked_ret $unbulked_err / "
	  . $node->safe_psql('postgres', reading('unbulked', 1)),
	'0  / (0,1) (0,2)',
	"VACUUM never calls the bulk delete of an index the fork stores");

# Bugs row 110: a standby replays VACUUM's frees of a rolled-back insert's
# twenty thousand tuple ids, a record each, and reads the table empty; its
# cost per removal, the pages a removal looks at, its free passes, and its
# compactions, is bounded by the txn units' counts rather than a clock here.
$node->backup('bounded_backup');
my $bounded = PostgreSQL::Test::Cluster->new('bounded_standby');
$bounded->init_from_backup($node, 'bounded_backup', has_streaming => 1);
$bounded->start;
$node->safe_psql('postgres',
	'CREATE TABLE bounded (k int4, v text) WITH (fillfactor = 10)');
$node->safe_psql('postgres',
	written('bounded', 'CREATE INDEX ON % USING hash (k)'));
$node->safe_psql(
	'postgres',
	written(
		'bounded',
		"BEGIN; INSERT INTO % SELECT k, 'v' || k FROM generate_series(1, 20000) k; ROLLBACK"
	));
$node->safe_psql('postgres',
	"SET maintenance_work_mem = 64; SELECT doltgres_test_vacuum('bounded', 'on')"
);
$node->wait_for_replay_catchup($bounded);
is( $bounded->safe_psql(
		'postgres',
		"SELECT doltgres_test_standing('bounded', 'SELECT count(*) FROM bounded')"),
	'0',
	"a standby replays VACUUM's frees of twenty thousand tuple ids and reads the table empty"
);
$bounded->stop;

# Bugs row 289: a standby whose own fold hasn't reached its primary's fold's
# point when it replays a free defers the free's take-outs to the read past
# that point's record, which its fold does once installed. A standby made now
# holds its fold launcher at doltgres-fold-launch while the primary folds an
# update and frees the row's old version: the version is still held after the
# free's replay, and once the launcher goes on, the standby's fold reads past
# the point and takes it out.
$node->backup('deferred_backup');
my $deferred = PostgreSQL::Test::Cluster->new('deferred_standby');
$deferred->init_from_backup($node, 'deferred_backup', has_streaming => 1);
$deferred->start;
$node->safe_psql('postgres', 'CREATE TABLE deferred (k int4, v text)');
$node->safe_psql('postgres',
	written('deferred', "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
$node->safe_psql('postgres',
	written('deferred', "UPDATE % SET v = 'z' WHERE k = 1"));
$node->wait_for_replay_catchup($deferred);
$deferred->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
ok( $deferred->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'"
	),
	"a standby's fold launcher waits at its injection point");
$node->safe_psql('postgres', "SELECT doltgres_test_fold('deferred')");
$node->safe_psql('postgres', written('deferred', 'vacuum'));
$node->wait_for_replay_catchup($deferred);
is( $deferred->safe_psql(
		'postgres', "SELECT doltgres_test_held('deferred', '(0,1)')"),
	't',
	"a standby whose fold hasn't reached its primary's fold's point still holds the freed row version after the free's replay"
);
$deferred->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
);
ok( $deferred->poll_query_until(
		'postgres', "SELECT doltgres_test_held('deferred', '(0,1)')", 'f'),
	"a standby's fold past its primary's fold's point does the take-outs the free's replay deferred"
);
is( $deferred->safe_psql('postgres', reading('deferred', 1)),
	$node->safe_psql('postgres', reading('deferred', 1)),
	"the standby's rows agree with the primary's after its fold reads past the deferred free"
);
$deferred->stop;

# Bugs row 294: a fork VACUUM running when a row's delete commits, held here
# at its look at the block of its own table, whose rows are unfolded, keeps
# every fold below the delete, since its snapshot may still read the row, so
# the fold applies nothing and VACUUM frees nothing; once the VACUUM ends, the
# next fold drops the row and VACUUM frees its tuple id, logging the deleter
# as its horizon. The fold launcher is held through the case, so no fold
# worker's snapshot keeps a fold below it.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
$node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'"
) or die 'the fold launcher is not held';
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'fold worker'"
) or die 'a fold worker still runs';
for my $table ('vacuuming', 'held_by')
{
	$node->safe_psql('postgres', "CREATE TABLE $table (k int4, v text)");
	$node->safe_psql('postgres',
		written($table, "INSERT INTO % VALUES (1, 'a'), (2, 'b')"));
}
$node->safe_psql('postgres', "SELECT doltgres_test_fold('held_by')");
my $vacuuming = $node->background_psql('postgres', on_error_stop => 0);
$vacuuming->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-vacuum-found', 'wait')"
);
$vacuuming->query_until(qr/vacuuming/,
	"\\echo vacuuming\n" . written('vacuuming', 'vacuum') . ";\n");
$node->wait_for_event('client backend', 'doltgres-vacuum-found');
my $deleter = (
	split /\n/,
	$node->safe_psql(
		'postgres',
		'BEGIN; '
		  . written('held_by', 'DELETE FROM % WHERE k = 1')
		  . '; SELECT pg_current_xact_id(); COMMIT'))[-1];
my $from = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $held_fold =
  $node->safe_psql('postgres', "SELECT doltgres_test_fold('held_by')");
$node->safe_psql('postgres', written('held_by', 'vacuum'));
$node->safe_psql('postgres', 'CHECKPOINT');
is( $held_fold . ' / ' . join(',', @{ horizons('held_by', $from) }),
	'f / ',
	"a VACUUM running when a delete commits keeps the fold below the delete, so VACUUM frees nothing"
);
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-vacuum-found'); SELECT injection_points_wakeup('doltgres-vacuum-found')"
);
$vacuuming->query_safe('SELECT 1');
$vacuuming->quit;
$from = $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $freed_fold =
  $node->safe_psql('postgres', "SELECT doltgres_test_fold('held_by')");
$node->safe_psql('postgres', written('held_by', 'vacuum'));
$node->safe_psql('postgres', 'CHECKPOINT');
is( $freed_fold . ' / '
	  . join(',',
		map { $_ eq $deleter ? 'deleter' : $_ } @{ horizons('held_by', $from) }),
	't / deleter',
	"once the VACUUM ends, the next fold drops the row and VACUUM's free logs its deleter"
);
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
);

done_testing();
