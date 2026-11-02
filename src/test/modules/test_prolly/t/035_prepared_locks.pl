# Prepared transactions keep prolly row locks and unique-key reservations.
# With prepared transactions enabled, reservations are lock records, which
# outlive the preparing backend and a restart.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('prepared_locks');
$node->init(extra => [ '--encoding=UTF8', '--locale=C' ]);
$node->append_conf('postgresql.conf',
	"max_prepared_transactions=2\ndefault_table_access_method=dolt_prolly_table\n");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE TABLE t(id int PRIMARY KEY, v int); INSERT INTO t VALUES (3, 30)});

# A prepared transaction keeps its row locks and key reservations through a
# crash, and publishes its changes when committed.
sql(q{BEGIN; UPDATE t SET v = 300 WHERE id = 3; INSERT INTO t VALUES (30, 300); PREPARE TRANSACTION 'locks'});
$node->stop('immediate');
$node->start;
my ($code, $out, $err) = $node->psql('postgres', q{SET lock_timeout = '500ms'; UPDATE t SET v = 0 WHERE id = 3});
like($err, qr/lock timeout/, 'a recovered prepared transaction holds its row lock');
($code, $out, $err) = $node->psql('postgres', q{SET lock_timeout = '500ms'; INSERT INTO t VALUES (30, 0)});
like($err, qr/lock timeout/, 'and its key reservation');
sql(q{COMMIT PREPARED 'locks'});
is(sql('SELECT string_agg(v::text, \',\' ORDER BY id) FROM t WHERE id IN (3, 30)'), '300,300',
	'committing it publishes its changes');

# Rolling back a recovered prepared transaction releases both.
sql(q{BEGIN; UPDATE t SET v = 301 WHERE id = 3; INSERT INTO t VALUES (31, 310); PREPARE TRANSACTION 'undo'});
$node->stop('immediate');
$node->start;
sql(q{ROLLBACK PREPARED 'undo'});
sql(q{SET lock_timeout = '500ms'; UPDATE t SET v = 302 WHERE id = 3; INSERT INTO t VALUES (31, 311)});
is(sql('SELECT string_agg(v::text, \',\' ORDER BY id) FROM t WHERE id IN (3, 31)'), '302,311',
	'rolling it back releases its row lock and key reservation');

# ON CONFLICT waits for a prepared inserter's reservation like any other.
sql(q{BEGIN; INSERT INTO t VALUES (40, 400); PREPARE TRANSACTION 'conflict'});
($code, $out, $err) = $node->psql('postgres', q{SET lock_timeout = '500ms'; INSERT INTO t VALUES (40, 0) ON CONFLICT DO NOTHING});
like($err, qr/lock timeout/, 'ON CONFLICT waits for a prepared inserter of its key');
sql(q{COMMIT PREPARED 'conflict'});
sql(q{INSERT INTO t VALUES (40, 0) ON CONFLICT DO NOTHING});
is(sql('SELECT v FROM t WHERE id = 40'), '400', 'and then finds its committed row');

# Known issue: a prepared transaction holds its tables' root rows until it
# ends, so every other commit to those tables waits, even one touching other
# rows. Readers and other tables are unaffected.
sql(q{CREATE TABLE u(id int PRIMARY KEY)});
sql(q{BEGIN; INSERT INTO t VALUES (50, 500); PREPARE TRANSACTION 'root'});
($code, $out, $err) = $node->psql('postgres', q{SET lock_timeout = '500ms'; INSERT INTO t VALUES (51, 510)});
like($err, qr/lock timeout/, 'a commit to a prepared transaction\'s table waits for it');
($code, $out, $err) = $node->psql('postgres',
	q{SET lock_timeout = '500ms'; BEGIN; INSERT INTO t VALUES (52, 520); PREPARE TRANSACTION 'second'});
like($err, qr/lock timeout/, 'and so does a second PREPARE');
is(sql('SELECT count(*) FROM t WHERE id >= 50'), '0', 'readers do not wait');
sql(q{SET lock_timeout = '500ms'; INSERT INTO u VALUES (1)});
pass('a commit to another table does not wait');
sql(q{ROLLBACK PREPARED 'root'});
sql(q{SET lock_timeout = '500ms'; INSERT INTO t VALUES (51, 510)});
is(sql('SELECT count(*) FROM t WHERE id >= 50'), '1', 'ending the prepared transaction releases the table');

$node->stop;
done_testing();
