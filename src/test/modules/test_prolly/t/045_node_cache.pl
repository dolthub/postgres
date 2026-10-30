# Prolly nodes are cached in shared memory for every session: a session reads
# from the chunk store only the nodes no session has read or written since the
# server started. Sessions borrow nodes only for the length of a transaction,
# results stay right when the cache is too small to hold a table, or holds
# nothing at all, and history imports leave the subtrees the chunk store holds
# to be read through the cache. Reads of pg_dolt_chunk's hash index count the
# chunks a transaction reads from the store.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node_cache');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\ndolt_node_cache_size=32MB\n");
$node->start;
sub sql { return $node->safe_psql('postgres', shift) }

# The chunk store reads and cache statistics of |query|, run in a new session.
sub reads {
	my ($query) = @_;
	my @lines = split /\n/, sql("BEGIN; $query; SELECT pg_stat_get_xact_numscans(9126) || ' ' || hits || ' ' || misses || ' ' || borrowed FROM dolt_node_cache_stats(); COMMIT");
	return split / /, $lines[-1];
}

sql(q{CREATE TABLE t(id int PRIMARY KEY, v text);
INSERT INTO t SELECT g, repeat(md5(g::text), 3) FROM generate_series(1, 20000) g;
SELECT dolt_commit('-Am', 'base')});
my $count = 'SELECT count(*), sum(length(v)) FROM t';
my $expected = sql($count);

# A committed transaction shares the nodes it wrote.
my ($store, $hits, $misses, $borrowed) = reads($count);
is($store, 0, 'a new session reads nothing from the chunk store after the writer commits');
cmp_ok($hits, '>', 100, "the scan borrows from the shared cache ($hits nodes)");
is($misses, 0, 'and finds every node there');
cmp_ok($borrowed, '>', 0, 'nodes stay borrowed until the transaction ends');
is(sql('SELECT borrowed FROM dolt_node_cache_stats()'), 0, 'a session borrows nothing between transactions');
is(sql('SELECT pinned FROM dolt_node_cache_stats()'), 0, 'no idle session pins the cache');

# After a restart, the first session reads the table from the store, and the next none of it.
$node->restart;
($store, $hits, $misses) = reads($count);
cmp_ok($store, '>', 100, "after a restart, the first session reads the table from the chunk store ($store chunks)");
is($misses, $store, 'one store read per node missing from the cache');
($store, $hits, $misses) = reads($count);
is($store, 0, 'the next session reads nothing from the chunk store');
is($misses, 0, 'and borrows every node');
is(sql($count), $expected, 'results are unchanged');
my ($entries, $bytes) = split /\|/, sql('SELECT entries, bytes FROM dolt_node_cache_stats()');
cmp_ok($entries, '>=', $hits, "the cache holds the table ($entries nodes, $bytes bytes)");

# Updates read through the cache too, and share their new nodes at commit.
sql(q{UPDATE t SET v = 'changed' WHERE id % 1000 = 0});
($store) = reads(q{SELECT count(*) FROM t WHERE v = 'changed'});
is($store, 0, "a reader finds an update's nodes in the cache");
is(sql(q{SELECT count(*) FROM t WHERE v = 'changed'}), 20, 'and sees the update');
sql(q{SELECT dolt_commit('-Am', 'changed')});
$expected = sql($count);

# An aborted writer shares nothing, and the same rows written again are stored.
sql(q{CREATE TABLE u(id int PRIMARY KEY, v int)});
is(sql(q{BEGIN; INSERT INTO u SELECT g, 1 FROM generate_series(1, 5000) g; ROLLBACK;
	INSERT INTO u SELECT g, 1 FROM generate_series(1, 5000) g; SELECT count(*), sum(id) FROM u}),
	'5000|12502500', 'rows inserted again after a rollback are read');
$node->restart;
is(sql('SELECT count(*), sum(id) FROM u'), '5000|12502500', 'and read from the chunk store after a restart');

# History: a diff imports only the nodes of the commits' trees that the chunk
# store lacks, rather than each tree's every node.
sql(q{UPDATE t SET v = 'later' WHERE id = 7});
sql(q{SELECT dolt_commit('-Am', 'later')});
my $diff = q{SELECT diff_type, from_v, to_v FROM dolt_diff('HEAD~1', 'HEAD', 't')};
is(sql($diff), 'modified|' . sql('SELECT repeat(md5(7::text), 3)') . '|later', 'a diff of one changed row');
my @lines = split /\n/, sql("BEGIN; $diff; SELECT pg_stat_get_xact_numscans(9103); COMMIT");
cmp_ok($lines[-1], '<', 1000, "the diff reads a fraction of the trees' history objects ($lines[-1])");
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD~2', 'HEAD', 't')}), 21, 'a diff across two commits');
sql(q{SELECT dolt_branch('old', 'HEAD~2')});
is(sql(q{SELECT dolt_checkout('old'); SELECT count(*), sum(length(v)) FROM t}),
	qq{(0,"Switched to branch 'old'")\n20000|1920000}, 'an old branch reads its own table');
$expected = sql($count);

# A cache too small for the table evicts as it goes; one too small for a
# transaction's working set refuses nodes, and the store keeps private copies.
$node->append_conf('postgresql.conf', "dolt_node_cache_size=1MB\n");
$node->restart;
is(sql(q{SELECT count(*), sum(length(v)) FROM t}), $expected, 'a scan through a 1 MB cache is right');
my ($evictions, $rejected, $pinned);
($evictions, $rejected, $bytes, $pinned) = split /\|/, sql('SELECT evictions, rejected, bytes, pinned FROM dolt_node_cache_stats()');
cmp_ok($rejected, '>', 0, "a scan pinning the whole cache has nodes refused ($rejected)");
cmp_ok($bytes, '<=', 1024 * 1024, "the cache stays within its size ($bytes bytes)");
is($pinned, 0, 'and is unpinned when the scan ends');
sql(q{DO $$ BEGIN FOR i IN 1..20 LOOP PERFORM v FROM t WHERE id = i * 997; END LOOP; END $$});
for my $i (1 .. 30) {
	sql("SELECT v FROM t WHERE id = " . ($i * 613));
}
($evictions) = split /\|/, sql('SELECT evictions FROM dolt_node_cache_stats()');
cmp_ok($evictions, '>', 0, "lookups across the table evict ($evictions)");
is(sql(q{SELECT count(*), sum(length(v)) FROM t}), $expected, 'and results stay right');

# With no cache, every node is read from the store, privately.
$node->append_conf('postgresql.conf', "dolt_node_cache_size=0\n");
$node->restart;
is(sql(q{SELECT count(*), sum(length(v)) FROM t}), $expected, 'without a cache, results are the same');
($store, $hits, $misses) = reads($count);
cmp_ok($store, '>', 100, 'and every session reads the table from the chunk store');
is("$hits $misses", '0 0', 'consulting no cache');

$node->stop;
done_testing();
