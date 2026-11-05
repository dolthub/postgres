use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Digest::SHA qw(sha256_hex);

my $node = PostgreSQL::Test::Cluster->new('manifest');
$node->init(allows_streaming => 'physical', extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "max_parallel_workers_per_gather=0\nmax_prepared_transactions=10\n");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sub failure {
 my ($query, $pattern, $label) = @_;
 my ($status, $out, $err) = $node->psql('postgres', $query);
 isnt($status, 0, $label);
 like($err, $pattern, "$label diagnostic");
}
sub readref { return sql("SELECT encode(target,'hex') FROM dolt_manifest_read('$_[0]')"); }
sub token { return sql("SELECT encode(token,'hex') FROM dolt_manifest_read('$_[0]')"); }
sub hasharg { return "decode('$_[0]','hex')"; }
# The hash of the bytes the chunk store holds under node hash |$_[0]|.
sub stored { return sql("SELECT encode(substr(sha256(dolt_chunk_read(".hasharg($_[0]).")),1,20),'hex')"); }
sub put {
 my ($kind, $payload, @children) = @_;
 return sql("SELECT encode(dolt_manifest_put('$kind',convert_to('$payload','UTF8'),ARRAY[" . join(',', map {hasharg($_)} @children) . "]::bytea[]),'hex')");
}
sub cas {
 my ($name, $expected, $target) = @_;
 return "SELECT encode(dolt_manifest_cas('$name'," . (defined($expected) ? hasharg($expected) : 'NULL') . ',' . (defined($target) ? hasharg($target) : 'NULL') . "),'hex')";
}

is(sql(q{SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid=c.relam WHERE c.relname IN ('pg_dolt_ref','pg_dolt_object') AND a.amname='heap'}), '2', 'manifest uses native heap');
my $schema = put('schema', 'test-schema-v1:id=int4,value=text');
is(put('schema', 'test-schema-v1:id=int4,value=text'), $schema, 'immutable object deduplication');
my $schema_payload = 'test-schema-v1:id=int4,value=text';
is($schema, substr(sha256_hex('DGMOBJ01' . pack('NNN', 1, 0, length($schema_payload)) . $schema_payload), 0, 40), 'object envelope matches independent truncated SHA-256 encoding');
my $empty = sql(q{SELECT encode(dolt_root_put('{}','{}','{}','{}'),'hex')});
my $c1 = put('commit', 'author=test;message=initial', $empty);
my $c2 = put('commit', 'author=test;message=second', $empty, $c1);
is(put('commit', 'author=test;message=initial', $empty), $c1, 'identical commit encoding deterministic');
isnt($c1, $c2, 'history identity differs while database target is equal');
my $t1 = sql(cas('heads/probe', undef, $c1));
is(readref('heads/probe'), $c1, 'create and read HEAD');
is(readref('heads/missing'), '', 'absent ref returns null fields');
failure(cas('heads/probe', undef, $c1), qr/duplicate key/, 'concurrent-style duplicate creation rejected');
failure(cas('heads/bad', undef, $empty), qr/not visible/, 'HEAD requires commit object');
failure(cas('working/bad', undef, $c1), qr/not in the chunk store/, 'a working set requires a database root');
failure(q{SELECT dolt_manifest_put('commit',''::bytea,ARRAY[]::bytea[])}, qr/child count/, 'commit needs database child');
failure(q{SELECT dolt_manifest_put('tag',''::bytea,ARRAY[decode(repeat('00',20),'hex')])}, qr/not visible/, 'dangling children rejected');
failure(q{SELECT dolt_manifest_put('commit',''::bytea,ARRAY[decode(repeat('00',20),'hex')])}, qr/not in the chunk store/, 'dangling database roots rejected');
failure(q{SELECT dolt_manifest_put('table',''::bytea,ARRAY[]::bytea[])}, qr/belong in database roots/, 'members are not manifest objects');
failure(q{SELECT dolt_manifest_put('database',''::bytea,ARRAY[]::bytea[])}, qr/belong in database roots/, 'nor are database states');
failure(q{SELECT dolt_manifest_put('schema',''::bytea,ARRAY[NULL,NULL]::bytea[])}, qr/child count/, 'schema child count validated');
failure(q{SELECT dolt_manifest_put('tag',''::bytea,ARRAY[NULL]::bytea[])}, qr/cannot be null/, 'null children rejected');
failure(q{SELECT dolt_manifest_put('chunk',''::bytea,ARRAY[]::bytea[])}, qr/belong in the chunk store/, 'tree nodes are not manifest objects');
failure(q{SELECT dolt_manifest_put('schema',convert_to('DGSCHEMA2','UTF8'),ARRAY[decode(repeat('00',20),'hex')])}, qr/chunk store does not hold/, 'tree edges must name stored nodes');
failure(q{SELECT dolt_manifest_get('\x00'::bytea)}, qr/20 bytes/, 'invalid hash size rejected');
failure(cas('main', undef, $c1), qr/heads\//, 'ref namespace checked');
failure("SELECT dolt_manifest_put('rebase',''::bytea,ARRAY[".hasharg($schema).",".hasharg($schema)."])", qr/invalid schema child/, 'typed DAG edges enforced');

# Snapshot semantics, own writes, rollback and ABA.
my $reader = $node->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($reader->query_safe(q{SELECT encode(target,'hex') FROM dolt_manifest_read('heads/probe')}), $c1, 'RR pins initial HEAD');
my $t2 = sql(cas('heads/probe', $t1, $c2));
is($reader->query_safe(q{SELECT encode(target,'hex') FROM dolt_manifest_read('heads/probe')}), $c1, 'RR retains old HEAD');
my ($out, $error) = $reader->query(cas('heads/probe', $t1, $c2));
is($error, 1, 'RR stale update fails');
$reader->query('ROLLBACK'); $reader->quit;
is(readref('heads/probe'), $c2, 'new statement sees new HEAD');
failure(cas('heads/probe', $t1, $c1), qr/ref changed/, 'stale CAS rejected');
my $t3 = sql(cas('heads/probe', $t2, $c1));
isnt($t3, $t1, 'move-back changes token');
failure(cas('heads/probe', $t1, $c2), qr/ref changed/, 'ABA move-back rejected');
my $own = $node->background_psql('postgres');
$own->query_safe('BEGIN');
$own->query_safe(cas('working/private', undef, $empty));
is($own->query_safe(q{SELECT encode(target,'hex') FROM dolt_manifest_read('working/private')}), $empty, 'own ref visible after command boundary');
is(readref('working/private'), '', 'uncommitted ref invisible to others');
$own->query_safe('SAVEPOINT s');
$own->query_safe(cas('tags/rolledback', undef, $c1));
$own->query_safe('ROLLBACK TO s');
is($own->query_safe(q{SELECT target IS NULL FROM dolt_manifest_read('tags/rolledback')}), 't', 'savepoint removes ref');
$own->query_safe('ROLLBACK'); $own->quit;
is(readref('working/private'), '', 'abort removes working ref');
sql(cas('heads/probe', $t3, undef));
is(readref('heads/probe'), '', 'delete with expected token');
my $t4 = sql(cas('heads/probe', undef, $c1));
failure(cas('heads/probe', $t3, $c2), qr/ref changed/, 'delete-recreate ABA rejected');

# Two writers: same ref waits, rechecks token; different refs proceed.
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres', on_error_stop => 0);
my $pid = $b->query_safe('SELECT pg_backend_pid()');
$a->query_safe('BEGIN');
$a->query_safe(cas('heads/probe', $t4, $c2));
$b->query_until(qr/waiting/, "\\echo waiting\n" . cas('heads/probe', $t4, $c1) . ";\n");
ok($node->poll_query_until('postgres', "SELECT wait_event_type='Lock' FROM pg_stat_activity WHERE pid=$pid"), 'competing HEAD writer waits on PostgreSQL lock');
is(length(sql(cas('heads/independent', undef, $c1))), 40, 'different ref writer proceeds while HEAD locked');
$a->query_safe('COMMIT');
($out, $error) = $b->query('SELECT 1');
is($error, 1, 'waiting writer rejects changed token after winner commits');
$a->quit; $b->quit;
is(readref('heads/probe'), $c2, 'winner not overwritten');
# If the competing updater aborts, the waiting CAS can use the original token.
my $retry_token = token('heads/probe');
$a = $node->background_psql('postgres');
$b = $node->background_psql('postgres');
$pid = $b->query_safe('SELECT pg_backend_pid()');
$a->query_safe('BEGIN');
$a->query_safe(cas('heads/probe', $retry_token, $c1));
$b->query_until(qr/waiting_abort/, "\\echo waiting_abort\n" . cas('heads/probe', $retry_token, $c2) . ";\n");
ok($node->poll_query_until('postgres', "SELECT wait_event_type='Lock' FROM pg_stat_activity WHERE pid=$pid"), 'writer waits before abort outcome');
$a->query_safe('ROLLBACK');
$b->query_safe('SELECT 1');
$a->quit; $b->quit;
is(readref('heads/probe'), $c2, 'waiting writer succeeds after competitor aborts');


# Independent transactions may store the same immutable bytes without waiting.
$a = $node->background_psql('postgres');
$b = $node->background_psql('postgres');
$a->query_safe('BEGIN');
my $shared_sql = q{SELECT encode(dolt_manifest_put('schema','shared-concurrent'::bytea,ARRAY[]::bytea[]),'hex')};
my $shared = $a->query_safe($shared_sql);
$b->query_safe("SET statement_timeout='2s'");
is($b->query_safe($shared_sql),$shared,'same new immutable object publishes without waiting for creator');
$a->query_safe('COMMIT');
$a->quit; $b->quit;
is(sql("SELECT count(*) FROM pg_dolt_object_visible WHERE objhash=".hasharg($shared)), '1', 'logical object reads deduplicate concurrent copies');
is(sql("SELECT count(*) FROM pg_dolt_object WHERE objhash=".hasharg($shared)), '2', 'physical immutable copies retain independent transaction visibility');
is(sql("SELECT count(*) FROM unnest(ARRAY[".hasharg($shared)."]) h(x) JOIN pg_dolt_object_visible o ON o.objhash=h.x"), '1', 'joined object reads deduplicate concurrent copies');
unlike(sql("EXPLAIN SELECT o.objkind FROM unnest(ARRAY[".hasharg($shared)."]) h(x) JOIN pg_dolt_object_visible o ON o.objhash=h.x"), qr/Aggregate|Unique/, 'joined object reads do not deduplicate the whole store');

# An old RR snapshot cannot adopt an object committed after its snapshot.
$a = $node->background_psql('postgres', on_error_stop => 0);
$a->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_dolt_object');
my $late = put('schema', 'created-after-snapshot');
is($a->query_safe("SELECT kind IS NULL FROM dolt_manifest_get(".hasharg($late).")"), 't', 'object lookup respects old snapshot');
($out, $error) = $a->query(q{SELECT dolt_manifest_put('schema','created-after-snapshot'::bytea,ARRAY[]::bytea[])});
is($error, 0, 'RR may publish its own identical object without adopting an invisible row');
$a->query('ROLLBACK'); $a->quit;

# Name real multi-node table and index content by root, independently of the relations.
sql(q{
CREATE TABLE p(id int PRIMARY KEY, value text COLLATE "C") USING dolt_prolly_table;
CREATE TABLE h(id int);
CREATE TABLE live(id int PRIMARY KEY) USING dolt_prolly_table;
INSERT INTO p SELECT g,repeat(md5(g::text),10) FROM generate_series(1,100) g;
});
# Export uses the statement snapshot, even though object insertion does writes.
$reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $old_root = $reader->query_safe(q{SELECT encode(dolt_hash_decode(dolt_prolly_root('p')),'hex')});
sql(q{UPDATE p SET value='changed after snapshot' WHERE id=1});
my $old_chunk = $reader->query_safe(q{SELECT encode(dolt_manifest_export('p'),'hex')});
is($old_chunk, $old_root, 'export excludes changes outside reader snapshot');
$reader->query_safe('COMMIT'); $reader->quit;
my $root = sql(q{SELECT encode(dolt_hash_decode(dolt_prolly_root('p')),'hex')});
my $chunk = sql(q{SELECT encode(dolt_manifest_export('p'),'hex')});
my $indexchunk = sql(q{SELECT encode(dolt_manifest_export('p_pkey'),'hex')});
is($chunk, $root, 'export names the root by its node hash');
is(stored($chunk), $root, 'the chunk store holds the exported root');
is(sql(q{SELECT encode(dolt_manifest_export('p'),'hex')}), $chunk, 'export deterministic and deduplicated');
sql(q{CREATE TABLE same(LIKE p) USING dolt_prolly_table; INSERT INTO same SELECT * FROM p ORDER BY id DESC});
is(sql(q{SELECT encode(dolt_manifest_export('same'),'hex')}), $chunk, 'export ignores relation identity and insertion history');
sql('DROP TABLE same');

my $database = sql("SELECT encode(dolt_root_put('{table,index}','{public.p,public.p_pkey}',ARRAY[".hasharg($schema).",".hasharg($schema)."],ARRAY[".hasharg($chunk).",".hasharg($indexchunk)."]),'hex')");
my $commit = put('commit', 'author=test;message=captured', $database, $c2);
sql(cas('tags/captured', undef, $commit));
is(sql(q{SELECT count(*) FROM pg_dolt_object WHERE objkind='chunk'}), '0', 'trees are not copied into manifest objects');
my $objects = sql('SELECT count(*) FROM pg_dolt_object');
sql('TRUNCATE p; DROP TABLE p');
is(stored($chunk), $root, 'historical content survives truncate/drop of original relation');
cmp_ok(sql('SELECT count(*) FROM pg_dolt_object'), '>=', $objects, 'all historical objects retained; DROP only publishes a new schema');

# Large opaque payloads exercise heap TOAST separately from prolly page storage.
my $large = sql(q{SELECT encode(dolt_manifest_put('schema',convert_to(string_agg(md5(g::text),''),'UTF8'),ARRAY[]::bytea[]),'hex') FROM generate_series(1,2000) g});
is(sql("SELECT octet_length(payload) FROM dolt_manifest_get(".hasharg($large).")"), '64000', 'large object payload stored through heap TOAST');

# Atomic mixed transactions, prepared outcomes, crash recovery.
my $main_token = token('heads/probe');
my $preparer = $node->background_psql('postgres');
$preparer->query_safe('BEGIN');
my $prepared_db = $preparer->query_safe(q{SELECT encode(dolt_root_put('{namespace}','{prepared}',ARRAY[decode(repeat('11',20),'hex')],'{NULL}'),'hex')});
my $prepared_commit = $preparer->query_safe("SELECT encode(dolt_manifest_put('commit','prepared-history'::bytea,ARRAY[".hasharg($prepared_db).','.hasharg($commit)."]),'hex')");
$preparer->query_safe(cas('heads/probe', $main_token, $prepared_commit));
$preparer->query_safe(cas('working/probe', undef, $prepared_db));
$preparer->query_safe(q{INSERT INTO h VALUES(1); INSERT INTO live VALUES(1); PREPARE TRANSACTION 'manifest_commit'});
$preparer->quit;
is(sql("SELECT kind IS NULL FROM dolt_manifest_get(".hasharg($prepared_commit).")"), 't', 'prepared new history object invisible');
$reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($reader->query_safe(q{SELECT encode(target,'hex') FROM dolt_manifest_read('heads/probe')}), $c2, 'snapshot taken before prepared commit');
is(readref('heads/probe'), $c2, 'prepared HEAD invisible');
is(readref('working/probe'), '', 'prepared working ref invisible');
my $abandoned = $node->background_psql('postgres');
$abandoned->query_safe('BEGIN');
$abandoned->query_safe(cas('tags/abandoned', undef, $commit));
my $aborted_hash = $abandoned->query_safe(q{SELECT encode(dolt_manifest_put('schema','crash-aborted'::bytea,ARRAY[]::bytea[]),'hex')});
$reader->quit;
$node->stop('immediate'); $abandoned->quit;
$node->start;
is(readref('tags/captured'), $commit, 'committed history survives crash');
is(readref('tags/abandoned'), '', 'uncommitted ref absent after crash');
is(sql("SELECT kind IS NULL FROM dolt_manifest_get(".hasharg($aborted_hash).")"), 't', 'crash-aborted immutable object invisible');
is(readref('heads/probe'), $c2, 'prepared ref still invisible after recovery');
failure("SET lock_timeout='100ms'; ".cas('heads/probe', $main_token, $c1), qr/lock timeout/, 'prepared ref lock survives restart');
is(sql('SELECT count(*) FROM live'), '0', 'prepared prolly changes remain invisible');
$reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_dolt_ref');
sql(q{COMMIT PREPARED 'manifest_commit'});
is($reader->query_safe(q{SELECT encode(target,'hex') FROM dolt_manifest_read('heads/probe')}), $c2, 'old snapshot keeps pre-publication HEAD');
is($reader->query_safe(q{SELECT target IS NULL FROM dolt_manifest_read('working/probe')}), 't', 'old snapshot excludes other ref in same publication');
$reader->query_safe('COMMIT'); $reader->quit;
is(readref('heads/probe'), $prepared_commit, 'prepared HEAD commits after recovery');
is(sql("SELECT kind FROM dolt_manifest_get(".hasharg($prepared_commit).")"), 'commit', 'new target object commits with prepared HEAD');
is(readref('working/probe'), $prepared_db, 'working ref commits with HEAD');
is(sql('SELECT id FROM h'), '1', 'ordinary heap shares commit decision');
is(sql('SET enable_seqscan=off; SELECT id FROM live WHERE id=1'), '1', 'prolly table and index share manifest commit decision');
sql('BEGIN; '.cas('tags/prepare_abort', undef, $commit).q{; INSERT INTO h VALUES(2); PREPARE TRANSACTION 'manifest_abort'});
$node->stop('immediate'); $node->start;
sql(q{ROLLBACK PREPARED 'manifest_abort'});
is(readref('tags/prepare_abort'), '', 'prepared abort remains invisible');
is(sql('SELECT count(*) FROM h'), '1', 'prepared abort rolls back heap with ref');
is(stored($chunk), $root, 'historical bytes survive repeated recovery');
is(sql("SELECT octet_length(payload) FROM dolt_manifest_get(".hasharg($large).")"), '64000', 'TOAST object survives recovery');


sql('CREATE ROLE reader');
failure(q{SET ROLE reader; SELECT * FROM pg_dolt_object}, qr/permission denied/, 'objects not a public data oracle');
failure(q{SET ROLE reader; SELECT dolt_manifest_read('heads/probe')}, qr/permission denied/, 'internal functions restricted');
sql('GRANT EXECUTE ON FUNCTION dolt_manifest_read(text) TO reader');
failure(q{SET ROLE reader; SELECT dolt_manifest_read('heads/probe')}, qr/require superuser/, 'C permission check survives accidental execute grant');
sql('CREATE DATABASE other');
is($node->safe_psql('other', q{SELECT string_agg(refname,',' ORDER BY refname) FROM pg_dolt_ref}), 'heads/main,staged/main,working/main', 'new database starts from the initialized template refs only');
# Physical backup includes manifest metadata and historical chunks.
$node->backup('history');
my $restore = PostgreSQL::Test::Cluster->new('manifest_restore');
$restore->init_from_backup($node, 'history');
$restore->start;
is($restore->safe_psql('postgres', q{SELECT encode(target,'hex') FROM dolt_manifest_read('tags/captured')}), $commit, 'physical backup preserves history ref');
is($restore->safe_psql('postgres', "SELECT encode(substr(sha256(dolt_chunk_read(".hasharg($chunk).")),1,20),'hex')"), $root, 'physical backup preserves historical chunks');
$restore->stop;
$node->stop;
done_testing();
