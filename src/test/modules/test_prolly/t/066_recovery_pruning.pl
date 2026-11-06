# Crash recovery replays every version written since the checkpoint, more than
# the delta store's memory holds once folds have written them into bases. It
# prunes what committed folds hold as it goes, whatever the wal_level, so a
# busy server restarts.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $wal_level ('minimal', 'replica')
{
	my $node = PostgreSQL::Test::Cluster->new("recovery_$wal_level");
	$node->init(extra => ['--encoding=UTF8', '--locale=C']);
	$node->append_conf('postgresql.conf', qq{default_table_access_method='dolt_prolly_table'
wal_level=$wal_level
max_wal_senders=0
dolt_delta_memory=4MB
dolt_fold_versions=500
dolt_fold_timeout=100
checkpoint_timeout=1h
max_wal_size=10GB
});
	$node->start;
	my $sql = sub { $node->safe_psql('postgres', shift) };
	$sql->(q{CREATE TABLE t(id int PRIMARY KEY, v text); INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 2000) g});
	$sql->('CHECKPOINT');

	# About 12 MB of versions, three times the memory, folded as they come. A
	# writer that finds the memory full holds back the folds it waits for, so
	# the rounds leave the workers time.
	for my $round (1 .. 30)
	{
		$sql->(qq{UPDATE t SET v = repeat('$round', 100) || id WHERE id % 2 = $round % 2});
		$sql->('SELECT dolt_fold()');
	}
	$sql->('SELECT dolt_fold()');
	my $expected = $sql->(q{SELECT count(*), sum(length(v)), max(v) FROM t});
	$node->stop('immediate');
	ok($node->start(fail_ok => 1), "with wal_level $wal_level, recovery replays more versions than the delta's memory");
	is($sql->(q{SELECT count(*), sum(length(v)), max(v) FROM t}), $expected, 'and reads what was committed');
	$node->stop;
}
done_testing();
