# The prolly delta store: committed changes live as versions over each
# tree's base until a fold writes them into a new base. They survive crashes
# through the WAL and the checkpoint's state file, replay on a hot standby,
# leave rolled back writers out, fold without changing what readers see, and
# are pruned once every snapshot reads past the fold.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('delta_primary');
$node->init(allows_streaming => 1, extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', qq{default_table_access_method='dolt_prolly_table'
dolt_fold_versions=100000000
dolt_fold_timeout=100000000
});
$node->start;
sub sql { return $node->safe_psql('postgres', shift) }

# Nothing folds on its own: every change below stays in the delta store.
sql(q{CREATE TABLE t(id int PRIMARY KEY, v text NOT NULL);
CREATE INDEX t_v ON t(v);
INSERT INTO t SELECT g, 'v' || g || repeat('.', 400) FROM generate_series(1, 2000) g});
my $expected = sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t});
my ($versions) = split /\|/, (sql('SELECT versions, trees FROM dolt_delta_stats()'));
cmp_ok($versions, '>', 4000, "the rows are versions in the delta store ($versions)");

# A checkpoint saves the versions, a chunk at a time; later ones come back
# from the WAL.
sql('CHECKPOINT');
cmp_ok(-s $node->data_dir . '/pg_dolt/delta.state', '>', 1024 * 1024, 'the state file spans several chunks');
sql(q{UPDATE t SET v = v || 'x' WHERE id % 3 = 0; DELETE FROM t WHERE id % 7 = 0});
sql(q{BEGIN; INSERT INTO t VALUES (-1, 'rolled back'); ROLLBACK});
$expected = sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t});
my $through_index = sql(q{SET enable_seqscan = off; SELECT count(*) FROM t WHERE v LIKE 'v1%'});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t}), $expected, 'a crash keeps every committed version');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM t WHERE v LIKE 'v1%'}), $through_index,
	'and the index agrees with the table');
is(sql(q{SELECT count(*) FROM t WHERE id = -1}), '0', 'a rolled back insert stays out');

# A standby replays the versions, and sees later commits.
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('delta_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;
sql(q{INSERT INTO t SELECT g, 'late' FROM generate_series(3001, 3100) g});
$node->wait_for_catchup($standby);
$expected = sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t});
is($standby->safe_psql('postgres', q{SELECT count(*), sum(id), sum(length(v)) FROM t}), $expected,
	'a hot standby reads the versions it replayed');

# A fold writes the versions into the trees without changing what anyone reads.
my $folded = q{SELECT count(*) FROM pg_dolt_root WHERE rootxmax::text <> '0'};
is(sql($folded), '0', 'no base is folded yet');
cmp_ok(sql('SELECT dolt_fold()'), '>=', 2, 'a fold writes the table and its index');
cmp_ok(sql($folded), '>=', 2, 'into new bases that record what they hold');
is(sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t}), $expected, 'and the rows read the same');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM t WHERE v = 'late'}), '100', 'through the index too');
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT count(*), sum(id), sum(length(v)) FROM t}), $expected,
	'and so does the standby');

# A snapshot older than the fold keeps reading the old base and the versions.
my $reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $before = $reader->query_safe('SELECT count(*), sum(id) FROM t');
sql(q{DELETE FROM t WHERE id > 3000});
sql('SELECT dolt_fold()');
is($reader->query_safe('SELECT count(*), sum(id) FROM t'), $before, 'a snapshot from before a fold reads as it did');
$reader->query_safe('COMMIT');
$reader->quit;
is(sql('SELECT count(*) FROM t WHERE id > 3000'), '0', 'and a new one sees the fold');

# A commit applies each changed tree's delta to capture its root, and installs
# that root as the tree's base, as a fold of its snapshot would: a fold after
# it writes the same trees. A writer still running at the capture keeps its
# version outside the base, and so do the capturing transaction's own later
# writes.
my $horizons = q{SELECT string_agg(rootxmax::text, ',' ORDER BY rootfile) FROM pg_dolt_root};
my $contents = q{SELECT string_agg(encode(rootcontent, 'hex'), ',' ORDER BY rootfile) FROM pg_dolt_root};
my $old_horizons = sql($horizons);
sql(q{UPDATE t SET v = 'captured' WHERE id = 1});
sql(q{SELECT dolt_commit('-Am', 'capture')});
isnt(sql($horizons), $old_horizons, 'a commit installs the trees it captured as their bases');
my $captured = sql($contents);
sql('SELECT dolt_fold()');
is(sql($contents), $captured, 'which are the trees a fold writes');
sql(q{UPDATE t SET v = 'again' WHERE id = 1});
my $running = $node->background_psql('postgres');
$running->query_safe(q{BEGIN; INSERT INTO t VALUES (-2, 'running')});
sql(q{SELECT dolt_commit('-am', 'capture again')});
$running->query_safe('COMMIT');
$running->quit;
is(sql(q{SELECT v FROM t WHERE id IN (-2, 1) ORDER BY id}), "running\nagain", 'a writer running at the capture is not lost');
sql(q{UPDATE t SET v = 'staged' WHERE id = 2});
sql(q{BEGIN; SELECT dolt_add('.'); INSERT INTO t VALUES (-3, 'after'); COMMIT});
is(sql(q{SET enable_seqscan = off; SELECT v FROM t WHERE v IN ('after', 'running', 'staged') ORDER BY v}), "after\nrunning\nstaged",
	"nor are the capturing transaction's own later writes");
sql('SELECT dolt_fold()');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM t WHERE v IN ('after', 'running', 'staged', 'again')}), '4', 'which a fold writes later');
$expected = sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t});
$node->wait_for_catchup($standby);
is($standby->safe_psql('postgres', q{SELECT count(*), sum(id), sum(length(v)) FROM t}), $expected,
	'and the standby reads what the primary does');
# An install rolls back with its savepoint, leaving the old base.
sql(q{UPDATE t SET v = 'rolled back install' WHERE id = 4});
$old_horizons = sql($horizons);
sql(q{BEGIN; SAVEPOINT s; SELECT dolt_add('.'); ROLLBACK TO s; COMMIT});
is(sql($horizons), $old_horizons, 'an install rolls back with its savepoint');
is(sql(q{SET enable_seqscan = off; SELECT v FROM t WHERE id = 4}), 'rolled back install', 'and the change it held still reads');
# A fold applies its delta a batch of keys at a time, writing and dropping each
# batch's nodes as it goes: it makes the tree a fold in one batch makes,
# holding a small part of it at once.
sql(q{CREATE TABLE batched(id int PRIMARY KEY, v text);
CREATE TABLE whole(id int PRIMARY KEY, v text);
INSERT INTO batched SELECT g, repeat(md5(g::text), 2) FROM generate_series(1, 40000) g;
INSERT INTO whole SELECT g, repeat(md5(g::text), 2) FROM generate_series(1, 40000) g;
UPDATE batched SET v = 'changed' WHERE id % 7 = 0;
UPDATE whole SET v = 'changed' WHERE id % 7 = 0});
my $tree_nodes = sql(q{SELECT count(*) FROM dolt_chunk_list()});
my $offset = -s $node->logfile;
sql(q{SET dolt_fold_batch = 100; SET client_min_messages = debug1; SET log_min_messages = debug1; SELECT dolt_fold()});
my @held = map { /held at most (\d+) nodes/ ? $1 : () } split /\n/, slurp_file($node->logfile, $offset);
my $roots = q{SELECT count(DISTINCT r.rootcontent) FROM pg_dolt_root r JOIN pg_class c ON c.relfilenode = r.rootfile WHERE c.relname IN ('batched', 'whole')};
$tree_nodes = sql(q{SELECT count(*) FROM dolt_chunk_list()}) - $tree_nodes;
cmp_ok(scalar(@held), '>=', 2, 'folds log what they held');
my ($most) = sort { $b <=> $a } @held;
cmp_ok($most, '<', $tree_nodes / 10, "a fold of 40,000 rows in batches of 100 holds at most $most nodes, of $tree_nodes written");
is(sql(q{SELECT count(*), sum(length(v)) FROM batched}), sql(q{SELECT count(*), sum(length(v)) FROM whole}), 'and reads as the other table');
sql(q{UPDATE batched SET v = 'again' WHERE id % 11 = 0; UPDATE whole SET v = 'again' WHERE id % 11 = 0});
sql(q{SET dolt_fold_batch = 7; SELECT dolt_fold()});
is(sql(q{SELECT count(*) > 0 FROM dolt_chunk_list()}), 't', 'folds in tiny batches finish');
is(sql(q{SELECT count(*) FILTER (WHERE v = 'again'), count(*) FILTER (WHERE v = 'changed') FROM batched}), '3636|5195', 'with every change applied');
is(sql(q{SELECT (SELECT string_agg(md5(v), '' ORDER BY id) FROM batched) = (SELECT string_agg(md5(v), '' ORDER BY id) FROM whole)}), 't',
	'the same rows, row for row');

# A commit's capture applies a table's delta the same way.
sql(q{UPDATE batched SET v = 'committed' WHERE id % 3 = 0});
$offset = -s $node->logfile;
sql(q{SET dolt_fold_batch = 100; SET log_min_messages = debug1; SELECT dolt_commit('-Am', 'batched capture')});
@held = map { /held at most (\d+) nodes/ ? $1 : () } split /\n/, slurp_file($node->logfile, $offset);
cmp_ok(scalar(@held), '>=', 1, 'a commit folds what it captures');
($most) = sort { $b <=> $a } @held;
cmp_ok($most, '<', $tree_nodes / 10, "in batches, holding at most $most nodes");
is(sql(q{SELECT count(*) FROM batched AS OF 'HEAD' WHERE v = 'committed'}), '13333', 'and the commit holds every change');
sql(q{UPDATE t SET v = 'crashed' WHERE id = 3});
sql(q{SELECT dolt_commit('-am', 'before a crash')});
$expected = sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t}), $expected, 'recovery replays an installed base');
$standby->stop;

# With dolt_unlogged_folds, a fold logs none of the chunks it writes. They are
# durable once the fold commits, so a crash right after it loses none.
sql(q{CREATE TABLE unlogged_fold(id int PRIMARY KEY, v text);
INSERT INTO unlogged_fold SELECT g, repeat(md5(g::text), 2) FROM generate_series(1, 20000) g});
sql('SELECT dolt_fold()');
sub fold_wal
{
	my ($value) = @_;
	sql(qq{UPDATE unlogged_fold SET v = '$value' WHERE id % 5 = 0});
	my $chunks = sql(q{SELECT count(*) FROM dolt_chunk_list()});
	my $wal = sql(q{SELECT pg_current_wal_insert_lsn()});
	sql('SELECT dolt_fold()');
	return (sql(qq{SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$wal')}),
		sql(q{SELECT count(*) FROM dolt_chunk_list()}) - $chunks);
}
my ($logged_wal, $logged_chunks) = fold_wal('logged fold');
cmp_ok($logged_wal, '>', $logged_chunks * 1000, "a logged fold logs its $logged_chunks chunks ($logged_wal bytes of WAL)");
sql(q{ALTER SYSTEM SET dolt_unlogged_folds = on; SELECT pg_reload_conf()});
my ($unlogged_wal, $unlogged_chunks) = fold_wal('unlogged fold');
cmp_ok($unlogged_chunks, '>', 100, "an unlogged fold writes $unlogged_chunks chunks");
cmp_ok($unlogged_wal, '<', $logged_wal / 20, "and logs none of them ($unlogged_wal bytes of WAL)");
$expected = sql(q{SELECT count(*), sum(length(v)) FILTER (WHERE v = 'unlogged fold') FROM unlogged_fold});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*), sum(length(v)) FILTER (WHERE v = 'unlogged fold') FROM unlogged_fold}), $expected,
	'a crash after an unlogged fold loses none of its tree');
sql(q{ALTER SYSTEM RESET dolt_unlogged_folds; SELECT pg_reload_conf()});

$node->stop;

# With the launcher and workers running, versions fold and are pruned on
# their own, and a writer whose changes fill the store waits for them.
my $busy = PostgreSQL::Test::Cluster->new('delta_busy');
$busy->init(extra => ['--encoding=UTF8', '--locale=C']);
$busy->append_conf('postgresql.conf', qq{default_table_access_method='dolt_prolly_table'
dolt_delta_memory=2MB
dolt_fold_versions=500
dolt_fold_timeout=200
});
$busy->start;
$busy->safe_psql('postgres', q{CREATE TABLE b(id int PRIMARY KEY, v int)});
for my $i (1 .. 40) {
	$busy->safe_psql('postgres', "INSERT INTO b SELECT g, g FROM generate_series($i * 500 - 499, $i * 500) g");
}
is($busy->safe_psql('postgres', q{SELECT count(*), sum(id) FROM b}), '20000|200010000',
	'writes past the delta memory wait for folds and finish');
ok($busy->poll_query_until('postgres', q{SELECT versions = 0 FROM dolt_delta_stats()}),
	'and every version folds and is pruned');
is($busy->safe_psql('postgres', q{SELECT count(*), sum(id) FROM b}), '20000|200010000', 'leaving the rows as they were');

# The workers prune by a commit's installs as by folds, but not by one rolled
# back with its savepoint: they keep the versions the old base lacks.
$busy->safe_psql('postgres', q{UPDATE b SET v = -1 WHERE id = 1});
$busy->safe_psql('postgres', q{BEGIN; SAVEPOINT s; SELECT dolt_add('.'); ROLLBACK TO s; COMMIT});
ok($busy->poll_query_until('postgres', q{SELECT versions = 0 FROM dolt_delta_stats()}),
	'the workers fold and prune after a rolled back install');
is($busy->safe_psql('postgres', q{SET enable_seqscan = off; SELECT v FROM b WHERE id = 1}), '-1',
	'without losing the change it held');
$busy->stop;

done_testing();
