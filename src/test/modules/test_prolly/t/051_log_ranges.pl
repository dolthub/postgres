use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('log_ranges');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub messages { sql("SELECT string_agg(message,',' ORDER BY message) FROM dolt_log($_[0]) WHERE message <> 'Initialize data repository'"); }
sql(q{SELECT dolt_commit('--allow-empty','-m','base')});
sql(q{SELECT dolt_branch('side')});
sql(q{SELECT dolt_commit('--allow-empty','-m','main-only')});
my $s = $node->background_psql('postgres');
$s->query_safe(q{SELECT dolt_checkout('side')});
$s->query_safe(q{SELECT dolt_commit('--allow-empty','-m','side-one')});
$s->query_safe(q{SELECT dolt_commit('--allow-empty','-m','side-two')});
$s->quit;
is(messages(q{'main..side'}),'side-one,side-two','two-dot range excludes shared ancestors');
is(messages(q{'side..main'}),'main-only','range direction');
is(messages(q{'main...side'}),'main-only,side-one,side-two','symmetric difference');
is(messages(q{'main','side'}),'base,main-only,side-one,side-two','multiple starts deduplicate ancestors');
is(messages(q{'side','--not','main'}),'side-one,side-two','explicit exclusion');
is(messages(q{'side','^main'}),'side-one,side-two','caret exclusion');
is(sql(q{SELECT count(*) FROM dolt_log('^main','^side')}),'0','only exclusions yield no commits');
is(sql(q{SELECT count(*) FROM dolt_log('main...main')}),'0','identical sides have empty symmetric difference');
is(sql(q{SELECT count(*) FROM dolt_log('main','-n','0')}),'0','zero limit');
is(sql(q{SELECT count(*) FROM dolt_log('main','--min-parents','2')}),'0','no merges yet');
sql(q{SELECT dolt_merge('side','--no-ff','-m','merge')});
is(messages(q{'--merges'}),'merge','merge filter');
is(messages(q{'--min-parents','1','--merges'}),'merge','merges overrides parent count');
is(sql(q{SELECT count(*) FROM dolt_log('--parents','--merges') WHERE cardinality(string_to_array(parents,', '))=2}),'1','merge has two parents');
is(sql(q{SELECT parents IS NOT NULL AND signature='' FROM dolt_log WHERE message='merge'}),'t','system table always populates parents and signature');
is(sql(q{SELECT refs FROM dolt_log('--decorate','full','-n','1')}),'HEAD -> refs/heads/main','full ref decoration');
is(sql(q{SELECT refs FROM dolt_log('--decorate','no','-n','1')}),'','no ref decoration');
is(sql(q{SELECT count(*) FROM dolt_log('main..side')}),'0','merge makes side reachable');
for my $args (q{'main..side','main'}, q{'main','--not','^side'}, q{'^main..side'}, q{'--min-parents','-1'})
{
 my ($status, $out, $err) = $node->psql('postgres', "SELECT * FROM dolt_log($args)");
 isnt($status, 0, "reject malformed arguments $args");
}
sql(q{CREATE TABLE log_a(id int PRIMARY KEY) USING dolt_prolly_table});
sql(q{SELECT dolt_commit('-Am','create-a')});
sql(q{CREATE TABLE log_b(id int PRIMARY KEY) USING dolt_prolly_table});
sql(q{SELECT dolt_commit('-Am','create-b')});
sql(q{SELECT dolt_branch('table-side')});
my $side=$node->background_psql('postgres');
$side->query_safe(q{SELECT dolt_checkout('table-side')});
$side->query_safe(q{INSERT INTO log_a VALUES(1)});
$side->query_safe(q{SELECT dolt_commit('-Am','side-a')});
$side->query_safe(q{CREATE TABLE log_c(id int PRIMARY KEY) USING dolt_prolly_table});
$side->query_safe(q{SELECT dolt_commit('-Am','create-c')});
$side->quit;
sql(q{INSERT INTO log_a VALUES(2)});
sql(q{SELECT dolt_commit('-Am','main-a')});
sql(q{SELECT dolt_merge('table-side')});
is(sql(q{SELECT message FROM dolt_log('--merges','-n','1')}),q{Merge branch 'table-side' into main},'default merge message includes target');
is(sql(q{SELECT count(*) FROM dolt_log('--tables','log_a')}),'4','table filter includes changes from both parents and merged result');
is(sql(q{SELECT count(*) FROM dolt_log('--tables','log_c')}),'1','unchanged table inherited from a merge parent is not a new edit');
is(messages(q{'--tables','public.log_b'}),'create-b','qualified table filter');
is(sql(q{SELECT count(*) FROM dolt_log('--tables','log_a,log_b','-n','2')}),'2','limit is applied after table filtering');
is(sql(q{SELECT count(*) FROM dolt_log('--tables','missing_table')}),'0','unknown table has no history');
sql(q{DROP TABLE log_c});
sql(q{SELECT dolt_commit('-Am','drop-c')});
is(messages(q{'--tables','log_c'}),'create-c,drop-c','table filter includes dropped tables');
$node->stop;
done_testing();
