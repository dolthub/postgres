# Row handles are derived from row content: the same row has the same TID on
# every branch and in every revision, and only duplicate rows, prefix
# collisions and recently used handles take allocated ones from the catalog.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('derived_handles');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
# The handle a ctid encodes, as dg_id computes it.
my $handle = q{((ctid::text::point)[0]::bigint * 291 + (ctid::text::point)[1]::bigint - 1)};
my $derived = 1 << 40;

sql(q{CREATE TABLE t(id int PRIMARY KEY, k int NOT NULL, v text); CREATE INDEX t_k ON t(k)});
sql(q{INSERT INTO t SELECT g, g % 10, 'v' || g FROM generate_series(1, 500) g});
is(sql(qq{SELECT count(*) FROM t WHERE $handle >= $derived}), '0', 'distinct rows take derived handles');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '0', 'and write no handle rows');
is(sql(q{SELECT count(DISTINCT ctid) FROM t}), '500', 'every row has its own handle');
is(sql(q{SET enable_seqscan=off; SELECT v FROM t WHERE id = 42}), 'v42', 'the index finds a row by its derived handle');

# The same row has the same handle everywhere: on a branch, in a revision.
sql(q{SELECT dolt_commit('-Am', 'base')});
sql(q{SELECT dolt_branch('other')});
my $tids = sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t WHERE id <= 20});
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM "postgres/other".public.t WHERE id <= 20}), $tids, 'a branch shares its rows\' handles');
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t AS OF 'HEAD' WHERE id <= 20}), $tids, 'a revision shows the same handles');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '0', 'creating the branch wrote no handle rows');

# An update gives the new version a handle derived from its content; the old one is free again.
my $old = sql(q{SELECT ctid FROM t WHERE id = 7});
sql(q{UPDATE t SET v = 'seven' WHERE id = 7});
my $new = sql(q{SELECT ctid FROM t WHERE id = 7});
isnt($new, $old, 'an update changes the handle');
is(sql(qq{SELECT $handle < $derived FROM t WHERE id = 7}), 't', 'to a derived one');
is(sql(q{SELECT ctid FROM "postgres/other".public.t WHERE id = 7}), $old, 'the other branch keeps the old version and handle');

# Duplicate rows: the first occurrence is derived, the rest allocated, one row each.
sql(q{CREATE TABLE dup(x int, y text); INSERT INTO dup VALUES (1, 'a'), (1, 'a'), (1, 'a'), (2, 'b')});
is(sql(q{SELECT count(DISTINCT ctid) FROM dup}), '4', 'duplicates have distinct handles');
is(sql(qq{SELECT count(*) FROM dup WHERE $handle >= $derived}), '2', 'two of which are allocated');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '2', 'with a catalog row each');
is(sql(q{SELECT count(*) FROM (SELECT ctid FROM dup WHERE x = 1 ORDER BY ctid LIMIT 1 FOR UPDATE) s}), '1', 'a duplicate occurrence locks on its own');
sql(q{DELETE FROM dup WHERE ctid = (SELECT ctid FROM dup WHERE x = 1 ORDER BY ctid DESC LIMIT 1)});
is(sql(q{SELECT count(*) FROM dup WHERE x = 1}), '2', 'deleting one occurrence by ctid removes one');
sql(q{DELETE FROM dup WHERE x = 1});
is(sql(q{SELECT count(*) FROM dup}), '1', 'deleting the row removes every occurrence');
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '0', 'and their catalog rows');
sql(q{INSERT INTO dup VALUES (1, 'a'), (1, 'a')});
is(sql(q{SELECT count(*) FROM pg_dolt_handle}), '1', 'reinserted duplicates take the derived handle again, and one allocated one');

# Within a transaction, each command sees the handles earlier ones allocated,
# and a rolled back savepoint's handles are gone, while a session reads the
# catalog once per storage rather than once per command.
sql(q{CREATE TABLE dup2(x int, y text)});
is(sql(q{BEGIN; INSERT INTO dup2 VALUES (1, 'a'), (1, 'a'); INSERT INTO dup2 VALUES (1, 'a');
 SAVEPOINT s; INSERT INTO dup2 VALUES (1, 'a'), (1, 'a'); ROLLBACK TO s;
 SELECT count(DISTINCT ctid) || ' ' || count(*) FROM dup2;
 DELETE FROM dup2 WHERE ctid = (SELECT max(ctid) FROM dup2);
 INSERT INTO dup2 VALUES (1, 'a'), (1, 'a');
 SELECT count(DISTINCT ctid) || ' ' || count(*) FROM dup2; COMMIT}), "3 3\n4 4", 'commands see earlier commands\' handles, not a rolled back savepoint\'s');
is(sql(qq{SELECT count(*) FROM dup2 WHERE $handle >= $derived}), '3', 'every occurrence but one keeps an allocated handle');
sql(q{CREATE TABLE parent(id int PRIMARY KEY); CREATE TABLE child(id int PRIMARY KEY, p int REFERENCES parent, v int);
 INSERT INTO parent SELECT generate_series(1, 100); INSERT INTO child SELECT g, g % 100 + 1, 0 FROM generate_series(1, 1000) g});
my @lines = split /\n/, sql(join(' ', 'BEGIN;', (map { "UPDATE child SET v = v + 1, p = " . ($_ % 100 + 1) . " WHERE id = " . ($_ * 37) . ';' } 1 .. 20),
	'SELECT pg_stat_get_xact_numscans(9140); COMMIT'));
cmp_ok($lines[-1], '<', 10, "twenty foreign-keyed updates read the handle catalog $lines[-1] times");
is(sql(q{SELECT sum(v) FROM child}), '20', 'and change twenty rows');

# A branch inherits derived handles for free and allocates for the duplicates it meets.
sql(q{SELECT dolt_commit('-Am', 'dup')});
sql(q{SELECT dolt_branch('dups')});
is(sql(q{SELECT count(DISTINCT ctid) FROM "postgres/dups".public.dup}), '3', 'a branch tells duplicates apart');
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY ctid) FROM "postgres/dups".public.dup WHERE x = 2}),
	sql(q{SELECT string_agg(ctid::text, ',' ORDER BY ctid) FROM dup WHERE x = 2}), 'and shares the distinct row\'s handle');
is(sql(q{SELECT count(DISTINCT ctid) FROM dup AS OF 'HEAD'}), '3', 'as does a revision');

# A row deleted for good and re-inserted takes its derived handle again; the
# dead lock record of the old row does not shadow it. A handle a live record
# still names, one of a row deleted in an open transaction, is not reused.
my $before = sql(q{SELECT ctid FROM t WHERE id = 100});
sql(q{DELETE FROM t WHERE id = 100});
sql(q{INSERT INTO t VALUES (100, 0, 'v100')});
is(sql(q{SELECT ctid FROM t WHERE id = 100}), $before, 'a re-inserted row takes its derived handle again');
sql(q{UPDATE t SET v = 'hundred' WHERE id = 100});
is(sql(q{SELECT v FROM t WHERE id = 100}), 'hundred', 'which locks and updates');
is(sql(q{SELECT count(*) FROM t WHERE id = 100}), '1', 'as one row');
my $root = sql(q{SELECT ctid FROM dup WHERE x = 2});
sql(q{UPDATE dup SET y = 'c' WHERE x = 2});
isnt(sql(q{INSERT INTO dup VALUES (2, 'b') RETURNING ctid}), $root, 'a handle at the root of a live update chain is not derived again');
is(sql(qq{SELECT count(*) FILTER (WHERE $handle >= $derived), count(*) FROM dup WHERE x = 2}), '1|2', 'the row takes an allocated one');

# Concurrent first inserters of identical rows into a keyless table: the
# first reserves the derived handle until it ends, the second allocates, and
# each occurrence keeps a handle of its own.
sql(q{CREATE TABLE race(x int, y text)});
my $first = $node->background_psql('postgres');
$first->query_safe(q{BEGIN; INSERT INTO race VALUES (1, 'same')});
my $second = sql(q{INSERT INTO race VALUES (1, 'same') RETURNING ctid});
$first->query_safe('COMMIT');
my $held = $first->query_safe(q{SELECT ctid FROM race ORDER BY ctid LIMIT 1});
$first->quit;
isnt($second, $held, 'a concurrent inserter of an identical row takes a different handle');
is(sql(qq{SELECT count(*), count(DISTINCT ctid), count(*) FILTER (WHERE $handle >= $derived) FROM race}), '2|2|1', 'one derived, one allocated');
is(sql(q{WITH d AS (DELETE FROM race WHERE x = 1 RETURNING 1) SELECT count(*) FROM d}), '2', 'and a delete removes both');
$first = $node->background_psql('postgres');
$first->query_safe(q{BEGIN; INSERT INTO race VALUES (2, 'gone'); ROLLBACK});
$first->quit;
is(sql(qq{INSERT INTO race VALUES (2, 'gone') RETURNING $handle < $derived}), 't', 'a reservation of an aborted transaction does not block the derived handle');

# Different rows whose digests share a derived handle's prefix: (604457, 0)
# and (2316285, 0), found among millions of rows. A primary key does not
# keep concurrent inserters of them apart; the claim on the handle does.
sql(q{CREATE TABLE clash(id int PRIMARY KEY, v int)});
$first = $node->background_psql('postgres');
$first->query_safe(q{BEGIN; INSERT INTO clash VALUES (604457, 0)});
sql(q{INSERT INTO clash VALUES (2316285, 0)});
$first->query_safe('COMMIT');
$first->quit;
is(sql(qq{SELECT count(*), count(DISTINCT ctid), count(*) FILTER (WHERE $handle >= $derived) FROM clash}), '2|2|1',
	'concurrent inserters of rows sharing a prefix take different handles');
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM clash}), '604457,2316285', 'a scan returns each row once');
is(sql(q{WITH d AS (DELETE FROM clash WHERE id = 604457 RETURNING id) SELECT string_agg(id::text, ',') FROM d}), '604457',
	'a delete by key removes its row');
is(sql(q{SELECT string_agg(id::text, ',') FROM clash}), '2316285', 'and leaves the other');

# A row committed while a long command runs, after the command loaded its
# state, is invisible to that state: its committer's claim outlives it.
sql(q{CREATE TABLE late(id int, v int)});
$first = $node->background_psql('postgres');
$first->query_until(qr/started/, q{\echo started
INSERT INTO late SELECT 1, 0 UNION ALL SELECT 2316285, 0 FROM pg_sleep(3);
});
sleep(1);
sql(q{INSERT INTO late VALUES (604457, 0)});
$first->query_safe('SELECT 1');
$first->quit;
is(sql(qq{SELECT count(*), count(DISTINCT ctid), count(*) FILTER (WHERE $handle >= $derived) FROM late WHERE id > 1}), '2|2|1',
	'a row committed during a command keeps a handle of its own');
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM late}), '1,604457,2316285', 'and every row is found once');

# Lossy bitmap pages: a page is a digest range, scanned once.
sql(q{CREATE TABLE big(id int PRIMARY KEY, k int NOT NULL); CREATE INDEX big_k ON big(k)});
sql(q{INSERT INTO big SELECT g, g % 100 FROM generate_series(1, 20000) g});
my $expected = sql(q{SET enable_bitmapscan=off; SELECT count(*), sum(id) FROM big WHERE k BETWEEN 10 AND 60});
is(sql(q{SET enable_seqscan=off; SET enable_indexscan=off; SET work_mem='64kB'; SELECT count(*), sum(id) FROM big WHERE k BETWEEN 10 AND 60}),
	$expected, 'a lossy bitmap scan finds every row once');
is(sql(q{SET enable_seqscan=off; SET enable_indexscan=off; SET work_mem='64kB'; SELECT count(*) FROM big WHERE k BETWEEN 10 AND 60 AND id % 7 = 0}),
	sql(q{SET enable_bitmapscan=off; SELECT count(*) FROM big WHERE k BETWEEN 10 AND 60 AND id % 7 = 0}), 'and rechecks its qualification');

# A claim outlives its transaction while a transaction that could still
# write holds an older snapshot: a row removed and inserted again then takes
# an allocated handle. An autovacuum worker's snapshot does not count, since
# it writes no rows: an ANALYZE of a large table would otherwise keep every
# claim made while it reads. This ANALYZE holds its snapshot while it
# evaluates an expensive index expression for each row it samples.
sql(q{CREATE TABLE reclaim(id int, v int) WITH (autovacuum_enabled = off)});
sub reinserted_handle {
	sql(q{INSERT INTO reclaim VALUES (1, 1)});
	sql(q{DELETE FROM reclaim WHERE id = 1});
	sql(q{INSERT INTO reclaim VALUES (1, 1)});
	my $allocated = sql(qq{SELECT $handle >= $derived FROM reclaim WHERE id = 1});
	sql(q{DELETE FROM reclaim WHERE id = 1});
	return $allocated eq 't' ? 'allocated' : 'derived';
}
my $reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t');
is(reinserted_handle(), 'allocated', 'an open snapshot keeps the claim of a row it may not see');
$reader->query_safe('COMMIT');
$reader->quit;
is(reinserted_handle(), 'derived', 'and once it ends, the row takes its derived handle again');
sql(q{CREATE TABLE slow(x int) USING heap WITH (autovacuum_analyze_threshold = 0, autovacuum_analyze_scale_factor = 0)});
sql(q{INSERT INTO slow SELECT generate_series(1, 120)});
sql(q{CREATE INDEX slow_md5 ON slow (md5(repeat(x::text, 20000000)))});
sql(q{ALTER SYSTEM SET autovacuum_naptime = '1s'});
sql(q{SELECT pg_reload_conf()});
ok($node->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_activity WHERE backend_type = 'autovacuum worker' AND query LIKE '%ANALYZE%slow%'}),
	'an autovacuum worker is analyzing');
is(reinserted_handle(), 'derived', "an autovacuum worker's older snapshot does not keep the claim");
ok(sql(q{SELECT count(*) > 0 FROM pg_stat_activity WHERE backend_type = 'autovacuum worker' AND query LIKE '%ANALYZE%slow%'}) eq 't',
	'while the worker was still analyzing');
sql(q{ALTER SYSTEM RESET autovacuum_naptime; SELECT pg_reload_conf(); DROP TABLE slow});

# Allocated handles continue past the committed ones after a crash.
sql(q{INSERT INTO dup VALUES (1, 'a')});
my $max = sql(qq{SELECT max($handle) FROM dup});
$node->stop('immediate');
$node->start;
sql(q{INSERT INTO dup VALUES (1, 'a')});
cmp_ok(sql(qq{SELECT max($handle) FROM dup}), '>', $max, 'allocation continues above committed handles after a crash');
is(sql(q{SELECT count(DISTINCT ctid), count(*) FROM dup WHERE x = 1}), '4|4', 'every duplicate keeps its own handle');
$node->stop;
done_testing();
