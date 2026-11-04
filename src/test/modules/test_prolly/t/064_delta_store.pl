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
INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 2000) g});
my $expected = sql(q{SELECT count(*), sum(id), sum(length(v)) FROM t});
my ($versions) = split /\|/, (sql('SELECT versions, trees FROM dolt_delta_stats()'));
cmp_ok($versions, '>', 4000, "the rows are versions in the delta store ($versions)");

# A checkpoint saves the versions; later ones come back from the WAL.
sql('CHECKPOINT');
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
$busy->stop;

done_testing();
