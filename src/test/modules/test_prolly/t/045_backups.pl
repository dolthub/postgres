# Native bundles preserve canonical history and dirty branch working sets,
# rebuilding local storage without importing physical catalog identifiers.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('backups');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', qq{default_table_access_method='dolt_prolly_table'
autovacuum=off
});
$node->start;
my $directory = PostgreSQL::Test::Utils::tempdir();
my $url = "file://$directory/snapshot";
sub sql { $node->safe_psql('postgres', $_[0]); }
sub restored { $node->safe_psql('restored', $_[0]); }

# Keep an allocator whose definition exists only in historical commits.
sql(q{CREATE SEQUENCE historical;
 SELECT nextval('historical');
 SELECT dolt_commit('-Am','old sequence');
 SELECT dolt_tag('old_sequence');
 SELECT nextval('historical');
 ALTER SEQUENCE historical INCREMENT 10;
 SELECT nextval('historical');
 SELECT dolt_commit('-Am','new sequence')});
sql(q{CREATE TABLE t(id int PRIMARY KEY, v text NOT NULL);
 INSERT INTO t VALUES(1,'main');
 CREATE SEQUENCE s;
 SELECT nextval('s');
 SELECT dolt_commit('-Am','seed');
 SELECT dolt_branch('feature');
 SELECT dolt_checkout('feature');
 INSERT INTO t VALUES(2,'dirty feature');
 SELECT nextval('s');
 SELECT dolt_checkout('main');
 INSERT INTO t VALUES(3,'staged main');
 SELECT dolt_add('.');
 INSERT INTO t VALUES(4,'dirty main')});
my $head = sql(q{SELECT dolt_hashof('HEAD')});
is(sql("SELECT dolt_backup('sync-url','$url')"), '0', 'write native bundle');
is(sql("SELECT dolt_backup('restore','$url','restored')"), '0', 'restore into new database');
is(restored(q{SELECT dolt_hashof('HEAD')}), $head, 'commit identity is unchanged');
is(restored(q{SELECT string_agg(id::text,',' ORDER BY id) FROM t}), '1,3,4', 'dirty working data survives');
is(restored(q{SELECT string_agg(name,',' ORDER BY name) FROM dolt.branches}), 'feature,main', 'all branches survive');
is(restored(q{SELECT status FROM dolt_checkout('feature'); SELECT string_agg(id::text,',' ORDER BY id) FROM t}), "0\n1,2", 'inactive branch dirty data survives');
is(restored(q{SELECT nextval('s')}), '3', 'shared sequence allocation survives');
is(restored(q{SELECT status FROM dolt_checkout('-b','old','old_sequence'); SELECT nextval('historical')}), "0\n3", 'historical sequence definition retains shared allocation');
is(restored(q{SELECT count(*) FROM t AS OF 'HEAD'}), '1', 'historical rows survive');
my ($status, $stdout, $stderr) = $node->psql('postgres', "SELECT dolt_backup('restore','$url','restored')");
isnt($status, 0, 'duplicate restore fails');
like($stderr, qr/already exists/, 'duplicate diagnostic');
restored(q{INSERT INTO t VALUES(99,'discard')});
is(sql("SELECT dolt_backup('restore','--force','$url','restored')"), '0', 'force restore replaces database');
is(restored(q{SELECT count(*) FROM t WHERE id=99}), '0', 'force discards previous data');
$node->restart;
is(restored(q{SELECT string_agg(id::text,',' ORDER BY id) FROM t}), '1,3,4', 'restored state survives restart');

# The bundle also works in an independent cluster with unrelated local OIDs.
my $other = PostgreSQL::Test::Cluster->new('destination');
$other->init(extra => ['--encoding=UTF8', '--locale=C']);
$other->start;
$other->safe_psql('postgres', "CREATE TABLE unrelated(i int)");
$other->safe_psql('postgres', "SELECT dolt_backup('restore','$url','copy')");
is($other->safe_psql('copy', q{SELECT dolt_hashof('HEAD')}), $head, 'cross-cluster commit identity preserved');
is($other->safe_psql('copy', q{SELECT string_agg(id::text,',' ORDER BY id) FROM t}), '1,3,4', 'cross-cluster rows restored');
$other->stop;

# Damage the bundle: force restore must leave the existing target intact.
open(my $file, '+<', "$directory/snapshot/manifest.bundle") or die $!;
seek($file, -1, 2) or die $!;
print $file "!";
close($file);
($status, $stdout, $stderr) = $node->psql('postgres', "SELECT dolt_backup('restore','--force','$url','restored')");
isnt($status, 0, 'corrupt restore rejected');
like($stderr, qr/checksum/, 'corruption diagnosed');
is(restored(q{SELECT count(*) FROM t}), '3', 'failed force restore preserves target');
is(sql(q{SELECT count(*) FROM pg_database WHERE datname LIKE 'dolt_restore_%'}), '0', 'failed staging database removed');
$node->stop;
done_testing();
