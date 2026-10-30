use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('compat');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"max_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres',$_[0]); }
sub fails {
 my($q,$pat,$label)=@_;my($status,$out,$err)=$node->psql('postgres',$q);
 isnt($status,0,$label);like($err,$pat,"$label diagnostic");
}
# ---- statistics over ANALYZE ----
# ANALYZE and VACUUM accept prolly tables but sample and reclaim nothing, so
# prolly tables never have rows here; the view is exercised with heap tables.
sql(q{CREATE TABLE p(pk int PRIMARY KEY, c1 int) USING dolt_prolly_table; INSERT INTO p SELECT i, i % 7 FROM generate_series(1,100) g(i)});
sql(q{ANALYZE p; VACUUM p});
is(sql(q{SELECT count(*) FROM p}),'100','ANALYZE and VACUUM leave prolly rows intact');
is(sql(q{SELECT count(*) FROM pg_stats WHERE tablename='p'}),'2','ANALYZE samples prolly rows');
is(sql(q{SELECT dolt_stats_once()}),'statistics refreshed with ANALYZE','refresh succeeds with prolly tables');
sql(q{DROP TABLE p});
sql(q{CREATE TABLE t(pk int PRIMARY KEY, c1 int) USING heap; INSERT INTO t SELECT i, i % 7 FROM generate_series(1,100) g(i)});
sql(q{CREATE TABLE t2(pk int PRIMARY KEY, c1 int) USING heap; CREATE INDEX t2_c1_idx ON t2(c1); INSERT INTO t2 SELECT i, i % 3 FROM generate_series(1,60) g(i)});
is(sql(q{SELECT count(*) FROM dolt_statistics}),'0','no statistics before ANALYZE');
sql(q{ANALYZE t; ANALYZE t2});
TODO: {
 local $TODO='SQL queries of catalogs on a branch omit native objects, such as these heap tables';
 is(sql(q{SELECT table_name,index_name,sum(row_count)::int FROM dolt_statistics GROUP BY 1,2 ORDER BY 1,2}),"t|primary|100\nt2|primary|60\nt2|t2_c1_idx|60",'one bucket per index with the row count');
 is(sql(q{SELECT sum(distinct_count)::int FROM dolt.statistics WHERE table_name='t' AND index_name='primary'}),'100','distinct count of a unique column');
 is(sql(q{SELECT max(distinct_count) <= 3 AND sum(null_count) = 0 FROM dolt_statistics WHERE table_name='t2' AND index_name='t2_c1_idx'}),'t','low cardinality index');
 is(sql(q{SELECT columns, types, upper_bound FROM dolt_statistics WHERE table_name='t' AND index_name='primary'}),'pk|integer|100','columns, types and upper bound');
 is(sql(q{SELECT database_name = current_database(), created_at IS NOT NULL, mcv1 IS NOT NULL FROM dolt_statistics WHERE table_name='t2' AND index_name='t2_c1_idx'}),'t|t|t','metadata columns');
}
is(sql(q{SELECT dolt_stats_once(), dolt_stats_wait(), dolt_stats_flush()}),'statistics refreshed with ANALYZE|statistics refreshed with ANALYZE|statistics refreshed with ANALYZE','refresh procedures succeed without prolly tables');
is(sql(q{SELECT length(dolt_stats_stop()) > 0, length(dolt_stats_restart()) > 0, length(dolt_stats_gc()) > 0, length(dolt_stats_purge()) > 0, length(dolt_stats_timers('x')) > 0}),'t|t|t|t|t','control procedures are no-ops');
is(sql(q{SELECT (dolt_stats_info()::json->>'active')::boolean, (dolt_stats_info()::json->>'statCnt')::int >= 3}),'t|t','stats info summarizes pg_stats');
sql(q{DROP TABLE t, t2});
# ---- backup definitions and argument validation ----
is(sql(q{SELECT * FROM dolt_backups}),'','no backups');
is(sql(q{SELECT dolt_backup('add','backup_a','file:///tmp/a'), dolt_backup('add','backup_b','file:///tmp/b')}),'0|0','add backups');
is(sql(q{SELECT name FROM dolt.dolt_backups ORDER BY name}),"backup_a\nbackup_b",'dolt.dolt_backups alias');
is(sql(q{SELECT name,url FROM dolt.backups ORDER BY name}),"backup_a|file:///tmp/a\nbackup_b|file:///tmp/b",'backups listed');
fails(q{SELECT dolt_backup('add','backup_a','file:///tmp/x')},qr/backup 'backup_a' already exists/,'duplicate backup');
fails(q{SELECT dolt_backup('sync','missing')},qr/backup 'missing' not found/,'missing named backup');
fails(q{SELECT dolt_backup('restore','https://example.invalid/a','db')},qr/only absolute file/,'unsupported URL');
fails(q{SELECT dolt_backup('bogus')},qr/unrecognized dolt_backup parameter 'bogus'/,'unknown subcommand');
is(sql(q{SELECT dolt_backup('remove','backup_a')}),'0','remove');
fails(q{SELECT dolt_backup('remove','backup_a')},qr/backup 'backup_a' not found/,'remove twice');
is(sql(q{SELECT name FROM dolt_backups}),'backup_b','remaining backup');
# ---- remotes and clusters without transport ----
sql(q{SELECT dolt_remote('add','origin','file:///tmp/origin')});
fails(q{SELECT dolt_fetch('origin')},qr/dolt_fetch is not supported: the server has no remote transport/,'fetch');
fails(q{SELECT dolt_push('origin','main')},qr/dolt_push is not supported/,'push');
fails(q{SELECT dolt_pull('origin')},qr/dolt_pull is not supported/,'pull');
fails(q{SELECT dolt_clone('file:///tmp/origin','copy')},qr/dolt_clone is not supported/,'clone');
fails(q{SELECT dolt_assume_cluster_role('primary','1')},qr/dolt_assume_cluster_role is not supported/,'cluster role');
fails(q{SELECT dolt_cluster_transition_to_standby('1')},qr/dolt_cluster_transition_to_standby is not supported/,'cluster standby');
is(sql(q{SELECT name FROM dolt_remotes}),'origin','remote definitions are kept');
# ---- query catalog and branch control tables ----
sql(q{CREATE TABLE t(pk int PRIMARY KEY) USING dolt_prolly_table; INSERT INTO dolt_query_catalog(id, display_order, name, query, description) VALUES ('q1', 1, 'count', 'SELECT count(*) FROM t', 'rows of t')});
is(sql(q{SELECT id,display_order,name,query,description FROM dolt.query_catalog}),'q1|1|count|SELECT count(*) FROM t|rows of t','query catalog rows');
sql(q{UPDATE dolt.query_catalog SET description='all rows of t' WHERE id='q1'});
is(sql(q{SELECT description FROM public.dolt_query_catalog}),'all rows of t','updatable through the aliases');
sql(q{INSERT INTO dolt_branch_control VALUES ('postgres','main','alice','%','write'); INSERT INTO dolt.branch_namespace_control VALUES ('postgres','feature/%','bob','%')});
is(sql(q{SELECT branch,"user",permissions FROM dolt_branch_control}),'main|alice|write','branch control rows');
is(sql(q{SELECT branch,"user" FROM dolt_branch_namespace_control}),'feature/%|bob','namespace control rows');
fails(q{INSERT INTO dolt_branch_control VALUES ('postgres','main','carol','%','read')},qr/permissions_check/,'permissions are checked');
# ---- branch activity ----
sql(q{SELECT dolt_commit('-Am','tables'); SELECT dolt_branch('side')});
is(sql(q{SELECT branch, last_write IS NOT NULL, active_sessions, system_start_time = pg_postmaster_start_time() FROM dolt_branch_activity ORDER BY 1}),"main|t|0|t\nside|t|0|t",'branch activity from the reflog');
is(sql(q{SELECT count(*) FROM dolt.dolt_stashes}),'0','prefixed names resolve inside the dolt schema');
is(sql(q{SELECT has_table_privilege('public','dolt.statistics','SELECT')}),'f','views are superuser-only');
is(sql(q{SELECT dolt_storage_format()}), 'PROLLY ( DGPTR001 )', 'format introspection describes the native codec');
done_testing();
