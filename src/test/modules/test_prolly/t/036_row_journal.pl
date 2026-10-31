# With dolt_row_journal on, small writes are journaled rather than folded into
# the trees; readers fold the journal in memory. What every reader sees,
# across commands, sessions, aborts, folds, commits, branches, resets and
# restarts, must not change.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('journal');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nmax_parallel_workers_per_gather=0\ndolt_row_journal=on\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub fails { my ($q, $re, $label) = @_; my ($code, $out, $err) = $node->psql('postgres', $q); isnt($code, 0, $label); like($err, $re, "$label diagnostic"); }
sub journal_rows { sql(q{SELECT count(*) FROM pg_dolt_journal}); }
sub roots { sql(q{SELECT encode(rootcontent, 'hex') FROM pg_dolt_root ORDER BY rootfile LIMIT 1}); }

sql(q{CREATE TABLE t(id int PRIMARY KEY, k int NOT NULL, v text); CREATE INDEX t_k ON t(k)});
sql(q{INSERT INTO t SELECT g, g % 10, 'v'||g FROM generate_series(1, 1000) g});
is(journal_rows(), '0', 'a bulk insert is folded, not journaled');
my $root = roots();

# Single-row writes are journaled; every kind of read sees them.
sql(q{INSERT INTO t VALUES (1001, 1, 'journaled')});
is(journal_rows(), '1', 'a single insert is journaled');
isnt($root, '', 'the table has roots');
is(roots(), $root, 'and the roots stay as they were');
is(sql(q{SELECT v FROM t WHERE id = 1001}), 'journaled', 'point read through the primary key');
is(sql(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t WHERE k = 1}), '101', 'index scan counts the journaled row');
is(sql(q{SET enable_seqscan=off; SET enable_indexscan=off; SELECT count(*) FROM t WHERE k = 1}), '101', 'bitmap scan counts it');
is(sql(q{SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t}), '1001', 'sequential scan counts it');
is(sql(q{SELECT count(*) FROM (SELECT DISTINCT ctid FROM t) x}), '1001', 'it has its own handle');
sql(q{UPDATE t SET v = 'changed' WHERE id = 1001; DELETE FROM t WHERE id = 1000});
is(journal_rows(), '4', 'an update and a delete add their entries');
is(sql(q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t WHERE id >= 999}), '999:v999,1001:changed', 'the update and delete are visible');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 0}), '99', 'the deleted row left the index');
fails(q{INSERT INTO t VALUES (1001, 2, 'dup')}, qr/duplicate key/, 'a journaled row holds its unique key');
sql(q{INSERT INTO t VALUES (1000, 0, 'again')});
is(sql(q{SELECT v FROM t WHERE id = 1000}), 'again', 'a deleted key can be reused');

# The same transaction, later commands and sessions, aborts.
is(sql(q{BEGIN; INSERT INTO t VALUES (2001, 1, 'a'); UPDATE t SET v = 'b' WHERE id = 2001; SELECT v FROM t WHERE id = 2001; COMMIT}), 'b', 'a later command sees the journaled row');
is(sql(q{BEGIN; INSERT INTO t VALUES (2002, 1, 'x'); ROLLBACK; SELECT count(*) FROM t WHERE id = 2002}), '0', 'an aborted entry is gone');
is(sql(q{BEGIN; INSERT INTO t VALUES (2003, 1, 'x'); SAVEPOINT s; DELETE FROM t WHERE id = 2003; ROLLBACK TO s; COMMIT; SELECT v FROM t WHERE id = 2003}), 'x', 'a savepoint rollback forgets only its entry');
# A fold cached under one command serves later ones only while the stream
# stays as it saw it: the transaction's own later writes, and a savepoint
# rollback of them, must show in the next command under the same snapshot.
is(sql(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t WHERE id >= 2004;
  INSERT INTO t VALUES (2004, 1, 'a'); SELECT count(*) FROM t WHERE id >= 2004; SELECT count(*) FROM t WHERE id >= 2004;
  SAVEPOINT s; INSERT INTO t VALUES (2005, 1, 'b'); SELECT count(*) FROM t WHERE id >= 2004;
  ROLLBACK TO s; SELECT count(*) FROM t WHERE id >= 2004; SET LOCAL enable_seqscan = off; SELECT count(*) FROM t WHERE k = 1 AND id >= 2004; COMMIT}),
  "0\n1\n1\n2\n1\n1", 'later commands see own writes and lose rolled-back ones');
my $w = $node->background_psql('postgres');
$w->query_safe(q{BEGIN; INSERT INTO t VALUES (3001, 1, 'w'); UPDATE t SET v = 'w' WHERE id = 1});
is(sql(q{SELECT count(*) FROM t WHERE v = 'w'}), '0', 'another session does not see uncommitted entries');
is($w->query_safe(q{SELECT count(*) FROM t WHERE v = 'w'}), '2', 'but the writer does');
$w->query_safe('COMMIT');
is(sql(q{SELECT count(*) FROM t WHERE v = 'w'}), '2', 'and everyone does after commit');
is($w->query_safe(q{SELECT count(*) FROM t WHERE v = 'w'}), '2', 'the writer still does');
my $r = $node->background_psql('postgres');
$r->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($r->query_safe(q{SELECT count(*) FROM t WHERE id > 3000}), '1', 'repeatable read snapshot');
sql(q{INSERT INTO t VALUES (3002, 1, 'later')});
is($r->query_safe(q{SELECT count(*) FROM t WHERE id > 3000}), '1', 'does not see a later entry');
$r->query_safe('COMMIT');
is($r->query_safe(q{SELECT count(*) FROM t WHERE id > 3000}), '2', 'and sees it in the next transaction');
$r->quit;

# A session keeps the entries it read across transactions, with its own
# changes: what it sees must match a fresh session after others' commits, its
# own commits, aborts, savepoint rollbacks, interleaved commits and folds.
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
my $count = q{SELECT count(*), coalesce(sum(k), 0) FROM t WHERE id > 7000};
sub agree
{
	my ($label) = @_;
	my $fresh = sql($count);
	is($a->query_safe($count), $fresh, "$label: the first session agrees");
	is($b->query_safe($count), $fresh, "$label: the second session agrees");
}
agree('before any change');
$b->query_safe(q{INSERT INTO t VALUES (7001, 1, 'b')});
agree('after another session commits');
$a->query_safe(q{INSERT INTO t VALUES (7002, 2, 'a'); UPDATE t SET k = 3 WHERE id = 7001});
agree('after its own commit');
$a->query_safe(q{BEGIN; INSERT INTO t VALUES (7003, 4, 'a'); SELECT count(*) FROM t; ROLLBACK});
agree('after its own abort');
$a->query_safe(q{BEGIN; INSERT INTO t VALUES (7004, 5, 'a'); SAVEPOINT s; INSERT INTO t VALUES (7005, 6, 'a'); SELECT count(*) FROM t; ROLLBACK TO s; COMMIT});
agree('after a savepoint rollback');
$a->query_safe(q{BEGIN; INSERT INTO t VALUES (7006, 7, 'a')});
$b->query_safe(q{INSERT INTO t VALUES (7007, 8, 'b'); DELETE FROM t WHERE id = 7002});
$a->query_safe(q{COMMIT});
agree('after interleaved commits');
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $old = $b->query_safe($count);
$a->query_safe(q{INSERT INTO t VALUES (7008, 9, 'a')});
is($b->query_safe($count), $old, 'an older snapshot does not see a later commit');
$b->query_safe('COMMIT');
agree('after the older snapshot ends');
$a->query_safe(join(';', 'BEGIN', (map { "INSERT INTO t VALUES ($_, 1, 'many')" } 7101 .. 7400), 'COMMIT'));
agree('after a fold consumes the stream');
$b->query_safe(q{UPDATE t SET k = k + 1 WHERE id BETWEEN 7101 AND 7103});
agree('after writes past the fold');
$a->quit;
$b->quit;

# A stream past its bound folds: the journal empties and the roots move.
my $before = journal_rows();
sql(join(';', map { "INSERT INTO t VALUES ($_, 5, 'fold')" } 4001 .. 4300));
ok(journal_rows() < $before + 300, 'the stream folded');
isnt(roots(), $root, 'into new roots');
is(sql(q{SELECT count(*) FROM t WHERE v = 'fold'}), '300', 'every folded row remains');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 5}), sql(q{SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t WHERE k = 5}), 'index and table agree after the fold');

# A batch past the stream's bound folds the stream with it, and its indexes'
# batches must save exactly the entries the table's took out of the journal,
# though other transactions commit before the fold: unrelated ones, which
# make the batches reload, or writers adding to the stream.
sql(q{CREATE TABLE f(id int PRIMARY KEY, k int, v text); CREATE INDEX f_k ON f(k); INSERT INTO f SELECT g, g, 'x' FROM generate_series(1, 1000) g});
my $index_k = q{SET enable_seqscan = off; SET enable_bitmapscan = off; };
my $table_k = q{SET enable_indexscan = off; SET enable_bitmapscan = off; };
my $fw = $node->background_psql('postgres');
sql(join(';', map { "UPDATE f SET v = 'u$_' WHERE id = $_" } 1 .. 120));
$fw->query_safe(q{BEGIN; UPDATE f SET k = k + 5000 WHERE id BETWEEN 111 AND 120});
sql(q{SELECT pg_current_xact_id()});
$fw->query_safe('COMMIT');
is(sql($index_k . q{SELECT count(*) FROM f WHERE k > 5000}), '10', 'an index batch reloaded after its table folded the stream keeps the folded entries');
is(sql($index_k . q{SELECT count(*) FROM f WHERE k > 0}), sql($table_k . q{SELECT count(*) FROM f WHERE k > 0}), 'index and table agree');
sql(join(';', map { "UPDATE f SET v = 'w$_' WHERE id = $_" } 1 .. 120));
$fw->query_safe(q{BEGIN; UPDATE f SET k = k + 5000 WHERE id BETWEEN 101 AND 110});
sql(q{UPDATE f SET v = 'other' WHERE id = 500});
$fw->query_safe('COMMIT');
is(sql($index_k . q{SELECT v FROM f WHERE k = 500}), 'other', 'the index holds a concurrent writer\'s entry the table folded');
is(sql($index_k . q{SELECT string_agg(id || v, ',' ORDER BY id) FROM f WHERE k > 0}), sql($table_k . q{SELECT string_agg(id || v, ',' ORDER BY id) FROM f WHERE k > 0}), 'index and table agree');
$fw->quit;

# Version control folds what it needs: commits, status, branches, resets.
sql(q{INSERT INTO t VALUES (5001, 1, 'before commit')});
is(sql(q{SELECT count(*) FROM dolt_status WHERE table_name = 'public.t'}), '1', 'status sees journaled changes');
sql(q{SELECT dolt_commit('-Am', 'with journaled rows')});
is(sql(q{SELECT count(*) FROM dolt_status WHERE table_name = 'public.t'}), '0', 'and nothing after the commit');
is(sql(q{SET enable_seqscan=off; SELECT v FROM t AS OF 'HEAD' WHERE id = 5001}), 'before commit', 'the committed index holds the journaled row');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t AS OF 'HEAD' WHERE k = 1}), sql(q{SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT count(*) FROM t AS OF 'HEAD' WHERE k = 1}), 'committed index and table agree');
sql(q{SELECT dolt_branch('other')});
is(sql(q{SELECT v FROM "postgres/other".public.t WHERE id = 5001}), 'before commit', 'a branch from the commit holds the journaled row');
sql(q{INSERT INTO t VALUES (5002, 1, 'after commit')});
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD', 'WORKING', 't')}), '1', 'diff sees the journaled row');
sql(q{SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FROM t WHERE id = 5002}), '0', 'reset discards journaled rows');
is(sql(q{SELECT v FROM t WHERE id = 5001}), 'before commit', 'and keeps committed ones');
sql(q{INSERT INTO t VALUES (5003, 1, 'to truncate')});
sql(q{TRUNCATE t});
is(sql(q{SELECT count(*) FROM t}), '0', 'truncate empties a journaled table');
sql(q{INSERT INTO t VALUES (1, 1, 'fresh')});
is(sql(q{SELECT v FROM t}), 'fresh', 'and it accepts rows again');

# Everything survives a restart, and handles continue past journaled ones.
sql(q{INSERT INTO t VALUES (2, 1, 'two'), (3, 1, 'three')});
my $ctids = sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t});
$node->restart;
is(sql(q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t}), '1:fresh,2:two,3:three', 'journaled rows survive a restart');
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t}), $ctids, 'with their handles');
sql(q{INSERT INTO t VALUES (4, 1, 'four')});
is(sql(q{SELECT count(*) FROM (SELECT DISTINCT ctid FROM t) x}), '4', 'a new handle does not collide after the restart');
is(sql(q{SET enable_seqscan=off; SELECT count(*) FROM t WHERE k = 1}), '4', 'the index agrees');

# Cancelling a speculative row must not journal a deletion of a nonexistent row.
# Test both the stream cache and the prepared-transaction uncached read path.
for my $prepared (0, 10)
{
    $node->append_conf('postgresql.conf', "max_prepared_transactions=$prepared\n");
    $node->restart;
    sql(q{CREATE TABLE spec(a int, b int, c int);
      CREATE UNIQUE INDEX spec_key ON spec(b,c) NULLS NOT DISTINCT;
      INSERT INTO spec VALUES (1,1,1);
      INSERT INTO spec VALUES (4,1,2),(5,2,1),(6,NULL,1),(7,NULL,1),
        (12,1,1),(8,1,NULL),(9,1,NULL),(10,NULL,NULL),(11,NULL,NULL)
        ON CONFLICT DO NOTHING});
    is(sql(q{SELECT string_agg(a::text, ',' ORDER BY a) FROM spec}), '1,4,5,6,8,10',
        "cancelled speculative rows leave no journal deletions ($prepared)");
    is(sql(q{SET enable_seqscan=off; SELECT a FROM spec WHERE b=1 AND c=2}), '4',
        "unique index agrees after cancellation ($prepared)");
    sql(q{BEGIN; INSERT INTO spec VALUES (20,3,3),(21,3,3) ON CONFLICT DO NOTHING; ROLLBACK});
    is(sql(q{SELECT count(*) FROM spec}), '6', "rollback discards speculative batch ($prepared)");
    $node->restart;
    is(sql(q{SELECT string_agg(a::text, ',' ORDER BY a) FROM spec}), '1,4,5,6,8,10',
        "cancelled speculative rows stay absent after restart ($prepared)");
    sql(q{DROP TABLE spec});
}

# A crash loses the shared handle slots; the first commit after it must not claim one.
$node->stop('immediate');
$node->start;
is(sql(q{ALTER TABLE t ALTER COLUMN v TYPE varchar(20); SELECT v FROM t WHERE id = 4}), 'four', 'a rewrite commits first after a crash');
$node->stop;
done_testing();
