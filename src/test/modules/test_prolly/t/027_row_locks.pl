# Row-level concurrency on prolly tables: writers lock rows, not tables, and
# conflicts follow heap semantics. Lock records are per branch.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('row_locks');
$node->init(extra => [ '--encoding=UTF8', '--locale=C' ]);
$node->append_conf('postgresql.conf',
	"max_parallel_workers_per_gather=0\ndefault_table_access_method=dolt_prolly_table\n");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub session
{
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	my $pid = $s->query_safe('SELECT pg_backend_pid()');
	return ($s, $pid);
}

# Start |$query| in |$s| without waiting for it to finish.
sub start
{
	my ($s, $query) = @_;
	$s->query_until(qr/started/, "\\echo started\n$query\n");
}

sub blocked
{
	my ($pid, $label) = @_;
	ok( $node->poll_query_until(
			'postgres',
			"SELECT wait_event_type = 'Lock' FROM pg_stat_activity WHERE pid = $pid"),
		$label);
}

sql(q{CREATE TABLE t(id int PRIMARY KEY, v int);
INSERT INTO t SELECT g, g * 10 FROM generate_series(1, 4) g});
my ($a, $apid) = session();
my ($b, $bpid) = session();

# Writers of different rows never wait for each other, and both publish.
$a->query_safe('BEGIN; UPDATE t SET v = 11 WHERE id = 1');
$b->query_safe('BEGIN; UPDATE t SET v = 21 WHERE id = 2');
$b->query_safe('INSERT INTO t VALUES (5, 50)');
$a->query_safe('COMMIT');
$b->query_safe('COMMIT');
is(sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM t}),
	'1:11,2:21,3:30,4:40,5:50', 'disjoint concurrent writers both publish');

# A writer of the same row waits; under Read Committed it then re-evaluates
# against the newer version.
$a->query_safe('BEGIN; UPDATE t SET v = v + 1 WHERE id = 3');
start($b, 'UPDATE t SET v = v * 2 WHERE id = 3 RETURNING v;');
blocked($bpid, 'second writer of a row waits for the first');
$a->query_safe('COMMIT');
like($b->query_safe('SELECT 1'), qr/62/, 'read committed writer updates the newer version');
is(sql('SELECT v FROM t WHERE id = 3'), '62', 'both updates of the row apply in order');

# Under Repeatable Read the second writer fails instead.
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t');
$a->query_safe('BEGIN; UPDATE t SET v = 0 WHERE id = 4');
start($b, 'UPDATE t SET v = 1 WHERE id = 4;');
blocked($bpid, 'repeatable read writer waits for the row lock');
$a->query_safe('COMMIT');
$b->query('SELECT 1');
like($b->{stderr}, qr/could not serialize access due to concurrent update/,
	'repeatable read writer of an updated row fails');
$b->{stderr} = '';
$b->query('ROLLBACK');

# A writer that waited follows the row to its latest version, passing the
# versions its own statement replaced, as heap does. If a later command of
# the statement, here in a function, made that version, the statement fails;
# if the statement itself made it, the second update is ignored.
sql(q{CREATE FUNCTION bump(int) RETURNS bool LANGUAGE sql AS $$UPDATE t SET v = v + 1 WHERE id = $1; SELECT true$$});
$a->query_safe('BEGIN; UPDATE t SET v = v + 1 WHERE id = 3');
start($b, 'WITH up AS (UPDATE t SET v = v + 100 WHERE id = 3 RETURNING bump(3)) UPDATE t SET v = 0 FROM up WHERE t.id = 3;');
blocked($bpid, 'writer waits behind a concurrent update');
$a->query_safe('COMMIT');
$b->query('SELECT 1');
like($b->{stderr}, qr/tuple to be updated was already modified by an operation triggered by the current command/,
	'a version a later command made is an error after waiting');
$b->{stderr} = '';
is(sql('SELECT v FROM t WHERE id = 3'), '63', 'the failed statement changed nothing');
$a->query_safe('BEGIN; UPDATE t SET v = v + 1 WHERE id = 3');
start($b, 'WITH up AS (UPDATE t SET v = v + 100 WHERE id = 3 RETURNING id) UPDATE t SET v = 0 FROM up WHERE t.id = 3;');
blocked($bpid, 'writer waits behind a concurrent update again');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 3'), '164', 'a version the same command made is updated only once');

# A deleted row is skipped by a waiting Read Committed writer.
$a->query_safe('BEGIN; DELETE FROM t WHERE id = 5');
start($b, 'UPDATE t SET v = 99 WHERE id = 5;');
blocked($bpid, 'writer waits for a deleter');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');
is(sql('SELECT count(*) FROM t WHERE id = 5'), '0', 'waiting writer finds the row deleted');

# Lock strengths: key share is compatible with a non-key update, share is not.
$a->query_safe('BEGIN; UPDATE t SET v = 12 WHERE id = 1');
is($b->query_safe('BEGIN; SELECT id FROM t WHERE id = 1 FOR KEY SHARE'),
	'1', 'key share lock does not wait for a non-key update');
$b->query_safe('ROLLBACK');
start($b, 'SELECT id FROM t WHERE id = 1 FOR SHARE;');
blocked($bpid, 'share lock waits for an update');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');

# A key share lock carries over to the version a non-key update makes, so a
# later delete of that version waits for the key share holder.
my ($c, $cpid) = session();
$a->query_safe('BEGIN; SELECT id FROM t WHERE id = 3 FOR KEY SHARE');
$b->query_safe('BEGIN; UPDATE t SET v = 63 WHERE id = 3');
$b->query_safe('COMMIT');
start($c, 'DELETE FROM t WHERE id = 3;');
blocked($cpid, 'delete of the new version waits for the propagated key share lock');
$a->query_safe('COMMIT');
$c->query_safe('SELECT 1');
is(sql('SELECT count(*) FROM t WHERE id = 3'), '0', 'delete proceeds once the key share holder commits');
$c->quit;
sql('INSERT INTO t VALUES (3, 62)');

# NOWAIT and SKIP LOCKED.
$a->query_safe('BEGIN; SELECT * FROM t WHERE id = 1 FOR UPDATE');
$b->query('SELECT * FROM t WHERE id = 1 FOR UPDATE NOWAIT');
like($b->{stderr}, qr/could not obtain lock on row in relation "t"/, 'NOWAIT reports the locked row');
$b->{stderr} = '';
is($b->query_safe(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM (SELECT id FROM t ORDER BY id FOR UPDATE SKIP LOCKED) s}),
	'2,3,4', 'SKIP LOCKED skips the locked row');
$a->query_safe('COMMIT');

# Concurrent inserts of one unique key: the second waits, then fails if the
# first commits and succeeds if it aborts.
$a->query_safe('BEGIN; INSERT INTO t VALUES (6, 60)');
start($b, 'INSERT INTO t VALUES (6, 61);');
blocked($bpid, 'second inserter of a key waits');
$a->query_safe('COMMIT');
$b->query('SELECT 1');
like($b->{stderr}, qr/duplicate key value violates unique constraint "t_pkey"/,
	'second inserter fails after the first commits');
$b->{stderr} = '';
$a->query_safe('BEGIN; INSERT INTO t VALUES (7, 70)');
start($b, 'INSERT INTO t VALUES (7, 71);');
blocked($bpid, 'second inserter waits again');
$a->query_safe('ROLLBACK');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 7'), '71', 'second inserter succeeds after the first aborts');

# Re-inserting a key whose row a running transaction deletes waits for it.
$a->query_safe('BEGIN; DELETE FROM t WHERE id = 7');
start($b, 'INSERT INTO t VALUES (7, 72);');
blocked($bpid, 'inserter waits for the deleter of a conflicting row');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 7'), '72', 'insert succeeds once the conflicting row is deleted');

# A deferrable unique constraint is checked when the statement or transaction
# ends. An inserter of a key another transaction holds does not wait; its
# commit-time recheck does.
sql(q{CREATE TABLE d(id int UNIQUE DEFERRABLE INITIALLY IMMEDIATE, v int);
INSERT INTO d VALUES (1, 1), (2, 2)});
sql('UPDATE d SET id = id + 1');
is(sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM d}), '2:1,3:2',
	'a statement may pass through duplicate keys of a deferrable constraint');
$b->query('INSERT INTO d VALUES (2, 9)');
like($b->{stderr}, qr/duplicate key value violates unique constraint "d_id_key"/,
	'a duplicate fails at the end of its statement');
$b->{stderr} = '';
$b->query_safe('BEGIN; SET CONSTRAINTS ALL DEFERRED; INSERT INTO d VALUES (2, 9); UPDATE d SET id = 4 WHERE v = 9; COMMIT');
$b->query('BEGIN; SET CONSTRAINTS ALL DEFERRED; INSERT INTO d VALUES (2, 10); COMMIT');
like($b->{stderr}, qr/duplicate key value violates unique constraint "d_id_key"/,
	'a deferred duplicate fails at commit');
$b->{stderr} = '';
is(sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM d}), '2:1,3:2,4:9',
	'deferred checks keep resolved duplicates and reject the rest');
$a->query_safe('BEGIN; INSERT INTO d VALUES (5, 50)');
$b->query_safe('BEGIN; SET CONSTRAINTS ALL DEFERRED; INSERT INTO d VALUES (5, 51)');
start($b, 'COMMIT;');
blocked($bpid, 'a deferred check waits at commit for an inserter of its key');
$a->query_safe('COMMIT');
$b->query('SELECT 1');
like($b->{stderr}, qr/duplicate key value violates unique constraint "d_id_key"/,
	'and fails once that inserter commits');
$b->{stderr} = '';

# ON CONFLICT waits for a concurrent inserter of its key, then takes its
# alternative path if that inserter commits, or inserts if it aborts.
$a->query_safe('BEGIN; INSERT INTO t VALUES (20, 80)');
start($b, 'INSERT INTO t VALUES (20, 81) ON CONFLICT DO NOTHING;');
blocked($bpid, 'ON CONFLICT DO NOTHING waits for an inserter of its key');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 20'), '80', 'DO NOTHING keeps the committed row');
$a->query_safe('BEGIN; INSERT INTO t VALUES (21, 90)');
start($b, 'INSERT INTO t VALUES (21, 91) ON CONFLICT (id) DO UPDATE SET v = t.v + excluded.v;');
blocked($bpid, 'ON CONFLICT DO UPDATE waits for an inserter of its key');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 21'), '181', 'DO UPDATE updates the row the inserter committed');
$a->query_safe('BEGIN; INSERT INTO t VALUES (22, 100)');
start($b, 'INSERT INTO t VALUES (22, 101) ON CONFLICT (id) DO UPDATE SET v = 0;');
blocked($bpid, 'ON CONFLICT waits again');
$a->query_safe('ROLLBACK');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 22'), '101', 'ON CONFLICT inserts once the other inserter aborts');

# It waits for a running updater of the conflicting row and updates the newer
# version, or inserts if a running deleter commits.
$a->query_safe('BEGIN; UPDATE t SET v = 102 WHERE id = 22');
start($b, 'INSERT INTO t VALUES (22, 1) ON CONFLICT (id) DO UPDATE SET v = t.v + excluded.v;');
blocked($bpid, 'DO UPDATE waits for an updater of the conflicting row');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 22'), '103', 'DO UPDATE updates the version the updater made');
$a->query_safe('BEGIN; DELETE FROM t WHERE id = 22');
start($b, 'INSERT INTO t VALUES (22, 104) ON CONFLICT (id) DO UPDATE SET v = 0;');
blocked($bpid, 'ON CONFLICT waits for a deleter of the conflicting row');
$a->query_safe('COMMIT');
$b->query_safe('SELECT 1');
is(sql('SELECT v FROM t WHERE id = 22'), '104', 'ON CONFLICT inserts once the conflicting row is deleted');

# One command proposing a key twice: DO UPDATE fails, DO NOTHING inserts once.
$b->query('INSERT INTO t VALUES (23, 110), (23, 111) ON CONFLICT (id) DO UPDATE SET v = excluded.v');
like($b->{stderr}, qr/ON CONFLICT DO UPDATE command cannot affect row a second time/,
	'DO UPDATE cannot change a row its own command inserted');
$b->{stderr} = '';
$b->query_safe('INSERT INTO t VALUES (23, 112), (23, 113) ON CONFLICT DO NOTHING');
is(sql('SELECT v FROM t WHERE id = 23'), '112', 'DO NOTHING skips a key its own command inserted');

# Under Repeatable Read a conflict with a row committed after the snapshot is
# a serialization failure.
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
sql('INSERT INTO t VALUES (24, 120)');
$b->query('INSERT INTO t VALUES (24, 121) ON CONFLICT DO NOTHING');
like($b->{stderr}, qr/could not serialize access due to concurrent update/,
	'repeatable read cannot skip a row it does not see');
$b->{stderr} = '';
$b->query_safe('ROLLBACK');

# Under Repeatable Read writes build on the transaction snapshot
# (dolt_snapshot_writes): a commit of other rows meanwhile neither fails the
# writer nor is lost when it publishes. Uniqueness still sees the latest
# state, as heap's dirty snapshot does: a key committed since conflicts, and
# a key whose row was deleted since is free.
my $updated = sql(q{SELECT string_agg((v + 1)::text, ',' ORDER BY id) FROM t WHERE id IN (1, 2)});
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t');
sql('INSERT INTO t VALUES (25, 130); UPDATE t SET v = v + 1 WHERE id = 1');
$b->query_safe('INSERT INTO t VALUES (26, 140); UPDATE t SET v = v + 1 WHERE id = 2');
is($b->query_safe('SELECT count(*) FROM t WHERE id = 25'), '0', 'the snapshot hides a row committed since');
$b->query_safe('COMMIT');
is(sql(q{SELECT string_agg(v::text, ',' ORDER BY id) FROM t WHERE id IN (1, 2, 25, 26)}),
	"$updated,130,140", 'writes on the snapshot merge with later commits');
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t');
sql('INSERT INTO t VALUES (27, 150); DELETE FROM t WHERE id = 26');
$b->query('INSERT INTO t VALUES (27, 151)');
like($b->{stderr}, qr/duplicate key value violates unique constraint "t_pkey"/,
	'a key committed since the snapshot is a duplicate');
$b->{stderr} = '';
$b->query_safe('ROLLBACK');
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t');
sql('DELETE FROM t WHERE id = 25');
$b->query_safe('INSERT INTO t VALUES (25, 131)');
$b->query_safe('COMMIT');
is(sql('SELECT v FROM t WHERE id = 25'), '131', 'a key deleted since the snapshot is free');
sql('CREATE TABLE snapdup(v int)');
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM snapdup');
sql('INSERT INTO snapdup VALUES (1), (1)');
$b->query_safe('INSERT INTO snapdup VALUES (1), (2)');
$b->query_safe('COMMIT');
is(sql(q{SELECT string_agg(v::text, ',' ORDER BY v) FROM snapdup}), '1,1,1,2',
	'identical keyless rows from both merge into their count');
sql('UPDATE snapdup SET v = 3 WHERE v = 1');
is(sql('SELECT count(*) FROM snapdup WHERE v = 3'), '3', 'and every occurrence stays addressable');

# AFTER triggers and RETURNING see the old version: each version has its own TID.
# TODO: triggers on versioned tables. CREATE TRIGGER is native DDL today, and
# a native trigger cannot depend on a versioned table.
SKIP: {
 skip 'triggers on versioned tables are not supported yet', 1;
 is(sql(q{SELECT old_v || '->' || new_v FROM audit}), '21->22', 'AFTER trigger sees old and new versions');
}
my $before = sql('SELECT ctid FROM t WHERE id = 2');
sql('UPDATE t SET v = 22 WHERE id = 2');
isnt(sql('SELECT ctid FROM t WHERE id = 2'), $before, 'an update gives the row a new TID');
is(sql(q{UPDATE t SET v = 23 WHERE id = 2 RETURNING old.v || '->' || new.v}), '22->23',
	'RETURNING old and new');

# A row's handle is derived from its content and survives a restart. Handles
# the counter allocates, for duplicate rows, never repeat a committed one
# across a restart. The handle an aborted update derived for its new version
# is derived again when that version is inserted, and the dead lock record
# the update left must not shadow the row that has it now.
$a->quit;
$b->quit;
sql('CREATE TABLE dup(x int); INSERT INTO dup VALUES (1), (1)');
my $max = sql('SELECT max((ctid::text::point)[0] * 65535 + (ctid::text::point)[1]) FROM dup');
my $derived = sql('INSERT INTO t VALUES (8, 80) RETURNING ctid');
$node->restart;
is(sql('SELECT ctid FROM t WHERE id = 8'), $derived, 'a derived handle survives a restart');
sql('INSERT INTO dup VALUES (1)');
cmp_ok(sql('SELECT max((ctid::text::point)[0] * 65535 + (ctid::text::point)[1]) FROM dup'),
	'>', $max, 'handles allocated after a restart exceed every committed one');
my $aborted = sql('BEGIN; UPDATE t SET v = 0 WHERE id = 1 RETURNING ctid; ROLLBACK');
$node->restart;
is(sql('DELETE FROM t WHERE id = 1; INSERT INTO t VALUES (1, 0) RETURNING ctid'), $aborted,
	'the handle an aborted update derived is derived again');
sql('UPDATE t SET v = 91 WHERE id = 1; UPDATE t SET v = v + 1 WHERE id = 1');
is(sql('SELECT v FROM t WHERE id = 1'), '92', 'the row with a reused handle locks and updates');

($a, $apid) = session();
($b, $bpid) = session();

# Foreign keys: a key-share check does not block non-key updates of the parent.
sql(q{CREATE TABLE child(id int PRIMARY KEY, parent int REFERENCES t(id))});
$a->query_safe('BEGIN; UPDATE t SET v = 24 WHERE id = 2');
is($b->query_safe('INSERT INTO child VALUES (1, 2); SELECT count(*) FROM child'), '1',
	'child insert does not wait for a non-key parent update');
$a->query_safe('COMMIT');

# Locks are per branch: a lock on main does not block the same row elsewhere.
sql(q{SELECT dolt_commit('-Am', 'base'); SELECT dolt_branch('other')});
$b->query_safe(q{SELECT dolt_checkout('other')});
$a->query_safe('BEGIN; UPDATE t SET v = 100 WHERE id = 1');
is($b->query_safe('BEGIN; UPDATE t SET v = 200 WHERE id = 1; SELECT v FROM t WHERE id = 1'),
	'200', 'a row lock on one branch does not block another branch');
$b->query_safe('COMMIT');
$a->query_safe('COMMIT');
is(sql('SELECT v FROM t WHERE id = 1'), '100', 'main keeps its own update');
is($b->query_safe('SELECT v FROM t WHERE id = 1'), '200', 'the other branch keeps its update');
$b->query('BEGIN; SET CONSTRAINTS ALL DEFERRED; INSERT INTO d VALUES (2, 11); COMMIT');
like($b->{stderr}, qr/duplicate key value violates unique constraint "d_id_key"/,
	'a branch rebuilt from canonical catalogs keeps the deferred recheck');
$b->{stderr} = '';

# Concurrent first publishers of a new table both commit.
sql('CREATE TABLE fresh(id int)');
$b->query_safe(q{SELECT dolt_checkout('main')});
$a->query_safe('BEGIN; INSERT INTO fresh VALUES (1)');
$b->query_safe('BEGIN; INSERT INTO fresh VALUES (2)');
$a->query_safe('COMMIT');
$b->query_safe('COMMIT');
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM fresh}), '1,2',
	'concurrent first publishers both publish');

$a->quit;
$b->quit;

$node->stop;
done_testing();
