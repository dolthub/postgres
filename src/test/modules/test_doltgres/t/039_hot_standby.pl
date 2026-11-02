# A hot standby of a primary with prolly storage replays its versioned tables
# and history, and serves reads of them: queries, history, diffs, and any
# branch, which a session may check out. Version-control writes are refused
# there as any write is in a read-only transaction. A promoted standby takes
# them again.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary=PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming=>1,extra=>['--encoding=UTF8','--locale=C']);
$primary->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$primary->start;
$primary->safe_psql('postgres',q{CREATE TABLE vals(i int PRIMARY KEY, v text); INSERT INTO vals VALUES (1, 'one'), (2, 'two');
SELECT dolt_commit('-Am', 'first'); SELECT dolt_branch('feature');
UPDATE vals SET v = 'uno' WHERE i = 1; SELECT dolt_commit('-am', 'second')});
$primary->backup('backup');
my $standby=PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary,'backup',has_streaming=>1);
$standby->start;
sub standby_sql { return $standby->safe_psql('postgres',shift) }
sub standby_fails {
 my ($query,$error,$name)=@_;
 my ($rc,$out,$err)=$standby->psql('postgres',$query);
 like($err,$error,$name);
}

# A session open on the standby sees what replay brings in, statement by statement.
my $reader=$standby->background_psql('postgres');
is($reader->query_safe(q{SELECT count(*) FROM vals}),'2','a standby session reads the rows replayed so far');
$primary->safe_psql('postgres',q{INSERT INTO vals VALUES (10, 'ten')});
$primary->wait_for_replay_catchup($standby);
is($reader->query_safe(q{SELECT count(*) FROM vals}),'3','and later rows once they replay');
$primary->safe_psql('postgres',q{DELETE FROM vals WHERE i = 10});
$primary->wait_for_replay_catchup($standby);
is($reader->query_safe(q{SELECT count(*) FROM vals}),'2','and their deletion');
$reader->quit;

# Writes made after the standby was copied replay too, schema changes included.
$primary->safe_psql('postgres',q{INSERT INTO vals VALUES (3, 'three'); CREATE TABLE later(a int PRIMARY KEY); INSERT INTO later VALUES (7);
CREATE SEQUENCE counter START WITH 10 INCREMENT BY 3; SELECT nextval('counter');
SELECT dolt_checkout('feature'); INSERT INTO vals VALUES (4, 'feature'); SELECT dolt_commit('-am', 'on feature')});
$primary->wait_for_replay_catchup($standby);
is(standby_sql(q{SELECT string_agg(i || v, ',' ORDER BY i) FROM vals}),'1uno,2two,3three','a standby reads versioned tables');
is(standby_sql(q{SELECT a FROM later}),'7','and tables created since it was copied');
is(standby_sql(q{SELECT string_agg(message, ',' ORDER BY date DESC) FROM dolt_log}),'second,first,Initialize data repository','and the branch\'s history');
is(standby_sql(q{SELECT v FROM vals AS OF 'HEAD~1' WHERE i = 1}),'one','and earlier revisions');
is(standby_sql(q{SELECT count(*) FROM dolt_diff('HEAD~1', 'HEAD', 'vals')}),'1','and diffs');
# As natively, a standby sees a sequence's state as logged, which runs ahead of the values handed out.
is(standby_sql(q{SELECT last_value >= 10, (SELECT last_value FROM pg_sequences WHERE sequencename = 'counter') >= 10 FROM counter}),'t|t','and sequences\' state');
is(standby_sql(q{SELECT dolt_checkout('feature'); SELECT active_branch(); SELECT string_agg(i::text, ',' ORDER BY i) FROM vals}) =~ s/^[^\n]*\n//sr,"feature\n1,2,4",'a session on a standby checks out another branch and reads it');

# Version-control writes are refused as other writes are.
standby_fails(q{INSERT INTO vals VALUES (9, 'x')},qr/cannot execute INSERT in a read-only transaction/,'a standby refuses writes');
standby_fails(q{SELECT nextval('counter')},qr/cannot execute nextval\(\) in a read-only transaction/,'and advancing a sequence');
for my $call ("dolt_add('.')","dolt_commit('--allow-empty', '-m', 'x')","dolt_branch('other')","dolt_checkout('-b', 'other')","dolt_tag('t1')","dolt_merge('feature')","dolt_reset('--hard')","dolt_branch('-d', 'feature')") {
 my ($function)=$call=~/^(\w+)/;
 standby_fails("SELECT $call",qr/cannot execute $function\(\) in a read-only transaction/,"a standby refuses $function");
}
my ($ro_rc,$ro_out,$ro_err)=$primary->psql('postgres',q{BEGIN READ ONLY; SELECT dolt_commit('--allow-empty', '-m', 'x')});
like($ro_err,qr/cannot execute dolt_commit\(\) in a read-only transaction/,'so does a primary\'s read-only transaction');

# A promoted standby takes writes and versions them.
$primary->stop;
$standby->promote;
$standby->safe_psql('postgres',q{INSERT INTO vals VALUES (5, 'five'); SELECT dolt_commit('-am', 'after promotion')});
is(standby_sql(q{SELECT message FROM dolt_log ORDER BY date DESC LIMIT 1}),'after promotion','a promoted standby versions writes');
$standby->stop;
done_testing();
