# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The delta's packing of its blocks for a writer the budget refused (bugs row
# 143): two transactions at once, each writing a row into each of 1,200
# tables at a 1 MB delta_memory, both complete, a spill retiring each emptied
# tuple id table, a packing moving the live range and tuple id table pages out
# of the sparsest blocks, and each waiting writer giving back its tuple id
# reservation while it waits; every row then reads back once, at its own
# ctid.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# tables makes count tables of a prefix in one transaction.
sub tables
{
	my ($node, $prefix, $count) = @_;
	$node->safe_psql('postgres',
		"DO \$\$ BEGIN FOR i IN 1 .. $count LOOP EXECUTE format('CREATE TABLE ${prefix}_%s (k int4, v text)', i); END LOOP; END \$\$",
		timeout => 600);
}

# written is a transaction's statement writing one row into each of count tables of a prefix.
sub written
{
	my ($prefix, $count) = @_;
	return
	  "DO \$\$ BEGIN FOR i IN 1 .. $count LOOP PERFORM set_config('application_name', '${prefix}:' || i, false); EXECUTE format('INSERT INTO ${prefix}_%s VALUES (%s, repeat(''r'', 200))', i, i); END LOOP; END \$\$";
}

my $node = PostgreSQL::Test::Cluster->new('packed');
$node->init;
$node->append_conf('postgresql.conf',
	"max_connections = 100\nfold_threshold = 1GB\nfold_timeout = 3600s");
$node->start;
tables($node, $_, 1200) for 'spilled', 'first', 'second', 'waited';
$node->append_conf('postgresql.conf', 'delta_memory = 1MB');
$node->restart;
$node->safe_psql('postgres',
	"SET statement_timeout = '600s'; " . written('spilled', 1200),
	timeout => 900);

my @sessions;
for my $prefix ('first', 'second')
{
	my $session = $node->background_psql('postgres', timeout => 900);
	$session->query_safe("SET statement_timeout = '600s'");
	$session->query_until(qr/started/,
		"\\echo started\n" . written($prefix, 1200) . ";\n\\echo done\n");
	push @sessions, $session;
}
for my $session (@sessions)
{
	$session->query_until(qr/done/, '');
	unlike($session->{stderr}, qr/ERROR/,
		'a writer of a row into each of 1,200 tables at a 1 MB delta_memory completes');
	$session->quit;
}

my $counts = q{SELECT sum(c) || ' ' || sum(d) FROM (SELECT (xpath('/row/c/text()', x))[1]::text::int8 AS c, (xpath('/row/d/text()', x))[1]::text::int8 AS d FROM pg_class, query_to_xml(format('SELECT count(*) AS c, count(DISTINCT ctid) AS d FROM %I', relname), false, true, '') x WHERE relkind = 'r' AND (relname LIKE 'spilled\_%' OR relname LIKE 'first\_%' OR relname LIKE 'second\_%')) t};
is($node->safe_psql('postgres', $counts),
	'3600 3600', 'two such transactions at once both complete, every row read back once at a distinct ctid');
is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_class, query_to_xml(format('SELECT coalesce((SELECT k FROM %I WHERE ctid = (SELECT ctid FROM %I LIMIT 1)), -1) AS k', relname, relname), false, true, '') x WHERE relkind = 'r' AND relname LIKE 'second\_%' AND (xpath('/row/k/text()', x))[1]::text::int4 = split_part(relname, '_', 2)::int4}
	),
	'1200',
	'each row reads back at its own ctid');

# A writer the budget refuses gives back its tuple id reservation before it
# waits and reserves again after (bugs row 143): paused between the give-back
# and the wait, its storage's tuple id table moves by the packing's own move,
# which a test calls itself since which blocks a packing drains follows the
# region's layout, and the writer then completes, every row reading back once
# at its own ctid.
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test');
$node->safe_psql('postgres', "SELECT injection_points_attach('doltgres-tuple-given', 'wait')");
my $waiter = $node->background_psql('postgres', timeout => 900);
$waiter->query_safe("SET statement_timeout = '600s'");
$waiter->query_until(qr/started/, "\\echo started\n" . written('waited', 1200) . ";\n\\echo done\n");
$node->wait_for_event('client backend', 'doltgres-tuple-given');
$node->safe_psql('postgres', "SELECT injection_points_detach('doltgres-tuple-given')");
my $at = $node->safe_psql('postgres',
	q{SELECT split_part(application_name, ':', 2) FROM pg_stat_activity WHERE wait_event = 'doltgres-tuple-given'});
my $before = $node->safe_psql('postgres', "SELECT doltgres_test_tuples_record('waited_$at')");
is($node->safe_psql('postgres', "SELECT doltgres_test_tuples_moved('waited_$at')"),
	'1', 'a writer paused after giving back its reservation leaves its tuple id table movable');
isnt($node->safe_psql('postgres', "SELECT doltgres_test_tuples_record('waited_$at')"),
	$before, 'and the table moves to a record of its own');
$node->safe_psql('postgres', "SELECT injection_points_wakeup('doltgres-tuple-given')");
$waiter->query_until(qr/done/, '');
unlike($waiter->{stderr}, qr/ERROR/, 'the writer reserves again after its wait and completes');
$waiter->quit;
is( $node->safe_psql(
		'postgres',
		q{SELECT sum(c) || ' ' || sum(d) FROM (SELECT (xpath('/row/c/text()', x))[1]::text::int8 AS c, (xpath('/row/d/text()', x))[1]::text::int8 AS d FROM pg_class, query_to_xml(format('SELECT count(*) AS c, count(DISTINCT ctid) AS d FROM %I', relname), false, true, '') x WHERE relkind = 'r' AND relname LIKE 'waited\_%') t}
	),
	'1200 1200',
	'every row of the paused writer reads back once at a distinct ctid');

$node->stop;

# A packing makes room by construction (bugs row 143): with the pages of four
# one-row tables spread over four blocks of the smallest pages and every
# other block of a 1 MB delta taken, a packing finds pages to move, a 4 KB
# page the budget refuses is given once one packing pass moves them, and no
# block was free before it.
my $spread = PostgreSQL::Test::Cluster->new('spread');
$spread->init;
$spread->append_conf('postgresql.conf', "fold_threshold = 1GB\nfold_timeout = 3600s");
$spread->start;
tables($spread, 'spread', 64);
$spread->append_conf('postgresql.conf', 'delta_memory = 1MB');
$spread->restart;
$spread->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$spread->safe_psql('postgres', written('spread', 64));
$spread->safe_psql('postgres',
	"SELECT count(doltgres_test_fold(format('spread_%s', i)::regclass)) FROM generate_series(1, 64) i WHERE i % 16 <> 1"
);
my ($filled, $packable, $asked) = split /\n/,
  $spread->safe_psql('postgres',
	'SELECT doltgres_test_delta(90, 0, 0); SELECT doltgres_test_delta(92, 0, 0); SELECT doltgres_test_delta(93, 0, 0); SELECT doltgres_test_delta(91, 0, 0)'
  );
cmp_ok($filled, '>', 0, 'whole-block and 4 KB pages take every free block and slot');
cmp_ok($packable, '>', 0, "the tables' pages spread over blocks a packing can empty");
is($asked, '1', 'a 4 KB page the budget refuses is given after one packing pass');
is($spread->safe_psql('postgres', q{SELECT sum(c) FROM (SELECT (xpath('/row/c/text()', query_to_xml(format('SELECT count(*) AS c FROM spread_%s', i), false, true, '')))[1]::text::int8 AS c FROM generate_series(1, 64) i) t}),
	'64', 'every row reads back after the packing');
$spread->stop;
done_testing();
