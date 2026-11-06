use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('catalog_binding');
$node->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"max_parallel_workers_per_gather=0\n");
$node->start;
sub sql {$node->safe_psql('postgres',$_[0]);}
sub fails {my($query,$pattern,$label)=@_;my($status,$out,$err)=$node->psql('postgres',$query);isnt($status,0,$label);like($err,$pattern,"$label diagnostic");}
is(sql(q{SELECT to_regprocedure('dolt_catalog_materialize(text)') IS NULL AND to_regclass('pg_catalog.pg_dolt_projection') IS NULL}),'t','materializer and persistent projection catalog removed');
sql(q{CREATE ROLE reader IN ROLE pg_read_all_data; CREATE TABLE t(id int PRIMARY KEY,v int) USING dolt_prolly_table; INSERT INTO t VALUES(1,10)});
my $old=sql(q{SELECT dolt_commit('-Am','integer schema')});my $short=substr($old,0,12);
my $catalogs=sql('SELECT (SELECT count(*) FROM pg_class),(SELECT count(*) FROM pg_namespace)');
is(sql("BEGIN READ ONLY; SELECT * FROM t AS OF '$old'; SELECT pg_current_xact_id_if_assigned() IS NULL; COMMIT"),"1|10\nt",'first-ever historical query is read-only and assigns no XID');
is(sql(qq{SELECT * FROM "postgres\@$short".public.t}),'1|10','abbreviated commit qualifier cold lookup');
my $reader=$node->background_psql('postgres');
$reader->query_safe("PREPARE old_plan AS SELECT v FROM t AS OF '$old'");
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($reader->query_safe('EXECUTE old_plan'),'10','retained descriptor and root bound');
sql(q{SET lock_timeout='1s'; DROP TABLE t; CREATE TABLE t(id int PRIMARY KEY,v text COLLATE "C",note text) USING dolt_prolly_table; INSERT INTO t VALUES(1,'ten','new schema')});
is(sql(q{SELECT v,note FROM "postgres/main".public.t}),'ten|new schema','branch-qualified name reads the branch working schema, never committed HEAD');
my $new=sql(q{SELECT dolt_commit('-Am','text schema')});
is($reader->query_safe('EXECUTE old_plan'),'10','live DDL does not invalidate old descriptor');$reader->query_safe('COMMIT');$reader->quit;
is(sql("SELECT a.v,b.v,b.note,pg_typeof(a.v),pg_typeof(b.v) FROM t AS OF '$old' a JOIN t AS OF '$new' b USING(id)"),'10|ten|new schema|integer|text','incompatible schemas in one query');
is(sql("SELECT (a).* FROM t AS OF '$old' a"),'1|10','whole-row reference retains descriptor');
is(sql("SET ROLE reader; BEGIN READ ONLY; SELECT v FROM t AS OF '$old'; COMMIT"),'10','ordinary reader needs no preparation or elevated privilege');
# TODO: history read privileges, once the ownership and ACL design decides
# what versioned storage records about roles.
is(sql('SELECT (SELECT count(*) FROM pg_class),(SELECT count(*) FROM pg_namespace)'),$catalogs,'queries create no persistent catalog objects');
is(sql("SET ROLE reader; CREATE TEMP TABLE pg_dolt_object(fake int); CREATE TEMP TABLE pg_dolt_ref(fake int); SELECT v FROM t AS OF '$old'"),'10','temporary names cannot shadow privileged catalog lookups');
sql('DROP TABLE t');
is(sql("SET ROLE reader; SELECT * FROM public.t AS OF '$old'"),'1|10','dropped table readable with retained permissions');
$node->stop('immediate');$node->start;
is(sql("SET ROLE reader; BEGIN READ ONLY; SELECT v FROM public.t AS OF '$old'; COMMIT"),'10','first access after restart needs no preparation');
$node->backup('cold_history');my $standby=PostgreSQL::Test::Cluster->new('standby');$standby->init_from_backup($node,'cold_history',has_streaming=>1);$standby->start;
is($standby->safe_psql('postgres',"SET ROLE reader; SELECT v FROM public.t AS OF '$new'"),'ten','cold standby resolves an unprepared historical schema');$standby->stop;
sql(q{CREATE TABLE bulk(id int) USING dolt_prolly_table; INSERT INTO bulk SELECT generate_series(1,500)});
my $bulk=sql(q{SELECT dolt_commit('-Am','bulk')});
my $explain=sql("EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF) SELECT * FROM bulk AS OF '$bulk' LIMIT 1");
like($explain,qr/Seq Scan on bulk/,'revision reads plan as an ordinary scan');
is(sql("SELECT count(*) FROM bulk AS OF '$bulk'"),'500','lazy traversal reads all leaves when requested');
is(sql("SELECT count(*) FROM bulk AS OF '$bulk' a JOIN bulk AS OF '$bulk' b USING(id)"),'500','independent scan state and joins');

# A concurrent heap update forces EvalPlanQual to reuse the historical whole row.
sql('CREATE TABLE epq_target(id int PRIMARY KEY,v int) USING heap; INSERT INTO epq_target VALUES(1,0)');
my $locker=$node->background_psql('postgres');
my $updater=$node->background_psql('postgres');
my $upid=$updater->query_safe('SELECT pg_backend_pid()');
$locker->query_safe('BEGIN; UPDATE epq_target SET v=5');
$updater->query_until(qr/updating/, "\\echo updating\nUPDATE epq_target t SET v=t.v+h.id FROM bulk AS OF '$bulk' h WHERE t.id=h.id;\n");
ok($node->poll_query_until('postgres', "SELECT wait_event_type='Lock' FROM pg_stat_activity WHERE pid=$upid"),'historical UPDATE waits on concurrent target');
$locker->query_safe('COMMIT');
$updater->query_safe('SELECT 1');
is(sql('SELECT v FROM epq_target'),'6','EPQ keeps historical source and rechecks new target tuple');
$locker->quit;$updater->quit;sql('DROP TABLE epq_target');

fails("CREATE VIEW unsupported_history AS SELECT * FROM bulk AS OF '$bulk'",qr/revision-qualified references in versioned schema objects/,'a view cannot persist a revision binding');
fails("SELECT * FROM bulk AS OF GREATEST(NULL,NULL)",qr/non-null constant/,'null revision expression rejected');
is(sql("SELECT count(*) FROM bulk AS OF LEAST('$bulk','$bulk')"),'500','immutable revision expression receives SQL collation');
sql(q{CREATE TABLE versions(id int PRIMARY KEY,v int) USING dolt_prolly_table; INSERT INTO versions VALUES(1,10)});
my $first=sql(q{SELECT dolt_commit('-Am','version one')});sql(q{SELECT dolt_tag('moving')});
sql('UPDATE versions SET v=20');my $second=sql(q{SELECT dolt_commit('-Am','version two')});
my $plans=$node->background_psql('postgres');
$plans->query_safe(q{PREPARE moving_plan AS SELECT v FROM versions AS OF 'moving'});
is($plans->query_safe('EXECUTE moving_plan'),'10','prepared named revision initial binding');
$plans->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($plans->query_safe('EXECUTE moving_plan'),'10','repeatable-read reference pinned');
sql("SELECT dolt_manifest_cas('tags/moving',(SELECT reftoken FROM pg_dolt_ref WHERE refname='tags/moving'),dolt_hash_decode('$second'))");
is($plans->query_safe('EXECUTE moving_plan'),'10','ref movement cannot escape old snapshot');
$plans->query_safe('COMMIT');is($plans->query_safe('EXECUTE moving_plan'),'20','new transaction rebinds named revision');
$plans->query_safe('DISCARD PLANS');is($plans->query_safe('EXECUTE moving_plan'),'20','discarded plans require no warming');
sql(q{SELECT dolt_branch('other')});
$plans->query_safe(q{PREPARE branch_plan AS SELECT v FROM "postgres/main".public.versions});
$plans->query_safe(q{SELECT dolt_checkout('other')});
$plans->query_safe(q{UPDATE versions SET v=30});
is($plans->query_safe('EXECUTE branch_plan'),'20','session checkout does not redirect explicitly bound branch');
is($plans->query_safe(q{SELECT a.v,b.v FROM "postgres/main".public.versions a JOIN "postgres/other".public.versions b USING(id)}),'20|30','two working roots coexist in one query');
$plans->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');is($plans->query_safe('EXECUTE branch_plan'),'20','working branch uses SQL snapshot');
my $writer=$node->background_psql('postgres');$writer->query_safe('BEGIN');$writer->query_safe('UPDATE versions SET v=21');
is($plans->query_safe('EXECUTE branch_plan'),'20','uncommitted root invisible');$writer->query_safe('COMMIT');
is($plans->query_safe('EXECUTE branch_plan'),'20','committed root outside old snapshot invisible');
$plans->query_safe('COMMIT');is($plans->query_safe('EXECUTE branch_plan'),'21','next snapshot sees published working root');
$writer->quit;$plans->quit;

# VACUUM and ANALYZE of the physical catalogs change only pg_class statistics:
# a session keeps its catalog view. A real change of a pg_class row's
# definition is still seen.
my $binder=$node->background_psql('postgres');
$binder->query_safe('SELECT count(*) FROM versions');
my $loads=$binder->query_safe('SELECT dolt_catalog_view_loads()');
sql('VACUUM pg_dolt_lock; ANALYZE pg_dolt_root; VACUUM ANALYZE pg_statistic');
$binder->query_safe('SELECT count(*) FROM versions');
is($binder->query_safe('SELECT dolt_catalog_view_loads()'),$loads,'catalog statistics updates leave the catalog view loaded');
sql('CREATE TABLE renamed_heap(a int) USING heap');
$binder->query_safe('SELECT count(*) FROM renamed_heap');
sql('ALTER TABLE renamed_heap RENAME TO renamed_heap2');
is($binder->query_safe(q{SELECT to_regclass('renamed_heap2') IS NOT NULL, to_regclass('renamed_heap') IS NULL}),'t|t','a renamed relation is seen by its new name');
is($binder->query_safe('SELECT count(*) FROM versions'),'1','and versioned tables still read');
$binder->quit;sql('DROP TABLE renamed_heap2');
$node->stop;done_testing();
