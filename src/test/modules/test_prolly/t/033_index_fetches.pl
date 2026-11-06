# Rows an index scan found reach the table fetch through the row stash, index
# entries satisfy index-only scans without a visibility check, and unique-key
# reservations live in shared memory. Each must give the same answers as
# before, as rows change, across transactions, sessions and branches.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('fetches');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nmax_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub idx { sql("SET enable_seqscan=off; SET enable_bitmapscan=off; $_[0]"); }
sub bitmap { sql("SET enable_seqscan=off; SET enable_indexscan=off; SET enable_indexonlyscan=off; $_[0]"); }
sub plan_of { my $p = sql("EXPLAIN (COSTS OFF) $_[0]"); $p =~ /(Index Only Scan|Index Scan|Bitmap Heap Scan|Seq Scan)/ ? $1 : $p; }
sql(q{CREATE TABLE t(id int PRIMARY KEY, k int NOT NULL, v text); CREATE INDEX t_k ON t(k);
 INSERT INTO t SELECT g, g % 50, 'v'||g FROM generate_series(1, 3000) g});

# Index and bitmap fetches agree with a sequential scan, before and after changes.
my $seq = sql(q{SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t WHERE k = 7});
is(idx(q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t WHERE k = 7}), $seq, 'index scan fetches every row');
is(bitmap(q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t WHERE k = 7}), $seq, 'bitmap scan fetches every row');
sql(q{UPDATE t SET v = v || '!' WHERE k = 7 AND id < 1000; DELETE FROM t WHERE k = 7 AND id > 2500});
$seq = sql(q{SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t WHERE k = 7});
is(idx(q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t WHERE k = 7}), $seq, 'index scan sees updates and deletes');
is(bitmap(q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t WHERE k = 7}), $seq, 'bitmap scan sees updates and deletes');
like($seq, qr/^7:v7!,/, 'the updated rows are in the answer');
# An index entry holds its key and the row's digest, which names the row in
# the table, not the row: wide rows reach the WAL once, however many indexes
# they have, and index scans read them from the table.
sql(q{CREATE TABLE wide_rows(id int PRIMARY KEY, k int, v text); CREATE INDEX wide_rows_k ON wide_rows(k)});
my $lsn = sql('SELECT pg_current_wal_insert_lsn()');
sql(q{INSERT INTO wide_rows SELECT g, g % 10, (SELECT string_agg(md5(g::text || i::text), '') FROM generate_series(1, 125) i) FROM generate_series(1, 200) g});
my $wal = sql("SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$lsn')");
cmp_ok($wal, '<', 200 * 4000 * 1.5, "200 rows of 4,000 bytes under two indexes log $wal bytes of WAL");
is(idx(q{SELECT count(*), sum(length(v)) FROM wide_rows WHERE k = 3}), '20|80000', 'an index scan reads the rows from the table');
is(bitmap(q{SELECT count(*), sum(length(v)) FROM wide_rows WHERE k = 3}), '20|80000', 'and so does a bitmap scan');
sql('SELECT dolt_fold()');
is(idx(q{SELECT count(*), sum(length(v)) FROM wide_rows WHERE k = 3}), '20|80000', 'and after a fold');
is(idx(q{BEGIN; UPDATE t SET v = 'pending' WHERE id = 57; SELECT v FROM t WHERE k = 7 AND id = 57; ROLLBACK}), 'pending', 'index fetch in the writing transaction sees its pending version');
is(idx(q{SELECT v FROM t WHERE k = 7 AND id = 57}), 'v57!', 'and the rolled back version is gone');

# A nested loop fetches the same rows again and again, across changes.
is(sql(q{SET enable_hashjoin=off; SET enable_mergejoin=off; SET enable_seqscan=off; SELECT count(*), sum(b.id) FROM t a JOIN t b ON b.k = a.k WHERE a.id <= 50}), sql(q{SELECT count(*), sum(b.id) FROM t a JOIN t b ON b.k = a.k WHERE a.id <= 50}), 'index nested loop join matches');
sql(q{UPDATE t SET k = k + 100 WHERE id <= 50});
is(sql(q{SET enable_hashjoin=off; SET enable_mergejoin=off; SET enable_seqscan=off; SELECT count(*), sum(b.id) FROM t a JOIN t b ON b.k = a.k WHERE a.id <= 50}), '50|1275', 'and after the join keys change');

# Scans read entries as they are fetched, in either direction, and can step
# back over what they returned or restore a mark, as cursors and merge joins do.
is(idx(q{SELECT string_agg(id::text, ',') FROM (SELECT id FROM t WHERE id BETWEEN 100 AND 110 ORDER BY id DESC) s}),
	'110,109,108,107,106,105,104,103,102,101,100', 'a bounded backward scan');
is(idx(q{SELECT string_agg(id::text, ',') FROM (SELECT id FROM t ORDER BY id DESC LIMIT 3) s}), '3000,2999,2998', 'a backward scan from the end');
is(idx(q{BEGIN; DECLARE c SCROLL CURSOR FOR SELECT id FROM t WHERE id BETWEEN 10 AND 20 ORDER BY id;
 FETCH 3 FROM c; FETCH BACKWARD 2 FROM c; FETCH LAST FROM c; FETCH PRIOR FROM c; FETCH ABSOLUTE 5 FROM c;
 MOVE LAST FROM c; FETCH NEXT FROM c; FETCH PRIOR FROM c; COMMIT}),
	"10\n11\n12\n11\n10\n20\n19\n14\n20", 'a scroll cursor moves both ways over an index scan');
is(idx(q{BEGIN; DECLARE c SCROLL CURSOR FOR SELECT id FROM t WHERE id BETWEEN 10 AND 20 ORDER BY id DESC;
 FETCH 2 FROM c; FETCH BACKWARD 1 FROM c; FETCH FORWARD 3 FROM c; COMMIT}), "20\n19\n20\n19\n18\n17", 'and over a backward one');
is(sql(q{SET enable_hashjoin=off; SET enable_nestloop=off; SET enable_seqscan=off; SET enable_sort=off;
 SELECT count(*), sum(a.id), sum(b.id) FROM t a JOIN t b ON a.k = b.k WHERE a.id <= 300}),
	sql(q{SET enable_mergejoin=off; SELECT count(*), sum(a.id), sum(b.id) FROM t a JOIN t b ON a.k = b.k WHERE a.id <= 300}),
	'a merge join restoring marks over duplicate keys matches a hash join');

# Fetching a few entries reads a few nodes, not the index: a 50,000-row
# index's first and last entries, as the planner's endpoint probes read them.
sql(q{CREATE TABLE wide(id int PRIMARY KEY, v text); INSERT INTO wide SELECT g, repeat('w', 40) FROM generate_series(1, 50000) g});
sub nodes_read {
	my @lines = split /\n/, idx("BEGIN; $_[0]; SELECT hits + misses FROM dolt_node_cache_stats(); COMMIT");
	return $lines[-1];
}
my $whole = nodes_read('SELECT count(*) FROM wide WHERE id > 0');
cmp_ok(nodes_read('SELECT id FROM wide ORDER BY id LIMIT 1'), '<', 30, "the first entry reads a few nodes (the index and table read $whole)");
cmp_ok(nodes_read('SELECT id FROM wide ORDER BY id DESC LIMIT 1'), '<', 30, 'and so does the last');
cmp_ok(nodes_read('SELECT id FROM wide WHERE id > 25000 ORDER BY id LIMIT 1'), '<', 30, 'and the first above a bound');
is(idx('SELECT min(id), max(id) FROM wide'), '1|50000', 'min and max read through the index');

# Without statistics, an index's cost counts the rows its bounds select: a
# unique index bound on every key column wins over an index sharing only its
# leading columns, as a foreign key check needs, and an index bound on more
# columns wins over one bound on fewer.
sql(q{CREATE TABLE c(w smallint, d smallint, id int, last text, PRIMARY KEY(w, d, id)); CREATE INDEX c_last ON c(w, d, last);
 INSERT INTO c SELECT 1, d, i, 'last' || (i % 100) FROM generate_series(1, 300) i, generate_series(1, 10) d;
 CREATE TABLE o(id int PRIMARY KEY, w smallint, d smallint, cid int, FOREIGN KEY (w, d, cid) REFERENCES c(w, d, id))});
like(sql(q{EXPLAIN (COSTS OFF) SELECT 1 FROM c WHERE w = 1 AND d = 2 AND id = 5}), qr/c_pkey/, 'a fully bound unique index is chosen');
like(sql(q{EXPLAIN (COSTS OFF) SELECT 1 FROM c WHERE w = 1 AND d = 2 AND last = 'last5'}), qr/Index Cond: .*last/, 'the index binding more columns is chosen');
sql(q{INSERT INTO o SELECT i, 1, 1 + i % 10, 1 + i % 300 FROM generate_series(1, 200) i});
is(sql(q{SELECT count(*) FROM o}), '200', 'foreign key checks pass through the unique index');

# Index-only scans are chosen for covering queries and need no heap visit.
is(plan_of(q{SELECT count(k) FROM t WHERE k > 10}), 'Index Only Scan', 'covering query uses an index-only scan');
is(sql(q{SELECT count(k) FROM t WHERE k > 10}), sql(q{SET enable_indexonlyscan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(k) FROM t WHERE k > 10}), 'index-only count matches');
is(sql(q{SELECT sum(k) FROM t WHERE k BETWEEN 20 AND 30}), sql(q{SET enable_indexonlyscan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT sum(k) FROM t WHERE k BETWEEN 20 AND 30}), 'index-only sum matches');
is(sql(q{BEGIN; UPDATE t SET k = 25 WHERE id BETWEEN 2000 AND 2010; DELETE FROM t WHERE k = 21; SELECT count(*), sum(k) FROM t WHERE k BETWEEN 20 AND 30; ROLLBACK}),
   sql(q{BEGIN; UPDATE t SET k = 25 WHERE id BETWEEN 2000 AND 2010; DELETE FROM t WHERE k = 21; SET LOCAL enable_indexonlyscan=off; SET LOCAL enable_indexscan=off; SET LOCAL enable_bitmapscan=off; SELECT count(*), sum(k) FROM t WHERE k BETWEEN 20 AND 30; ROLLBACK}), 'index-only scan sees the transaction\'s own changes');
my $other = $node->background_psql('postgres');
$other->query_safe(q{BEGIN; UPDATE t SET k = 29 WHERE id = 1500; DELETE FROM t WHERE id = 1501});
is(sql(q{SELECT count(*) FROM t WHERE k = 29}), sql(q{SET enable_indexonlyscan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t WHERE k = 29}), 'index-only scan ignores another session\'s uncommitted changes');
$other->query_safe('COMMIT');
is(sql(q{SELECT count(*) FROM t WHERE k = 29}), sql(q{SET enable_indexonlyscan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t WHERE k = 29}), 'and sees them once committed');
$other->quit;
sql(q{CREATE INDEX t_k_v ON t(k) INCLUDE (v)});
is(plan_of(q{SELECT v FROM t WHERE k = 3}), 'Index Only Scan', 'included column makes the query covering');
is(sql(q{SELECT string_agg(v, ',' ORDER BY v) FROM t WHERE k = 3}), sql(q{SET enable_indexonlyscan=off; SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT string_agg(v, ',' ORDER BY v) FROM t WHERE k = 3}), 'included column values match');

# Another branch with the same relation and different rows is fetched correctly.
sql(q{SELECT dolt_commit('-Am', 'base'); SELECT dolt_branch('other')});
sql(q{SELECT dolt_checkout('other'); UPDATE t SET v = 'branched' WHERE k = 7});
is(idx(q{SELECT dolt_checkout('other'); SELECT count(*) FILTER (WHERE v = 'branched'), count(*) FROM t WHERE k = 7}), idx(q{SELECT dolt_checkout('other'); SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FILTER (WHERE v = 'branched'), count(*) FROM t WHERE k = 7}), 'fetches on the other branch');
is(idx(q{SELECT count(*) FILTER (WHERE v = 'branched') FROM t WHERE k = 7}), '0', 'fetches on main are untouched');

# Unique-key reservations: the second inserter waits for the first.
sub blocked { my ($pid, $label) = @_; ok($node->poll_query_until('postgres', "SELECT wait_event_type = 'Lock' FROM pg_stat_activity WHERE pid = $pid"), $label); }
my $a = $node->background_psql('postgres', on_error_stop => 0);
my $b = $node->background_psql('postgres', on_error_stop => 0);
my $bpid = $b->query_safe('SELECT pg_backend_pid()');
$a->query_safe('BEGIN; INSERT INTO t VALUES (10001, 1, $$a$$)');
$b->query_until(qr/started/, "\\echo started\nINSERT INTO t VALUES (10001, 1, 'b');\n");
blocked($bpid, 'second inserter waits');
$a->query_safe('COMMIT');
$b->query('SELECT 1');
like($b->{stderr}, qr/duplicate key value/, 'and fails once the first commits');
$b->{stderr} = '';
$a->query_safe('BEGIN; SAVEPOINT s; INSERT INTO t VALUES (10002, 1, $$a$$); ROLLBACK TO s');
is($b->query_safe(q{INSERT INTO t VALUES (10002, 1, 'b') RETURNING v}), 'b', 'a reservation rolled back with its savepoint is released');
$a->query_safe('COMMIT');
$a->query_safe('BEGIN; INSERT INTO t VALUES (10004, 1, $$a$$)');
$b->query_until(qr/started/, "\\echo started\nINSERT INTO t VALUES (10004, 1, 'b');\n");
blocked($bpid, 'waits again');
$a->query_safe('ROLLBACK');
$b->query_safe('SELECT 1');
is(sql(q{SELECT v FROM t WHERE id = 10004}), 'b', 'the waiter succeeds after the first aborts');
$a->quit;
$b->quit;

# With prepared transactions allowed, reservations are lock records that
# outlive the preparing backend, and a waiter sees what COMMIT PREPARED made
# visible.
$node->append_conf('postgresql.conf', "max_prepared_transactions=5\n");
$node->restart;
$a = $node->background_psql('postgres', on_error_stop => 0);
$b = $node->background_psql('postgres', on_error_stop => 0);
$bpid = $b->query_safe('SELECT pg_backend_pid()');
$a->query_safe('BEGIN; INSERT INTO t VALUES (10003, 1, $$a$$); PREPARE TRANSACTION $$reserved$$');
$b->query_until(qr/started/, "\\echo started\nINSERT INTO t VALUES (10003, 1, 'b');\n");
blocked($bpid, 'a prepared transaction keeps its reservation');
sql(q{COMMIT PREPARED 'reserved'});
$b->query('SELECT 1');
like($b->{stderr}, qr/duplicate key value/, 'and the waiter fails after it commits');
$b->{stderr} = '';
$a->quit;
$b->quit;

# A warm session binds names through a cache that must follow schema changes,
# branch switches, search paths and dropped and recreated tables.
my $w = $node->background_psql('postgres', on_error_stop => 0);
is($w->query_safe(q{SELECT count(*) FROM t WHERE k = 29}), sql(q{SELECT count(*) FROM t WHERE k = 29}), 'warm session binds t');
$w->query_safe(q{ALTER TABLE t ADD COLUMN extra int DEFAULT 7});
is($w->query_safe(q{SELECT max(extra) FROM t}), '7', 'an added column is bound after ALTER');
$w->query_safe(q{CREATE SCHEMA other; CREATE TABLE other.t(id int PRIMARY KEY, note text); INSERT INTO other.t VALUES (1, 'other')});
is($w->query_safe(q{SET search_path = other, public; SELECT note FROM t}), 'other', 'search_path selects the other table');
is($w->query_safe(q{SET search_path = public; SELECT count(*) FROM t WHERE k = 29}), sql(q{SELECT count(*) FROM t WHERE k = 29}), 'and back');
$w->query_safe(q{SELECT dolt_checkout('other')});
is($w->query_safe(q{SELECT count(*) FROM t WHERE v = 'branched'}), sql(q{SELECT count(*) FROM "postgres/other".public.t WHERE v = 'branched'}), 'the other branch binds its own t');
$w->query_safe(q{UPDATE t SET v = 'branched' WHERE k = 7; SELECT dolt_checkout('main')});
is($w->query_safe(q{SELECT count(*) FROM t WHERE v = 'branched'}), '0', 'main after switching back');
$w->query_safe(q{DROP TABLE t; CREATE TABLE t(a text); INSERT INTO t VALUES ('new')});
is($w->query_safe(q{SELECT a FROM t}), 'new', 'a recreated table binds to its new definition');
$w->query_safe(q{BEGIN; ALTER TABLE t ADD COLUMN b int DEFAULT 1});
is($w->query_safe(q{SELECT a, b FROM t}), 'new|1', 'uncommitted DDL binds in its transaction');
$w->query_safe('ROLLBACK');
is($w->query_safe(q{SELECT * FROM t}), 'new', 'and not after rollback');
is(sql(q{SELECT * FROM t}), 'new', 'another session agrees');
$w->quit;
$node->stop;
done_testing();
