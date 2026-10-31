use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('diff_ranges');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->start;
sub sql { $node->safe_psql('postgres',$_[0]); }
sql(q{CREATE TABLE ranged(id int PRIMARY KEY,v text) USING dolt_prolly_table; INSERT INTO ranged VALUES(1,'one')});
sql(q{SELECT dolt_commit('-Am','base')});
sql(q{SELECT dolt_branch('side')});
sql(q{ALTER TABLE ranged ADD COLUMN extra int; INSERT INTO ranged VALUES(2,'two',2)});
sql(q{SELECT dolt_commit('-Am','main edits')});
my $s=$node->background_psql('postgres');
$s->query_safe(q{SELECT dolt_checkout('side')});
$s->query_safe(q{INSERT INTO ranged VALUES(3,'three')});
$s->query_safe(q{SELECT dolt_commit('-Am','side edits')});
$s->quit;
for my $fn ('dolt_diff_summary','dolt_diff_stat','dolt_schema_diff')
{
 is(sql("SELECT count(*) FROM $fn('HEAD~1..HEAD')"),'1',"$fn single range");
 is(sql("SELECT count(*) FROM $fn('HEAD~1..HEAD','ranged')"),'1',"$fn range and table");
 is(sql("SELECT count(*) FROM ((SELECT * FROM $fn('HEAD~1..HEAD') EXCEPT SELECT * FROM $fn('HEAD~1','HEAD')) UNION ALL (SELECT * FROM $fn('HEAD~1','HEAD') EXCEPT SELECT * FROM $fn('HEAD~1..HEAD'))) x"),'0',"$fn range matches explicit revisions");
 is(sql("SELECT count(*) FROM ((SELECT * FROM $fn('side...main','ranged') EXCEPT SELECT * FROM $fn('HEAD~1','HEAD','ranged')) UNION ALL (SELECT * FROM $fn('HEAD~1','HEAD','ranged') EXCEPT SELECT * FROM $fn('side...main','ranged'))) x"),'0',"$fn three-dot uses merge base");
 is(sql("SELECT count(*) FROM $fn('HEAD..HEAD')"),'0',"$fn identical endpoints");
 my($status,$out,$err)=$node->psql('postgres',"SELECT * FROM $fn('main')");
 isnt($status,0,"$fn one argument requires range syntax");
 like($err,qr/revision range/,"$fn range diagnostic");
}
$node->stop;
done_testing();
