use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('row_diff');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"max_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres',$_[0]); }
sub fails {
 my($q,$pat,$label)=@_;my($status,$out,$err)=$node->psql('postgres',$q);
 isnt($status,0,$label);like($err,$pat,"$label diagnostic");
}
sql(q{CREATE TABLE test(id int PRIMARY KEY,val int) USING dolt_prolly_table; INSERT INTO test VALUES(1,1)});
my $c1=sql(q{SELECT dolt_commit('-Am','commit 1','--author','Ann <ann@example.com>')});
sql(q{INSERT INTO test VALUES(2,2)});
my $c2=sql(q{SELECT dolt_commit('-Am','commit 2','--author','Ann <ann@example.com>')});
sql(q{UPDATE test SET val=3 WHERE id=1});
my $c3=sql(q{SELECT dolt_commit('-Am','commit 3')});
# dolt_diff() table function with derived columns.
is(sql(q{SELECT diff_type,from_id,from_val,to_id,to_val FROM dolt_diff('HEAD~1','HEAD','test')}),'modified|1|1|1|3','two-point diff');
is(sql(q{SELECT to_commit,from_commit,to_commit_date IS NOT NULL,from_commit_date IS NOT NULL FROM dolt_diff('HEAD~2','HEAD','test') ORDER BY to_id LIMIT 1}),'HEAD|HEAD~2|t|t','diff reports revision labels and commit dates');
is(sql(q{SELECT diff_type,to_id FROM dolt_diff('HEAD~2..HEAD','test') ORDER BY to_id}),"modified|1\nadded|2",'two-dot range form');
is(sql("SELECT diff_type,to_id FROM dolt_diff('$c1','$c3','public.test') ORDER BY to_id"),"modified|1\nadded|2",'hash revisions and qualified table');
sql(q{INSERT INTO test VALUES(4,4)});
is(sql(q{SELECT diff_type,from_id,to_id,to_commit_date FROM dolt_diff('HEAD','WORKING','test')}),'added||4|','working set diff has no to date');
is(sql(q{SELECT count(*) FROM dolt_diff('STAGED','WORKING','test')}),'1','staged revision');
fails(q{SELECT * FROM dolt_diff('HEAD','WORKING','missing')},qr/table not found: public.missing/,'unknown table');
fails(q{SELECT * FROM dolt_diff('HEAD','WORKING','')},qr/cannot be empty/,'empty table name');
# dolt_diff_<table> partitions and commit filters.
is(sql("SELECT from_id,to_id,to_val,diff_type FROM dolt_diff_test WHERE from_commit='$c2' AND to_commit='$c3'"),'1|1|3|modified','commit pair filter');
is(sql(q{SELECT from_id,to_id,to_val,diff_type FROM dolt_diff_test WHERE to_commit=HASHOF('HEAD~1')}),'|2|2|added','to_commit filter with hashof');
is(sql(q{SELECT to_id,diff_type FROM dolt_diff_test WHERE to_commit IN (HASHOF('HEAD'),HASHOF('HEAD~1')) ORDER BY to_id}),"1|modified\n2|added",'IN filter scans all partitions');
is(sql(q{SELECT to_id FROM dolt_diff_test WHERE to_commit NOT IN (HASHOF('HEAD'),'WORKING') ORDER BY to_id}),"1\n2",'NOT IN excludes working partition');
is(sql(q{SELECT from_id,to_id,diff_type,from_commit=HASHOF('HEAD') FROM dolt_diff_test WHERE to_commit='WORKING'}),'|4|added|t','working partition against head');
is(sql(q{SELECT dolt_diff_test.to_id FROM public.dolt_diff_test WHERE to_commit=HASHOF('HEAD')}),'1','schema-qualified generated table and column qualification');
is(sql(q{SELECT d.to_id FROM dolt_diff_test AS d WHERE d.to_commit=HASHOF('HEAD')}),'1','alias on generated table');
is(sql(q{SELECT count(*) FROM dolt_diff_test}),'4','unfiltered partitions');
is(sql(q{SELECT from_commit=(SELECT commit_hash FROM dolt.commits WHERE message='Initialize data repository') FROM dolt_diff_test WHERE to_commit=HASHOF('HEAD~2')}),'t','first user commit diffs against the initial commit');
fails(q{SELECT * FROM public.dolt_diff_none},qr/does not exist/,'generated table requires its base table');
fails(q{SELECT * FROM other.dolt_diff_test},qr/does not exist/,'generated table requires an existing schema');
# dolt_commit_diff_<table> requires both commits.
is(sql(q{SELECT from_id,to_id,diff_type FROM dolt_commit_diff_test WHERE from_commit=HASHOF('HEAD^1') AND to_commit=HASHOF('HEAD')}),'1|1|modified','commit diff');
is(sql(q{SELECT to_id FROM dolt_commit_diff_test WHERE to_commit=dolt_hashof('HEAD') AND from_commit=dolt_hashof('HEAD~')}),'1','trailing tilde ancestor');
fails(q{SELECT to_id FROM dolt_commit_diff_test},qr/must be filtered to a single 'to_commit'/,'commit diff without filter');
fails(q{SELECT to_id FROM dolt_commit_diff_test WHERE from_commit=HASHOF('HEAD^2') AND to_commit=HASHOF('HEAD^1')},qr/invalid ancestor spec/,'invalid ancestor spec');
# dolt_history_<table>, dolt_blame_<table>, dolt_workspace_<table>.
is(sql(q{SELECT id,val,committer FROM dolt_history_test WHERE commit_hash=HASHOF('HEAD~1') ORDER BY id}),"1|1|Ann\n2|2|Ann",'history rows at a commit');
is(sql(q{SELECT count(*) FROM dolt_history_test}),'5','history rows over all commits');
is(sql(q{SELECT id,message FROM dolt_blame_test ORDER BY id}),"1|commit 3\n2|commit 2",'blame attributes latest change per key');
is(sql("SELECT commit=HASHOF('HEAD'),commit_date IS NOT NULL,committer,email FROM dolt_blame_test WHERE id=1"),'t|t|' . sql('SELECT current_user') . '|' . sql('SELECT current_user') . '@localhost','blame commit metadata');
is(sql("SELECT committer,email FROM dolt_blame_test WHERE id=2"),'Ann|ann@example.com','author also sets the committer unless --committer is given');
is(sql(q{SELECT id,staged,from_id,to_id,diff_type FROM dolt_workspace_test}),'0|f||4|added','workspace unstaged row');
sql(q{SELECT dolt_add('.')});
sql(q{UPDATE test SET val=44 WHERE id=4});
is(sql(q{SELECT id,staged,from_val,to_val,diff_type FROM dolt_workspace_test ORDER BY id}),"0|t||4|added\n1|f|4|44|modified",'workspace staged then unstaged rows');
is(sql(q{SELECT count(*) FROM dolt_workspace_none}),'0','workspace of unknown table is empty');
sql(q{SELECT dolt_commit('-Am','commit 4')});
# Schema-qualified generated tables and search_path resolution.
sql(q{CREATE SCHEMA newschema; CREATE TABLE newschema.test(id int PRIMARY KEY) USING dolt_prolly_table; INSERT INTO newschema.test VALUES(12)});
sql(q{SELECT dolt_commit('-Am','newschema test')});
is(sql(q{SET search_path=newschema; SELECT from_id,to_id,diff_type FROM dolt_diff_test WHERE to_commit=HASHOF('HEAD')}),'|12|added','search_path selects the schema of the base table');
is(sql(q{SET search_path=newschema; SELECT to_id FROM public.dolt_diff_test WHERE to_commit=HASHOF('HEAD~1')}),'4','explicit schema qualification');
is(sql(q{SET search_path=newschema; SELECT id FROM dolt_history_test}),'12','history in another schema');
is(sql(q{SELECT count(*) FROM public.dolt_history_test WHERE commit_hash=HASHOF('HEAD')}),'3','rows at commits that changed other schemas');
is(sql(q{SELECT diff_type,to_id FROM dolt_diff('HEAD~1','HEAD','newschema.test')}),'added|12','qualified table argument');
fails(q{SET search_path=pg_catalog; SELECT * FROM dolt_diff('HEAD~1','HEAD','test')},qr/ambiguous table name/,'ambiguous unqualified table outside the search path');
is(sql(q{SELECT count(*) FROM dolt_diff('HEAD~1','HEAD','test')}),'0','search path gives precedence to its schemas');
is(sql(q{SET search_path=newschema,public; SELECT diff_type,to_id FROM dolt_diff('HEAD~1','HEAD','test')}),'added|12','search_path breaks ties');
# dolt_column_diff and dolt_diff_stat.
is(sql("SELECT table_name,column_name,diff_type FROM dolt.column_diff WHERE commit_hash='$c3'"),'public.test|val|modified','column diff of a modification');
is(sql("SELECT string_agg(column_name,',' ORDER BY column_name),diff_type FROM dolt_column_diff WHERE commit_hash='$c1' GROUP BY diff_type"),'id,val|added','column diff of an added table');
is(sql(q{SELECT count(*) FROM dolt.column_diff WHERE commit_hash IN ('STAGED','WORKING')}),'0','clean working set has no column rows');
sql(q{INSERT INTO public.test VALUES(5,5)});
is(sql(q{SELECT commit_hash,table_name,column_name FROM dolt_column_diff WHERE commit_hash='WORKING' ORDER BY column_name}),"WORKING|public.test|id\nWORKING|public.test|val",'working column rows');
is(sql(q{SELECT * FROM dolt_diff_stat('HEAD','WORKING')}),'public.test|3|1|0|0|2|0|0|3|4|6|8','diff stat for an insert');
is(sql("SELECT rows_unmodified,rows_added,rows_deleted,rows_modified,cells_modified FROM dolt_diff_stat('$c2','$c3','test')"),'1|0|0|1|1','diff stat for a modification');
is(sql(q{SELECT count(*) FROM dolt_diff_stat('HEAD','HEAD')}),'0','identical revisions');
# Keyless tables diff by occurrence.
sql(q{CREATE TABLE kl(v int) USING dolt_prolly_table; INSERT INTO kl VALUES(1),(1),(2)});
sql(q{SELECT dolt_commit('-Am','keyless')});
sql(q{DELETE FROM kl WHERE v=1; INSERT INTO kl VALUES(3)});
is(sql(q{SELECT diff_type,from_v,to_v FROM dolt_diff('HEAD','WORKING','kl') ORDER BY 1,2,3}),"added||3\nremoved|1|\nremoved|1|",'keyless occurrences');
is(sql(q{SELECT rows_added,rows_deleted,old_row_count,new_row_count FROM dolt_diff_stat('HEAD','WORKING','kl')}),'1|2||','keyless stat uses Dolt unknown-total contract');
fails(q{SELECT * FROM dolt_blame_kl},qr/requires a primary key/,'blame requires a key');
# Branch views keep generated tables working.
sql(q{SELECT dolt_commit('-Am','before branch')});
my $s=$node->background_psql('postgres');
$s->query_safe(q{SELECT dolt_checkout('-b','feature')});
$s->query_safe(q{UPDATE public.test SET val=100 WHERE id=1});
is($s->query_safe(q{SELECT diff_type,from_val,to_val FROM dolt_diff_test WHERE to_commit='WORKING'}),'modified|3|100','working diff on a branch');
is($s->query_safe(q{SELECT id,staged,diff_type FROM public.dolt_workspace_test}),'0|f|modified','workspace on a branch');
$s->query_safe(q{SELECT dolt_commit('-Am','feature change')});
is($s->query_safe(q{SELECT id,message FROM dolt_blame_test WHERE id=1}),'1|feature change','blame on a branch');
$s->quit;
is(sql(q{SELECT id,message FROM dolt_blame_test WHERE id=1}),'1|commit 3','main blame unchanged');
# Schema rewrites should not manufacture logical row modifications.
sql(q{CREATE TABLE stat_schema(id int PRIMARY KEY,v int) USING dolt_prolly_table});
sql(q{SELECT dolt_commit('-Am','empty statistics table')});
is(sql(q{SELECT count(*) FROM dolt_diff_stat('HEAD~1','HEAD','stat_schema')}),'0','empty creation has no data statistics');
is(sql(q{SELECT data_change,schema_change FROM dolt_diff_summary('HEAD~1','HEAD','stat_schema')}),'f|t','empty creation changes schema only');
sql(q{INSERT INTO stat_schema VALUES(1,10),(2,20)});
sql(q{SELECT dolt_commit('-Am','statistics rows')});
sql(q{ALTER TABLE stat_schema ADD COLUMN extra int});
is(sql(q{SELECT rows_unmodified,rows_modified,cells_added FROM dolt_diff_stat('HEAD','WORKING','stat_schema')}),'2|0|2','adding NULL column changes cells but not rows');
is(sql(q{SELECT data_change,schema_change FROM dolt_diff_summary('HEAD','WORKING','stat_schema')}),'f|t','NULL column changes schema only');
sql(q{UPDATE stat_schema SET extra=7 WHERE id=1});
is(sql(q{SELECT rows_unmodified,rows_modified,cells_added,cells_modified FROM dolt_diff_stat('HEAD','WORKING','stat_schema')}),'1|1|2|1','non-NULL added value modifies one row and counts structural cells');
is(sql(q{SELECT data_change,schema_change FROM dolt_diff_summary('HEAD','WORKING','stat_schema')}),'t|t','non-NULL new column changes data and schema');
sql(q{SELECT dolt_commit('-Am','statistics new column')});
sql(q{UPDATE stat_schema SET extra=NULL WHERE id=1});
sql(q{SELECT dolt_commit('-Am','clear added column')});
sql(q{ALTER TABLE stat_schema DROP COLUMN extra});
is(sql(q{SELECT rows_unmodified,rows_modified,cells_deleted FROM dolt_diff_stat('HEAD','WORKING','stat_schema')}),'2|0|2','dropping NULL column changes cells but not rows');
sql(q{CREATE TABLE stat_empty(id int PRIMARY KEY) USING dolt_prolly_table});
sql(q{SELECT dolt_commit('-Am','empty to drop')});
sql(q{DROP TABLE stat_empty});
is(sql(q{SELECT count(*) FROM dolt_diff_stat('HEAD','WORKING','stat_empty')}),'0','empty deletion has no data statistics');
is(sql(q{SELECT data_change,schema_change FROM dolt_diff_summary('HEAD','WORKING','stat_empty')}),'f|t','empty deletion changes schema only');

sql(q{ALTER TABLE stat_schema DROP CONSTRAINT stat_schema_pkey; ALTER TABLE stat_schema ADD PRIMARY KEY(id,v)});
my ($stat_status,$stat_out,$stat_err)=$node->psql('postgres',q{SELECT * FROM dolt_diff_stat('HEAD','WORKING','stat_schema')});
isnt($stat_status,0,'named table rejects changed primary key');
like($stat_err,qr/primary key set changed/,'primary key diagnostic');
($stat_status,$stat_out,$stat_err)=$node->psql('postgres',q{SELECT rows_added,rows_modified FROM dolt_diff_stat('HEAD','WORKING') WHERE table_name='public.stat_schema'});
is($stat_status,0,'all-table statistics continue after key change');
is($stat_out,'0|0','unavailable key-change statistics use zero row');
like($stat_err,qr/WARNING:.*primary key set changed/,'all-table key change emits warning');

($stat_status,$stat_out,$stat_err)=$node->psql('postgres',q{SELECT * FROM dolt_diff_summary('HEAD','WORKING','stat_schema')});
isnt($stat_status,0,'named summary rejects changed primary key');
like($stat_err,qr/primary key set changed/,'summary key diagnostic');
($stat_status,$stat_out,$stat_err)=$node->psql('postgres',q{SELECT count(*) FROM dolt_diff_summary('HEAD','WORKING') WHERE to_table_name='public.stat_schema'});
is($stat_status,0,'all-table summary continues after key change');
is($stat_out,'0','all-table summary omits incompatible key change');
like($stat_err,qr/WARNING:.*primary key set changed/,'all-table summary warns on key change');
$node->stop;
done_testing();
