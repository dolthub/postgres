# A branch's tables keep planner statistics under their durable OIDs: ANALYZE,
# database-wide ANALYZE and autovacuum collect them per branch, schema changes
# and branch deletion remove them, and the planner uses them.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_statistics');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', qq{default_table_access_method='dolt_prolly_table'
autovacuum = off
});
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }

# The result of |query| on branch |branch|, after the checkout's own result.
sub on_branch
{
	my ($branch, $query) = @_;
	my $result = sql("SELECT dolt_checkout('$branch'); $query");
	$result =~ s/^[^\n]*\n?//;
	return $result;
}

# The planner's row estimate for a query.
sub estimate
{
	my ($prefix, $query) = @_;
	my $plan = sql("$prefix EXPLAIN $query");
	$plan =~ /rows=(\d+)/ or die "no estimate in $plan";
	return $1;
}

sql(q{CREATE TABLE t(id int PRIMARY KEY, k int, s text);
 INSERT INTO t SELECT i, i % 10, CASE WHEN i % 100 = 0 THEN 'rare' ELSE 'common' END FROM generate_series(1, 20000) i;
 CREATE TABLE u(id int PRIMARY KEY, v int);
 INSERT INTO u SELECT i, i FROM generate_series(1, 100) i});
my $main_t = sql(q{SELECT 't'::regclass::oid});
is(sql(q{SELECT count(*) FROM pg_stats WHERE tablename = 't'}), '0', 'a new table has no statistics');
my $default = estimate('', q{SELECT * FROM t WHERE k = 3});

is(estimate('', q{SELECT * FROM u}), 100, 'small tables retain their exact cardinality');
my $before_analyze = sql(q{SELECT dolt_hashof_db('WORKING')});
sql(q{ANALYZE t});
is(sql(q{SELECT dolt_hashof_db('WORKING')}), $before_analyze, 'ANALYZE metadata does not change canonical content');
is(sql(q{SELECT sum(row_count)::int, sum(distinct_count)::int, max(created_at) IS NOT NULL FROM dolt_statistics WHERE table_name='t' AND index_name='primary'}), '20000|20000|t', 'Dolt statistics exposes the analyzed branch row count');
is(sql(q{SELECT count(*) FROM dolt_statistics WHERE table_name='u'}), '0', 'unanalyzed tables have no histogram');
is(sql(q{SELECT string_agg(attname, ',' ORDER BY attname) FROM pg_stats WHERE tablename = 't'}), 'id,k,s',
	'ANALYZE collects statistics for every column');
is(sql(q{SELECT n_distinct FROM pg_stats WHERE tablename = 't' AND attname = 'k'}), '10', 'the distinct count is exact');
is(sql(q{SELECT count(*) FROM pg_statistic WHERE starelid = 't'::regclass}), '3', 'statistics are keyed by the durable OID');
my $k = estimate('', q{SELECT * FROM t WHERE k = 3});
ok($k > 1500 && $k < 2500 && $k != $default, "the planner uses the statistics ($default rows before, $k after)");
my $rare = estimate('', q{SELECT * FROM t WHERE s = 'rare'});
ok($rare > 100 && $rare < 300, "a rare value is estimated from its frequency ($rare rows)");

# Rows past the histogram's end are found by probing the index.
sql(q{INSERT INTO t SELECT i, 0, 'new' FROM generate_series(20001, 30000) i});
is(sql(q{SELECT sum(row_count)::int FROM dolt_statistics WHERE table_name='t'}), '20000', 'Dolt statistics preserves the analyzed count until refresh');
my $past = estimate('', q{SELECT * FROM t WHERE id > 25000});
ok($past > 1, "a range past the histogram is estimated from the index ($past rows)");

# Database-wide ANALYZE includes the branch's tables.
sql(q{DELETE FROM pg_statistic WHERE starelid IN ('t'::regclass, 'u'::regclass)});
sql(q{ANALYZE});
is(sql(q{SELECT count(*) FROM pg_statistic WHERE starelid IN ('t'::regclass, 'u'::regclass)}), '5',
	'database-wide ANALYZE includes the branch tables');
sql(q{VACUUM ANALYZE t});
is(sql(q{SELECT count(*) FROM pg_statistic WHERE starelid = 't'::regclass}), '3', 'VACUUM ANALYZE collects statistics');

# Another branch's table has its own OID and statistics.
my $commit = sql(q{SELECT dolt_commit('-Am', 'tables'); SELECT dolt_branch('feature')});
($commit) = $commit =~ /([0-9a-v]{32})/;
is(sql(qq{SELECT count(*) FROM t AS OF '$commit' WHERE k = 3}), '2000', 'a revision of an analyzed table is planned without its statistics');
my $feature_t = on_branch('feature', q{SELECT 't'::regclass::oid});
isnt($feature_t, $main_t, 'the table has another OID on another branch');
is(on_branch('feature', q{SELECT count(*) FROM dolt_statistics WHERE table_name='t'}), '0', 'copied branch does not inherit ANALYZE metadata');
is(sql(qq{SELECT count(*) FROM pg_statistic WHERE starelid = $feature_t}), '0', 'a new branch starts without statistics');
on_branch('feature', q{UPDATE t SET k = 77 WHERE id <= 15000; ANALYZE t});
is(on_branch('feature', q{SELECT sum(row_count)::int FROM dolt_statistics WHERE table_name='t'}), '30000', 'branch ANALYZE metadata is visible');
is(on_branch('feature', q{SELECT (most_common_vals::text::int[])[1] FROM pg_stats WHERE tablename = 't' AND attname = 'k'}), '77',
	'the branch has statistics of its own rows');
is(sql(q{SELECT 77 = ANY (most_common_vals::text::int[]) FROM pg_stats WHERE tablename = 't' AND attname = 'k'}), 'f',
	'the original branch keeps its own');

# A schema change removes the changed table's statistics only.
sql(q{ALTER TABLE t ALTER COLUMN k TYPE bigint});
is(sql(qq{SELECT count(*) FROM pg_statistic WHERE starelid = $main_t}), '0', 'changing a table schema removes its statistics');
is(sql(q{SELECT count(*) FROM dolt_statistics WHERE table_name='t'}), '0', 'schema change removes histogram metadata too');
is(sql(q{SELECT count(*) FROM pg_statistic WHERE starelid = 'u'::regclass}), '2', 'other tables keep theirs');
is(sql(qq{SELECT count(*) FROM pg_statistic WHERE starelid = $feature_t}), '3', 'and so does the table on another branch');
sql(q{ANALYZE t});
is(sql(q{SELECT count(*) FROM pg_statistic WHERE starelid = 't'::regclass}), '3', 'the changed table can be analyzed again');

sql(q{BEGIN; DELETE FROM t; ANALYZE t; ROLLBACK});
is(sql(q{SELECT sum(row_count)::int FROM dolt_statistics WHERE table_name='t'}), '30000', 'rollback restores analyzed row counts');
$node->restart;
is(sql(q{SELECT sum(row_count)::int FROM dolt_statistics WHERE table_name='t'}), '30000', 'analyzed totals survive restart');
is(sql(qq{SELECT count(*) FROM pg_statistic WHERE starelid IN ($main_t, $feature_t)}), '6', 'statistics survive a restart');

# Dropping a table removes its statistics.
my $u = sql(q{SELECT 'u'::regclass::oid});
sql(q{DROP TABLE u});
is(sql(qq{SELECT count(*) FROM pg_statistic WHERE starelid = $u}), '0', 'dropping a table removes its statistics');

# Deleting a branch removes its tables' statistics.
sql(q{SELECT dolt_branch('-D', 'feature')});
is(sql(qq{SELECT count(*) FROM pg_statistic WHERE starelid = $feature_t}), '0', 'deleting a branch removes its statistics');
is(sql(qq{SELECT count(*) FROM pg_dolt_relation_stats WHERE statrelid=$feature_t}), '0', 'branch deletion removes ANALYZE totals');

# Autovacuum analyzes changed tables on every branch, counting the rows the
# table AM changes.
$node->append_conf('postgresql.conf', qq{autovacuum = on
autovacuum_naptime = 1
});
$node->reload;
sql(q{SELECT dolt_commit('-Am', 'analyzed'); SELECT dolt_branch('auto')});
my $auto_t = on_branch('auto', q{SELECT 't'::regclass::oid});
on_branch('auto', q{UPDATE t SET k = 55 WHERE id <= 20000});
ok($node->poll_query_until('postgres', qq{SELECT pg_stat_get_tuples_updated($auto_t) = 20000}),
	'the table AM counts updated rows');
ok($node->poll_query_until('postgres', qq{SELECT count(*) = 3 FROM pg_statistic WHERE starelid = $auto_t}),
	'autovacuum analyzes a table on another branch');
is(on_branch('auto', q{SELECT (most_common_vals::text::bigint[])[1] FROM pg_stats WHERE tablename = 't' AND attname = 'k'}), '55',
	'with the branch rows');
sql(q{DELETE FROM t WHERE id > 1000});
ok($node->poll_query_until('postgres', q{SELECT n_live_tup = 1000 AND n_mod_since_analyze = 0 FROM pg_stat_user_tables WHERE relname = 't'}),
	'autovacuum analyzes a table with deleted rows');
is(sql(q{SELECT vacuum_count + autovacuum_count FROM pg_stat_user_tables WHERE relname = 't'}), '0', 'but never vacuums it');

done_testing();
