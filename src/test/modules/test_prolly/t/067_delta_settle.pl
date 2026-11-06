# Settling: a hot row updated over and over leaves dead keys in the delta, an
# insert and a delete of each old version that cancel. Once every snapshot
# sees them and no base holds them, the fold launcher drops them without a
# fold, and collapses other keys' versions to one. Settling is logged, so a
# standby and crash recovery agree, and a snapshot that still needs the old
# versions keeps them.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('settle');
$node->init(allows_streaming => 1, extra => ['--encoding=UTF8', '--locale=C']);

# No folds: only settling shrinks the delta.
$node->append_conf('postgresql.conf', qq{default_table_access_method='dolt_prolly_table'
dolt_fold_versions=100000000
dolt_fold_timeout=100000000
});
$node->start;
sub sql { return $node->safe_psql('postgres', shift) }
sql(q{CREATE TABLE hot(id int PRIMARY KEY, n int, pad text); CREATE INDEX hot_n ON hot(n);
INSERT INTO hot SELECT g, 0, repeat('p', 50) FROM generate_series(1, 10) g});
sql('SELECT dolt_fold()');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('settle_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

# A snapshot from before the updates needs their old versions.
my $old = $node->background_psql('postgres');
$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($old->query_safe('SELECT sum(n) FROM hot'), '0', 'an old snapshot reads the rows before the updates');
for my $round (1 .. 20)
{
	sql(join('', map { "UPDATE hot SET n = n + 1 WHERE id = 1;" } 1 .. 50));
}
my $versions = q{SELECT versions FROM dolt_delta_stats()};
sleep 1;
cmp_ok(sql($versions), '>=', 2000, 'while it runs, the dead keys stay (' . sql($versions) . ' versions)');
is($old->query_safe('SELECT sum(n) FROM hot'), '0', 'and it still reads them');
$old->query_safe('COMMIT');
$old->quit;

ok($node->poll_query_until('postgres', "SELECT versions < 20 FROM dolt_delta_stats()"),
	'once no snapshot needs them, the launcher settles the dead keys without a fold');
is(sql('SELECT n FROM hot WHERE id = 1'), '1000', 'the row reads its last value');
is(sql(q{SET enable_seqscan = off; SELECT id FROM hot WHERE n = 1000}), '1', 'and so does its index');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM hot WHERE n < 1000}), '9', 'which holds no stale entries');

# Settling commits nothing, so its records reach the standby once flushed.
sql('CHECKPOINT');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT n FROM hot WHERE id = 1'), '1000', 'a standby replays the settling');
cmp_ok($standby->safe_psql('postgres', $versions), '<', 20, 'and holds as few versions');
is($standby->safe_psql('postgres', q{SET enable_seqscan = off; SELECT count(*) FROM hot WHERE n < 1000}), '9', 'and index entries');

$node->stop('immediate');
$node->start;
is(sql('SELECT n, (SELECT count(*) FROM hot) FROM hot WHERE id = 1'), '1000|10', 'crash recovery replays the settling too');
sql('SELECT dolt_fold()');
is(sql(q{SELECT n FROM hot WHERE id = 1}), '1000', 'and a fold afterwards writes the same rows');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM hot WHERE n < 1000}), '9', 'and index entries');

$standby->stop;
$node->stop;
done_testing();
