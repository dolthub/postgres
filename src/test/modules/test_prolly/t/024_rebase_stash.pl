use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('rebasestash');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"max_parallel_workers_per_gather=0\n");
$node->start;
sub sql { $node->safe_psql('postgres',$_[0]); }
sub fails {
 my($q,$pat,$label)=@_;my($status,$out,$err)=$node->psql('postgres',$q);
 isnt($status,0,$label);like($err,$pat,"$label diagnostic");
}
# History: main has row 0; branch1 diverges with rows 1, 2, 3 (Doltgres "dolt rebase" scenario).
sql(q{CREATE TABLE t(pk int PRIMARY KEY, v text) USING dolt_prolly_table; SELECT dolt_commit('-Am','creating table t'); SELECT dolt_branch('branch1'); INSERT INTO t VALUES (0,'zero'); SELECT dolt_commit('-am','inserting row 0')});
my $s=$node->background_psql('postgres',on_error_stop=>0);
sub ok_sql { my($q,$exp,$label)=@_; is($s->query_safe($q),$exp,$label); }
sub bad_sql { my($q,$pat,$label)=@_; $s->{stderr}=''; $s->query($q); like($s->{stderr},$pat,$label); $s->{stderr}=''; }
$s->query_safe(q{SELECT dolt_checkout('branch1')});
$s->query_safe(q{INSERT INTO t VALUES (1,'one'); SELECT dolt_commit('-am','inserting row 1'); INSERT INTO t VALUES (2,'two'); SELECT dolt_commit('-am','inserting row 2'); INSERT INTO t VALUES (3,'three'); SELECT dolt_commit('-am','inserting row 3')});
# ---- rebase table visibility and plan ----
bad_sql(q{SELECT * FROM dolt_rebase},qr/table not found: dolt_rebase/,'no plan table outside a rebase');
bad_sql(q{SELECT * FROM dolt.rebase},qr/table not found: rebase/,'dolt.rebase also absent');
bad_sql(q{SELECT dolt_rebase('--continue')},qr/no rebase in progress/,'continue without a rebase');
bad_sql(q{SELECT dolt_rebase('main')},qr/non-interactive rebases not currently supported/,'only interactive rebases');
bad_sql(q{SELECT dolt_rebase('-i')},qr/not enough args/,'upstream required');
$s->query_safe(q{INSERT INTO t VALUES (9,'dirty')});
bad_sql(q{SELECT dolt_rebase('-i','main')},qr/cannot start a rebase with uncommitted changes/,'dirty working set');
$s->query_safe(q{SELECT dolt_reset('--hard')});
ok_sql(q{SELECT dolt_rebase('-i','main')},q{(0,"interactive rebase started on branch dolt_rebase_branch1; adjust the rebase plan in the dolt_rebase table, then continue rebasing by calling dolt_rebase('--continue')")},'rebase starts');
ok_sql(q{SELECT active_branch()},'dolt_rebase_branch1','session moved to the temporary branch');
ok_sql(q{SELECT rebase_order,action,commit_message FROM dolt_rebase ORDER BY rebase_order},"1|pick|inserting row 1\n2|pick|inserting row 2\n3|pick|inserting row 3",'default plan picks every commit');
ok_sql(q{SELECT rebase.commit_message FROM dolt.rebase ORDER BY rebase_order},"inserting row 1\ninserting row 2\ninserting row 3",'dolt.rebase alias');
ok_sql(q{SELECT count(*) FROM public.dolt_rebase},'3','schema-qualified alias');
ok_sql(q{SELECT count(*) FROM t},'1','temporary branch starts at the upstream');
bad_sql(q{SELECT dolt_rebase('-i','main')},qr/another rebase is in progress/,'no nested rebase');
bad_sql(q{SELECT dolt_rebase('-i','main')},qr/another rebase is in progress/,'repeat');
$s->query_safe(q{UPDATE dolt.rebase SET action='reword', commit_message='insert rows' WHERE rebase_order=1});
$s->query_safe(q{UPDATE dolt.rebase SET action='drop' WHERE rebase_order=2});
$s->query_safe(q{UPDATE dolt_rebase SET action='fixup' WHERE rebase_order=3});
ok_sql(q{SELECT rebase_order,action,commit_message FROM dolt_rebase ORDER BY rebase_order},"1|reword|insert rows\n2|drop|inserting row 2\n3|fixup|inserting row 3",'plan edited through both aliases');
$s->query_safe(q{UPDATE dolt_rebase SET action='squash' WHERE rebase_order=1});
bad_sql(q{SELECT dolt_rebase('--continue')},qr/squash and fixup actions must appear after a pick or reword action/,'plan validation');
$s->query_safe(q{UPDATE dolt_rebase SET action='bogus' WHERE rebase_order=1});
bad_sql(q{SELECT dolt_rebase('--continue')},qr/rebase action 'bogus' is not supported/,'unknown action');
$s->query_safe(q{UPDATE dolt_rebase SET action='reword' WHERE rebase_order=1});
ok_sql(q{SELECT dolt_rebase('--continue')},'(0,"Successfully rebased and updated refs/heads/branch1")','rebase completes');
ok_sql(q{SELECT active_branch()},'branch1','back on the original branch');
ok_sql(q{SELECT message FROM dolt_log},"insert rows\ninserting row 0\ncreating table t\nInitialize data repository",'rewritten history');
ok_sql(q{SELECT pk FROM t ORDER BY pk},"0\n1\n3",'live table carries the rebased rows');
ok_sql(q{SELECT count(*) FROM dolt_status},'0','clean after rebase');
ok_sql(q{SELECT string_agg(name,',' ORDER BY name) FROM dolt_branches},'branch1,main','temporary branch removed');
bad_sql(q{SELECT * FROM dolt_rebase},qr/table not found: dolt_rebase/,'plan table gone');
ok_sql(q{SELECT count(*) FROM pg_catalog.pg_dolt_rebase},'0','plan rows cleared');
# Squash keeps both messages; abort restores the branch.
$s->query_safe(q{INSERT INTO t VALUES (4,'four'); SELECT dolt_commit('-am','four'); INSERT INTO t VALUES (5,'five'); SELECT dolt_commit('-am','five')});
$s->query_safe(q{SELECT dolt_rebase('-i','main')});
$s->query_safe(q{UPDATE dolt_rebase SET action='squash' WHERE rebase_order=3});
ok_sql(q{SELECT dolt_rebase('--continue')::text},'(0,"Successfully rebased and updated refs/heads/branch1")','squash rebase');
ok_sql(q{SELECT replace(message,E'\n','|') FROM dolt_log LIMIT 2},"four||five\ninsert rows",'squash joins the messages');
$s->query_safe(q{SELECT dolt_rebase('-i','main')});
ok_sql(q{SELECT dolt_rebase('--abort')::text},'(0,"Interactive rebase aborted")','abort');
ok_sql(q{SELECT active_branch(), count(*) FROM pg_catalog.pg_dolt_rebase},'branch1|0','abort restores the session');
ok_sql(q{SELECT pk FROM t ORDER BY pk},"0\n1\n3\n4\n5",'abort leaves the branch intact');
bad_sql(q{SELECT dolt_rebase('--abort')},qr/no rebase in progress/,'nothing to abort');
# Conflicting picks abort with Dolt's message.
$s->query_safe(q{SELECT dolt_checkout('main'); UPDATE t SET v='main-one' WHERE pk=0; SELECT dolt_commit('-am','main edits 0'); SELECT dolt_checkout('branch1'); UPDATE t SET v='branch-one' WHERE pk=0; SELECT dolt_commit('-am','branch edits 0')});
$s->query_safe(q{SELECT dolt_rebase('-i','main')});
bad_sql(q{SELECT dolt_rebase('--continue')},qr/data conflict detected while rebasing commit [0-9a-v]{32} \(branch edits 0\)/,'conflicting commit');
$s->query_safe(q{SELECT dolt_rebase('--abort')});
# ---- stash ----
$s->query_safe(q{SELECT dolt_checkout('main')});
ok_sql(q{SELECT pk FROM t ORDER BY pk},'0','main contents');
bad_sql(q{SELECT dolt_stash('push','mine')},qr/No local changes to save/,'clean stash push');
bad_sql(q{SELECT dolt_stash('push')},qr/Must provide valid subcommand and stash name/,'name required');
bad_sql(q{SELECT dolt_stash('frob','mine')},qr/unknown stash subcommand frob/,'unknown subcommand');
bad_sql(q{SELECT dolt_stash('push','mine','1')},qr/Push takes only subcommand and stash name/,'push arity');
$s->query_safe(q{INSERT INTO t VALUES (10,'ten'); UPDATE t SET v='edited' WHERE pk=0; CREATE TABLE u(pk int PRIMARY KEY) USING dolt_prolly_table});
ok_sql(q{SELECT dolt_stash('push','mine')},'0','stash push');
ok_sql(q{SELECT pk||'|'||v FROM t ORDER BY pk},'0|main-one','tracked changes are stashed');
ok_sql(q{SELECT * FROM dolt_status},'public.u|f|new table','untracked tables stay');
ok_sql(q{SELECT name,stash_id,branch,hash=dolt_hashof('main'),commit_message LIKE 'WIP on main: % main edits 0' FROM dolt_stashes},'mine|stash@{0}|main|t|t','stashes table');
$s->query_safe(q{INSERT INTO t VALUES (11,'eleven')});
ok_sql(q{SELECT dolt_stash('push','mine')},'0','second stash');
ok_sql(q{SELECT stash_id FROM dolt.stashes ORDER BY stash_id},"stash\@{0}\nstash\@{1}",'newest stash is stash@{0}');
ok_sql(q{SELECT dolt_stash('pop','mine','1')},'0','pop the older stash');
ok_sql(q{SELECT pk||'|'||v FROM t ORDER BY pk},"0|edited\n10|ten",'popped changes applied');
ok_sql(q{SELECT count(*) FROM dolt_stashes},'1','popped stash removed');
ok_sql(q{SELECT dolt_stash('apply','mine')},'0','apply keeps the stash');
ok_sql(q{SELECT pk||'|'||v FROM t ORDER BY pk},"0|edited\n10|ten\n11|eleven",'applied changes merged');
ok_sql(q{SELECT count(*) FROM dolt_stashes},'1','apply keeps the entry');
ok_sql(q{SELECT dolt_stash('drop','mine')},'0','drop');
ok_sql(q{SELECT count(*) FROM dolt_stashes},'0','dropped');
bad_sql(q{SELECT dolt_stash('pop','mine')},qr/stash@\{0\} is not a valid reference in stash mine/,'missing stash');
$s->query_safe(q{SELECT dolt_stash('push','a'); INSERT INTO t VALUES (12,'twelve'); SELECT dolt_stash('push','a')});
ok_sql(q{SELECT dolt_stash('clear','a')},'0','clear');
ok_sql(q{SELECT count(*) FROM dolt_stashes},'0','cleared');
$s->quit;
# ---- dolt_query_diff ----
sql(q{INSERT INTO t VALUES (20,'twenty'),(21,'twenty-one')});
is(sql(q{SELECT * FROM dolt_query_diff('SELECT pk, v FROM t WHERE pk < 21 ORDER BY pk', 'SELECT pk, v FROM t WHERE pk > 0 ORDER BY pk')}),"0|main-one|||deleted\n||21|twenty-one|added",'whole-row differences with from_/to_ columns');
is(sql(q{SELECT from_total, to_total, diff_type FROM dolt_query_diff('SELECT count(*) AS total FROM t WHERE false', 'SELECT count(*) AS total FROM t') ORDER BY diff_type}),"|3|added\n0||deleted",'aggregate rows differ');
is(sql(q{SELECT count(*) FROM dolt_query_diff('SELECT * FROM t', 'SELECT * FROM t')}),'0','identical queries');
fails(q{SELECT * FROM dolt_query_diff('DELETE FROM t', 'SELECT 1')},qr/query must be a SELECT statement/,'only SELECT statements');
fails(q{SELECT * FROM dolt_query_diff('SELECT 1')},qr/expected 2 arguments, 1 received/,'arity');
# ---- dolt_help ----
is(sql(q{SELECT type, synopsis FROM dolt_help WHERE name='dolt_rebase'}),'procedure|dolt_rebase(-i upstream | --continue | --abort)','help rows');
is(sql(q{SELECT (arguments->>'-b') IS NOT NULL FROM dolt.help WHERE name='dolt_checkout'}),'t','help arguments are json');
is(sql(q{SELECT count(*) > 30 FROM dolt_help}),'t','help covers the implemented commands');
# ---- dolt_tests and dolt_test_run ----
is(sql(q{SELECT * FROM dolt_tests}),'','no tests yet');
sql(q{INSERT INTO dolt_tests VALUES ('rows','grp','SELECT * FROM t','expected_rows','>','1'),('cols','grp','SELECT pk FROM t','expected_columns','==','1'),('one',NULL,'SELECT count(*) FROM t','expected_single_value','>=','100'),('bad',NULL,'SELECT * FROM nope','expected_rows','==','0'),('multi',NULL,'SELECT * FROM t','expected_single_value','==','1')});
is(sql(q{SELECT test_name,test_group_name,status,message FROM dolt_test_run()}),"bad||FAIL|Query error: relation nope does not exist on branch main\nmulti||FAIL|expected_single_value expects exactly one cell. Received multiple columns\none||FAIL|Assertion failed: expected_single_value greater than or equal to 100, got 3\ncols|grp|PASS|\nrows|grp|PASS|",'test results and messages');
is(sql(q{SELECT test_name,status FROM dolt_test_run('grp')}),"cols|PASS\nrows|PASS",'run a group');
is(sql(q{SELECT status FROM dolt_test_run('one')}),'FAIL','run one test');
is(sql(q{SELECT count(*) FROM dolt_test_run('*')}),'5','star runs everything');
fails(q{SELECT * FROM dolt_test_run('missing')},qr/could not find tests for argument: missing/,'unknown selector');
fails(q{INSERT INTO dolt.tests VALUES ('x',NULL,'SELECT 1','bogus','==','1')},qr/assertion_type_check/,'assertion types are checked');
sql(q{DELETE FROM dolt.tests WHERE test_name='bad'});
is(sql(q{SELECT count(*) FROM dolt_tests}),'4','tests table is writable through the aliases');
is(sql(q{SELECT has_table_privilege('public','dolt.tests','SELECT')}),'f','tables are superuser-only');
done_testing();
