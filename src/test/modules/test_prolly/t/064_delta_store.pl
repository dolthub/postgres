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
sql(q{UPDATE t SET v = 'crashed' WHERE id = 3});
sql(q{SELECT dolt_commit('-am', 'before a crash')});
$expected = sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t});
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t}), $expected, 'recovery replays an installed base');

$standby->stop;
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
