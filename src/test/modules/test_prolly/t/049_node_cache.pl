# Prolly nodes are cached in shared memory for every session: a session reads
# from the chunk store only the nodes no session has read or written since the
# server started. Writes go to the delta store, and a fold writes the nodes of
# the trees they make, so the test folds before it measures trees. Results stay right when the cache is too small to hold a table, or holds
# nothing at all, and history imports leave the subtrees the chunk store holds
# to be read through the cache. dolt_chunk_stats() counts the chunks a session
# reads from the store.
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

# Fold every pending change into the trees now, as the background workers would.
sub fold { sql('SELECT dolt_fold()') }

# The chunk store reads and cache statistics of |query|, run in a new session.
sub reads {
	my ($query) = @_;
	my @lines = split /\n/, sql("BEGIN; $query; SELECT (SELECT session_reads FROM dolt_chunk_stats()) || ' ' || hits || ' ' || misses FROM dolt_node_cache_stats(); COMMIT");
	return split / /, $lines[-1];
}

sql(q{CREATE TABLE t(id int PRIMARY KEY, v text);
INSERT INTO t SELECT g, repeat(md5(g::text), 3) FROM generate_series(1, 20000) g;
SELECT dolt_commit('-Am', 'base')});
fold();
my $count = 'SELECT count(*), sum(length(v)) FROM t';
my $expected = sql($count);

# A committed fold shares the nodes it wrote.
my ($store, $hits, $misses) = reads($count);
is($store, 0, 'a new session reads nothing from the chunk store after the fold commits');
cmp_ok($hits, '>', 100, "the scan reads from the shared cache ($hits nodes)");
is($misses, 0, 'and finds every node there');

# After a restart, the first session reads the table from the store, and the next none of it.
$node->restart;
($store, $hits, $misses) = reads($count);
cmp_ok($store, '>', 100, "after a restart, the first session reads the table from the chunk store ($store chunks)");
is($misses, $store, 'one store read per node missing from the cache');
my $first_misses = $misses;
($store, $hits, $misses) = reads($count);
is($store, 0, 'the next session reads nothing from the chunk store');
is($misses, 0, 'and finds every node in the cache');
my ($total_hits, $total_misses) = split /\|/, sql('SELECT total_hits, total_misses FROM dolt_node_cache_stats()');
cmp_ok($total_hits, '>=', $hits, "the server counts every session's hits ($total_hits)");
cmp_ok($total_misses, '>=', $first_misses, "and misses ($total_misses)");
is(sql($count), $expected, 'results are unchanged');
my ($entries, $bytes) = split /\|/, sql('SELECT entries, bytes FROM dolt_node_cache_stats()');
cmp_ok($entries, '>=', $hits, "the cache holds the table ($entries nodes, $bytes bytes)");

# A session keeps the nodes it parsed across committed transactions, up to
# dolt_session_node_cache, so it repeats no shared cache lookups; past that,
# or after an abort, it drops them.
sub session_lookups {
	my ($settings, $between) = @_;
	my @lookups = map { /^lookups (\d+)$/ ? $1 : () } split /\n/,
	  sql("$settings BEGIN; $count; COMMIT; SELECT 'lookups ' || hits + misses FROM dolt_node_cache_stats();
 $between BEGIN; $count; COMMIT; SELECT 'lookups ' || hits + misses FROM dolt_node_cache_stats()");
	return $lookups[1] - $lookups[0];
}
is(session_lookups('', ''), 0, 'a later transaction of the session finds its nodes parsed');
cmp_ok(session_lookups('SET dolt_session_node_cache = 0;', ''), '>', 100, 'with no session budget it looks them up again');
cmp_ok(session_lookups('', 'BEGIN; SELECT count(*) FROM t; ROLLBACK;'), '>', 100, 'as it does after an abort');

# Updates read the trees through the cache too, and their own rows from the delta store.
sql(q{UPDATE t SET v = 'changed' WHERE id % 1000 = 0});
($store) = reads(q{SELECT count(*) FROM t WHERE v = 'changed'});
is($store, 0, "a reader of an update finds the trees in the cache");
is(sql(q{SELECT count(*) FROM t WHERE v = 'changed'}), 20, 'and sees the update');
sql(q{SELECT dolt_commit('-Am', 'changed')});
$expected = sql($count);

# Writers store no chunks, and a fold stores only the trees it ends with:
# rewriting rows twenty times stores about what rewriting them once does.
my $chunks = 'SELECT count(*) FROM dolt_chunk_list()';
fold();
my $start = sql($chunks);
sql(q{UPDATE t SET v = 'once' WHERE id % 500 = 0});
is(sql($chunks) - $start, 0, 'a writer stores no chunks');
fold();
my $once = sql($chunks) - $start;
$start = sql($chunks);
sql(join('', 'BEGIN;', (map { "UPDATE t SET v = 'pass $_' WHERE id % 500 = 0;" } 1 .. 20), 'COMMIT'));
fold();
my $twenty = sql($chunks) - $start;
cmp_ok($twenty, '<', 2 * $once + 10, "twenty rewrites in a transaction store $twenty chunks, one stores $once");
is(sql(q{SELECT count(*) FROM t WHERE v = 'pass 20'}), 40, 'and the last rewrite is what is read');
sql(q{SELECT dolt_commit('-Am', 'rewritten')});
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
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD~2', 'HEAD', 't')}), 41, 'a diff across two commits');
$node->restart;
my ($rc, $out, $err) = $node->psql('postgres', q{SELECT count(*) FROM dolt_diff('HEAD~2', 'HEAD', 't')});
is("$out|$err", '41|', 'a new session diffing nodes missing from the cache leaks nothing');
sql(q{SELECT dolt_branch('old', 'HEAD~3')});
is(sql(q{SELECT dolt_checkout('old'); SELECT count(*), sum(length(v)) FROM t}),
	qq{(0,"Switched to branch 'old'")\n20000|1920000}, 'an old branch reads its own table');
$expected = sql($count);

# dolt_fold_cache decides what a fold leaves in the cache: by default
# (replace) the nodes it writes, demoting the nodes of the base it reads, which
# those replace; with add, both; with cold, both, but unused; with none,
# nothing, so readers of the new trees read them from the chunk store.
my $added = q{SELECT added FROM dolt_node_cache_stats()};
for my $mode ('replace', 'add', 'cold', 'none')
{
	sql(qq{UPDATE t SET v = 'fold $mode' WHERE id % 400 = 0});
	my $before = sql($added);
	sql(qq{SET dolt_fold_cache = $mode; SELECT dolt_fold()});
	my $fold_added = sql($added) - $before;
	if ($mode eq 'none')
	{
		is($fold_added, 0, 'a fold with dolt_fold_cache = none adds nothing to the cache');
		my ($from_store) = reads(qq{SELECT count(*) FROM t WHERE v = 'fold $mode'});
		cmp_ok($from_store, '>', 0, "and a reader of its tree reads the new nodes from the chunk store ($from_store)");
	}
	else
	{
		cmp_ok($fold_added, '>', 0, "a fold with dolt_fold_cache = $mode adds the nodes it writes ($fold_added)");
	}
	is(sql(qq{SELECT count(*) FROM t WHERE v = 'fold $mode'}), 50, "and its tree reads right ($mode)");
}

$expected = sql($count);

# A cache too small for the table evicts as it goes, and stays within its size.
fold();
$node->append_conf('postgresql.conf', "dolt_node_cache_size=1MB\n");
$node->restart;
is(sql(q{SELECT count(*), sum(length(v)) FROM t}), $expected, 'a scan through a 1 MB cache is right');
my ($evictions);
($evictions, $bytes) = split /\|/, sql('SELECT evictions, bytes FROM dolt_node_cache_stats()');
cmp_ok($evictions, '>', 1000, "a scan larger than the cache evicts as it goes ($evictions)");
cmp_ok($bytes, '<=', 1024 * 1024, "the cache stays within its size ($bytes bytes)");
sql(q{DO $$ BEGIN FOR i IN 1..20 LOOP PERFORM v FROM t WHERE id = i * 997; END LOOP; END $$});
for my $i (1 .. 30) {
	sql("SELECT v FROM t WHERE id = " . ($i * 613));
}
my ($after) = split /\|/, sql('SELECT evictions FROM dolt_node_cache_stats()');
cmp_ok($after, '>', $evictions, 'lookups across the table evict too (' . ($after - $evictions) . ')');
is(sql(q{SELECT count(*), sum(length(v)) FROM t}), $expected, 'and results stay right');

# With no cache, every node is read from the store, privately.
fold();
$node->append_conf('postgresql.conf', "dolt_node_cache_size=0\n");
$node->restart;
is(sql(q{SELECT count(*), sum(length(v)) FROM t}), $expected, 'without a cache, results are the same');
($store, $hits, $misses) = reads($count);
cmp_ok($store, '>', 100, 'and every session reads the table from the chunk store');
is("$hits $misses", '0 0', 'consulting no cache');

$node->stop;
done_testing();
