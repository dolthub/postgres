use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('canonical_alter');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sub fails {my($q,$re,$label)=@_;my($s,$o,$e)=$node->psql('postgres',$q);isnt($s,0,$label);like($e,$re,"$label diagnostic");}
sql(q{CREATE ROLE denied; CREATE TABLE t(id int PRIMARY KEY,discard text,v int) USING dolt_prolly_table; INSERT INTO t VALUES(1,'old',10),(2,'two',20); CREATE TABLE bag(a int,b text) USING dolt_prolly_table; INSERT INTO bag VALUES(1,'a'),(1,'b'),(1,'b'); SELECT dolt_commit('-Am','base'); SELECT dolt_branch('other'); SELECT dolt_checkout('other')});
sql(q{CREATE TABLE shifted(trash text NOT NULL,id int PRIMARY KEY,v int UNIQUE); INSERT INTO shifted VALUES('remove',1,10); SELECT dolt_commit('-Am','shift fixture'); SELECT dolt_branch('shiftcopy')});
my $old=sql(q{SELECT commit_hash FROM dolt_log('-n','1')});
my $resources=sql(q{SELECT count(*) FROM pg_dolt_storage});
my $a=$node->background_psql('postgres');my $b=$node->background_psql('postgres');
$b->query_safe(q{SELECT dolt_checkout('other')});
$a->query_safe(q{BEGIN; COMMENT ON TABLE t IS 'main'; COMMENT ON COLUMN t.v IS 'main value'; ALTER TABLE t ADD COLUMN extra text; INSERT INTO t VALUES(3,'third',30,'new')});
$b->query_safe(q{SET statement_timeout='3s'; BEGIN; COMMENT ON TABLE t IS 'other'; ALTER TABLE t ADD COLUMN extra text; INSERT INTO t VALUES(4,'four',40,'other'); COMMIT});
$b->query_safe(q{SELECT dolt_commit('-Am','other schema while main is open')});
is($b->query_safe(q{SELECT count(*),max(extra) FROM t}),'3|other','other branch commits identical schema rewrite while main transaction stays open');
is($a->query_safe(q{SELECT count(*),max(extra) FROM t}),'3|new','main retains own uncommitted schema and rows');
is(sql(q{SELECT count(*) FROM pg_attribute WHERE attrelid='t'::regclass AND attname='extra'}),'0','uncommitted schema hidden from another main session');
$a->query_safe('COMMIT');
is(sql(q{SELECT count(*) FROM pg_dolt_storage}),$resources,'schema revisions allocate no new storage IDs');
is(sql(q{SELECT a.extra,b.extra FROM "postgres/main".public.t a JOIN "postgres/other".public.t b USING(id) WHERE id=1}),'|','same query sees both nullable layouts');
my $reader=$node->background_psql('postgres');
$reader->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t});
my $plan=$node->background_psql('postgres',on_error_stop=>0);
$plan->query_safe(q{PREPARE p AS SELECT * FROM t; EXECUTE p});
# DDL locks the branch's relation as PostgreSQL locks a native one: it waits
# for readers of the same branch and never for sessions on another branch.
my $apid=$a->query_safe('SELECT pg_backend_pid()');
$a->query_until(qr/start_drop/,q{\echo start_drop
BEGIN; ALTER TABLE t DROP COLUMN discard;
});
ok($node->poll_query_until('postgres',"SELECT wait_event_type='Lock' FROM pg_stat_activity WHERE pid=$apid"),'main DROP waits for a reader of main');
$b->query_safe(q{BEGIN; ALTER TABLE t DROP COLUMN extra; UPDATE t SET v=41 WHERE id=4; COMMIT});
is($b->query_safe(q{SELECT * FROM t WHERE id=4}),'4|four|41','other DROP and write proceed during main DROP');
is($reader->query_safe(q{SELECT discard FROM t WHERE id=1}),'old','reader keeps the pre-DROP layout and root until it ends');
$reader->query_safe('COMMIT');$reader->quit;
is($a->query_safe(q{SELECT * FROM t WHERE id=3}),'3|30|new','main DROP rewrites and compacts its own rows');
$a->query_safe('COMMIT');
my($output,$error)=$plan->query('EXECUTE p');is($error,1,'prepared SELECT star rejects changed result type');$plan->quit;
is(sql(qq{SELECT * FROM t AS OF '$old' ORDER BY id}),"1|old|10\n2|two|20",'historical rows retain dropped values and old schema');
is(sql(q{SELECT row_to_json(a),row_to_json(b) FROM "postgres/main".public.t a JOIN "postgres/other".public.t b USING(id) WHERE id=1}),'{"id":1,"v":10,"extra":null}|{"id":1,"discard":"old","v":10}','incompatible branch layouts coexist in one query');
sql(q{ALTER TABLE bag DROP COLUMN b});
is(sql(q{SELECT * FROM bag} ),"1\n1\n1",'DROP coalesces identical rows without losing occurrences');
sql(q{INSERT INTO t VALUES(5,50,'five'); UPDATE t SET v=51 WHERE id=5; DELETE FROM t WHERE id=2});
is(sql(q{SELECT * FROM t WHERE id=5}),'5|51|five','native DML and shifted primary key mapping remain usable');
fails(q{INSERT INTO t VALUES(5,99,'duplicate')},qr/duplicate key/,'unique index rebuilt after rewrite');
sql(q{BEGIN; ALTER TABLE t DROP COLUMN extra; ROLLBACK});
is(sql(q{SELECT extra FROM t WHERE id=5}),'five','rollback restores schema and row payload');
sql(q{BEGIN; SAVEPOINT s; ALTER TABLE t ADD COLUMN gone int; ROLLBACK TO s; INSERT INTO t VALUES(6,60,'six'); COMMIT});
is(sql(q{SELECT extra FROM t WHERE id=6}),'six','savepoint rollback restores writable schema');
is(sql(q{SELECT count(*) FROM (SELECT id FROM t FOR UPDATE) l}),sql(q{SELECT count(*) FROM t}),'branch reads lock rows like any table');
sql(q{BEGIN; ALTER TABLE t DROP COLUMN id; ROLLBACK});
is(sql(q{SELECT count(*) FROM pg_dolt_storage}),$resources,'ADD and DROP reuse the original allocations');
# Overlapping large writes exercise contiguous chunk framing in a shared file.
$a->query_until(qr/large_main/, "\\echo large_main\nBEGIN; UPDATE t SET extra=repeat('x',50000) WHERE id=1;\n");
$b->query_until(qr/large_other/, "\\echo large_other\nBEGIN; UPDATE t SET discard=repeat('y',50000) WHERE id=1;\n");
like($a->query_safe(q{SELECT length(extra) FROM t WHERE id=1}),qr/50000/,'large main write completes with other branch transaction open');
like($b->query_safe(q{SELECT length(discard) FROM t WHERE id=1}),qr/50000/,'large other write keeps its own schema and fragmented chunks');
$a->query_safe('ROLLBACK');$b->query_safe('ROLLBACK');
# TODO: ownership checks for COMMENT and ALTER, once the ownership and ACL
# design settles who owns a versioned table.
is(sql(q{SELECT description FROM pg_description WHERE objoid='t'::regclass AND objsubid=0}),'main','table COMMENT belongs to selected branch');
is($b->query_safe(q{SELECT description FROM pg_description WHERE objoid='t'::regclass AND objsubid=0}),'other','other branch retains its own COMMENT');
sql(q{BEGIN; COMMENT ON TABLE t IS ''; COMMIT});
is(sql(q{SELECT count(*) FROM pg_description WHERE objoid='t'::regclass AND objsubid=0}),'0','empty COMMENT removes metadata like PostgreSQL');
sql(q{COMMENT ON TABLE t IS 'main'; BEGIN; COMMENT ON TABLE t IS NULL; ROLLBACK});
is(sql(q{SELECT description FROM pg_description WHERE objoid='t'::regclass AND objsubid=0}),'main','metadata-only revision rolls back');
$a->query_safe(q{BEGIN; ALTER TABLE bag ADD COLUMN aborted text});
$b->query_safe(q{BEGIN; ALTER TABLE bag DROP COLUMN b; COMMIT});
$a->query_safe('ROLLBACK');
is($b->query_safe(q{SELECT count(*) FROM bag}),'3','other DROP survives concurrent rewrite rollback');
sql(q{ALTER TABLE shifted DROP COLUMN trash; UPDATE shifted SET v=11 WHERE id=1; INSERT INTO shifted VALUES(2,20)});
is(sql(q{SELECT * FROM shifted ORDER BY id}),"1|11\n2|20",'DROP before indexed columns remaps keys and rebuilds indexes');
fails(q{INSERT INTO shifted VALUES(3,20)},qr/duplicate key/,'shifted unique index still enforces values');
my $schema_before=sql(q{SELECT encode(bindschema,'hex') FROM pg_dolt_binding WHERE bindbranch='main' AND bindname='public.shifted'});
my $content_before=sql(q{SELECT encode(m.content,'hex') FROM pg_dolt_ref r CROSS JOIN LATERAL dolt_root_find(r.reftarget,'table','public.shifted') m WHERE refname='working/main'});
# Capture the preceding DML before comparing the canonical content root.
sql(q{SELECT dolt_add('.')});
$content_before=sql(q{SELECT encode(m.content,'hex') FROM pg_dolt_ref r CROSS JOIN LATERAL dolt_root_find(r.reftarget,'table','public.shifted') m WHERE refname='working/main'});
like($content_before,qr/^[0-9a-f]{40}$/,'the working root holds the table\'s content root');
sql(q{ALTER TABLE shifted ADD COLUMN roundtrip text; ALTER TABLE shifted DROP COLUMN roundtrip});
is(sql(q{SELECT encode(bindschema,'hex') FROM pg_dolt_binding WHERE bindbranch='main' AND bindname='public.shifted'}),$schema_before,'ADD then DROP restores byte-identical schema');
is(sql(q{SELECT encode(m.content,'hex') FROM pg_dolt_ref r CROSS JOIN LATERAL dolt_root_find(r.reftarget,'table','public.shifted') m WHERE refname='working/main'}),$content_before,'ADD then DROP restores byte-identical content root');
$a->quit;$b->quit;
sql(q{SELECT dolt_commit('-Am','main altered')});
$node->stop('immediate');$node->start;
is(sql(q{SELECT extra FROM t WHERE id=6}),'six','schema and data rewrite recover together');
is(sql(qq{SELECT discard FROM t AS OF '$old' WHERE id=1}),'old','history survives restart');
$node->backup('alter');my $standby=PostgreSQL::Test::Cluster->new('alter_standby');$standby->init_from_backup($node,'alter',has_streaming=>1);$standby->start;
is($standby->safe_psql('postgres',q{SELECT a.v,b.v FROM "postgres/main".public.t a JOIN "postgres/other".public.t b USING(id) WHERE id=1}),'10|10','standby decodes independent rewritten schemas');
$standby->stop;
my $pending=$node->background_psql('postgres');$pending->query_safe(q{BEGIN; ALTER TABLE t DROP COLUMN extra; UPDATE t SET v=999});
$node->stop('immediate');$pending->quit;$node->start;
is(sql(q{SELECT extra,v FROM t WHERE id=6}),'six|60','crash discards uncommitted schema and rewritten roots');
$node->stop;done_testing();
