use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('publication');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "max_prepared_transactions=10\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub refs { sql(q{SELECT string_agg(refname||':'||encode(reftarget,'hex'),',' ORDER BY refname) FROM pg_dolt_ref}); }
sql(q{CREATE TABLE t(id int PRIMARY KEY) USING dolt_prolly_table; INSERT INTO t VALUES(1)});
my $head = sql(q{SELECT dolt_commit('-Am','initial')});
my $base = refs();
my $session = $node->background_psql('postgres');
$session->query_safe(q{BEGIN; SAVEPOINT staged; INSERT INTO t VALUES(2)});
$session->query_safe(q{SELECT dolt_add('.')});
is($session->query_safe(q{SELECT data_change FROM dolt_diff_summary('HEAD','STAGED')}), 't', 'own staged changes visible');
is(refs(), $base, 'other sessions cannot see staged transaction');
$session->query_safe('ROLLBACK TO staged');
is($session->query_safe(q{SELECT count(*) FROM dolt_diff_summary('HEAD','STAGED')}), '0', 'savepoint restores stage');
$session->query_safe('COMMIT');
is(sql('SELECT count(*) FROM t'), '1', 'row and staging rollback together');
$session->query_safe('BEGIN');
$session->query_safe(q{INSERT INTO t VALUES(3)});
$session->query_safe(q{SELECT dolt_add('.')});
$session->query_safe(q{PREPARE TRANSACTION 'stage'});
$session->quit;
is(refs(), $base, 'prepared staging invisible');
$node->stop('immediate');
$node->start;
is(refs(), $base, 'prepared staging invisible after crash');
sql(q{COMMIT PREPARED 'stage'});
is(sql(q{SELECT data_change FROM dolt_diff_summary('HEAD','STAGED')}), 't', 'prepared stage publishes after restart');
is(sql('SELECT string_agg(id::text,\',\' ORDER BY id) FROM t'), '1,3', 'prepared data publishes with stage');
$head = sql(q{SELECT dolt_commit('-m','prepared stage')});

# Block the second ref update: HEAD has been updated privately, but SQL commit
# has not happened. Observe, cancel, and crash at this actual publication boundary.
for my $crash (0, 1)
{
 my $before = refs();
 my $objects = sql('SELECT count(*) FROM pg_dolt_object');
 my $blocker = $node->background_psql('postgres');
 my $writer = $node->background_psql('postgres', on_error_stop => 0);
 $blocker->query_safe(q{BEGIN; SELECT dolt_manifest_cas('staged/main',token,target) FROM dolt_manifest_read('staged/main')});
 my $pid = $writer->query_safe('SELECT pg_backend_pid()');
 $writer->query_until(qr/publishing/, "\\echo publishing\nSELECT dolt_commit('--allow-empty','-m','interrupted');\n");
 ok($node->poll_query_until('postgres', "SELECT wait_event_type='Lock' FROM pg_stat_activity WHERE pid=$pid"), 'publisher reaches blocked second ref');
 is(refs(), $before, 'partially updated refs invisible');
 is(sql('SELECT count(*) FROM pg_dolt_object'), $objects, 'uncommitted immutable objects invisible');
 if ($crash)
 {
  $node->stop('immediate');
  # The blocked client's connection was terminated by the crash.
  $writer->{run}->finish; $blocker->quit;
  $node->start;
 }
 else
 {
  is(sql("SELECT pg_cancel_backend($pid)"), 't', 'cancel publication');
  my ($out, $err) = $writer->query('SELECT 1');
  is($err, 1, 'publisher reports cancellation');
  $blocker->query_safe('ROLLBACK');
  $writer->quit; $blocker->quit;
 }
 is(refs(), $before, 'failed publication restores all refs');
 is(sql('SELECT count(*) FROM pg_dolt_object'), $objects, 'failed publication leaves no visible objects');
 is(sql(q{SELECT commit_hash FROM dolt_log('-n','1')}), $head, 'HEAD remains complete prior commit');
}
# Errors inside capture must unwind snapshots, SPI, GUCs and lock acquisitions.
$session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe(q{SET search_path=public,pg_catalog; SET quote_all_identifiers=on; BEGIN; SAVEPOINT bad; CREATE TABLE damaged(id int) USING dolt_prolly_table});
my ($out, $err) = $session->query(q{DELETE FROM pg_dolt_binding WHERE bindname='public.damaged'; SELECT dolt_add('.')});
is($err, 1, 'capture of a table with a damaged binding fails');
$session->{stderr} = ''; # BackgroundPsql retains diagnostics until cleared.
$session->query_safe('ROLLBACK TO bad');
is($session->query_safe('SHOW search_path'), 'public, pg_catalog', 'error restores search path');
is($session->query_safe('SHOW quote_all_identifiers'), 'on', 'error restores quoting');
$session->query_safe(q{SELECT dolt_add('.')});
$session->query_safe('COMMIT');
$session->quit;
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD','STAGED')}), '0', 'capture succeeds after savepoint recovery');
my ($status, $stdout, $stderr) = $node->psql('postgres', '', extra_params => ['-c', q{SELECT dolt_commit('--allow-empty','-m','implicit block'); SELECT 1}]);
is($status, 0, 'commit ends multi-statement implicit transaction');
like($stdout, qr/[0-9a-v]{32}\n1/, 'subsequent statement runs after commit');
# Publishing parent edits in a savepoint must not rebase the parent's overlay
# permanently: an error while returning the commit result aborts that publish.
sql(q{CREATE TABLE result_rows(id int PRIMARY KEY) USING dolt_prolly_table});
sql(q{SELECT dolt_commit('-Am','result base')});
$session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe(q{SET dolt_transaction_commit=1; BEGIN; INSERT INTO result_rows VALUES(1); SAVEPOINT result_failure});
($out, $err) = $session->query(q{SELECT dolt_commit('-Am','bad result')::integer});
is($err, 1, 'commit result conversion fails');
$session->{stderr} = '';
$session->query_safe('ROLLBACK TO result_failure');
is($session->query_safe('TABLE result_rows'), '1', 'parent insert survives rollback of publication');
$session->query_safe(q{INSERT INTO result_rows VALUES(2); COMMIT});
$session->quit;
is(sql(q{SELECT id FROM result_rows AS OF 'HEAD' ORDER BY id}), "1\n2", 'automatic commit retains both parent and later inserts');
is(sql(q{SELECT count(*) FROM dolt_log WHERE message='bad result'}), '0', 'failed commit leaves no history');
$node->stop;
done_testing();
