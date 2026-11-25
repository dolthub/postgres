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

# Bugs row 291: a transaction's own identity tree versions spill past the
# spill threshold, as its other trees' do. At delta_memory 1MB a transaction
# making hundreds of tables spills its identity pairs, reads them back in its
# own transaction and, once it commits, in another; an abort leaves no pair
# and its files go; a restart after a checkpoint that listed a running
# transaction's identity spill finds every pair it committed; and a 1MB
# standby replays a transaction of a thousand tables.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $settings =
  "delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\ncheckpoint_timeout = 1h";
my $node = PostgreSQL::Test::Cluster->new('identity');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', $settings);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$node->backup('replayed');
my $standby = PostgreSQL::Test::Cluster->new('replayed');
$standby->init_from_backup($node, 'replayed', has_streaming => 1);
$standby->append_conf('postgresql.conf', $settings);
$standby->start;

# made returns a DO block that makes count tables of a prefix, a key and a
# text column each.
sub made
{
	my ($prefix, $count) = @_;
	return
	  "DO \$\$ BEGIN FOR i IN 1 .. $count LOOP EXECUTE format('CREATE TABLE ${prefix}_%s (k int4, v text)', i); END LOOP; END \$\$";
}

# identity_files returns the count of a transaction's identity spill files.
sub identity_files
{
	my ($xid) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_ls_dir('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = 'postgres') || '/delta/spill', true, false) f WHERE f LIKE 'identity-$xid-%'"
	);
}

# A transaction's spilled pairs read back in it and, after its commit, in
# another session.
my $own = $node->background_psql('postgres');
$own->query_safe("SET statement_timeout = '300s'");
my $owner = $own->query_safe('BEGIN; SELECT txid_current()');
$own->query_safe(made('own', 400));
cmp_ok(identity_files($owner), '>', 0,
	"a transaction making 400 tables at 1MB spills its identity pairs");
my $first = $own->query_safe("SELECT 'own_1'::regclass::oid");
my $found = $own->query_safe("SELECT doltgres_test_identity(2, $first, 0)");
isnt($found, 'none', "its own transaction finds a spilled table's pair");
$own->query_safe('INSERT INTO own_1 VALUES (1, \'a\')');
is($own->query_safe('SELECT count(*) FROM own_1'),
	'1', "and writes and reads that table");
$own->query_safe('COMMIT');
$own->quit;
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $first, 0)"),
	$found,
	"after its commit another session finds the same pair");
is($node->safe_psql('postgres', 'SELECT count(*) FROM own_1'),
	'1', "and reads the table's row");

# An abort after a spill leaves no pair, and a later checkpoint removes its
# files.
my $abort = $node->background_psql('postgres');
$abort->query_safe("SET statement_timeout = '300s'");
my $aborted = $abort->query_safe('BEGIN; SELECT txid_current()');
$abort->query_safe(made('aborted', 400));
my $gone = $abort->query_safe("SELECT 'aborted_1'::regclass::oid");
cmp_ok(identity_files($aborted),
	'>', 0, "an aborting transaction's identity pairs spill");
$abort->query_safe('ROLLBACK');
$abort->quit;
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $gone, 0)"),
	'none',
	"after its abort no pair names its table");
$node->safe_psql('postgres', 'CHECKPOINT') for 1 .. 3;
is(identity_files($aborted),
	'0', "and its identity spill files are gone after three checkpoints");
my ($code, $out, $err) = $node->psql('postgres',
	"SELECT count(*) FROM pg_class WHERE relname LIKE 'own\\_%' AND doltgres_test_identity(2, oid, 0) <> 'none'"
);
is( $code . $err . ',' . $out,
	'0,400',
	"and once those checkpoints' saves fold the first transaction's spill, every table it made has its pair"
);

# A checkpoint lists a running transaction's identity spill; after its commit
# and a crash, the restart finds every pair from the listed file.
my $listed = $node->background_psql('postgres');
$listed->query_safe("SET statement_timeout = '300s'");
my $lister = $listed->query_safe('BEGIN; SELECT txid_current()');
$listed->query_safe(made('listed', 400));
cmp_ok(identity_files($lister), '>', 0,
	"a third transaction's identity pairs spill");
my $kept = $listed->query_safe("SELECT 'listed_1'::regclass::oid");
my $kept_id =
  $listed->query_safe("SELECT doltgres_test_identity(2, $kept, 0)");
$node->safe_psql('postgres', 'CHECKPOINT');
$listed->query_safe('COMMIT');
$listed->quit;
$node->stop('immediate');
$node->start;
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $kept, 0)"),
	$kept_id,
	"after a crash the restart finds a pair the checkpoint's listed spill holds"
);
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_class WHERE relname LIKE 'listed\\_%' AND doltgres_test_identity(2, oid, 0) <> 'none'"
	),
	'400',
	"and every table the transaction made has its pair");
$node->safe_psql('postgres', 'CHECKPOINT');
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $kept, 0)"),
	$kept_id,
	"and still does once a checkpoint's save folds them");

for my $round (1 .. 3)
{
	$node->safe_psql('postgres',
		"INSERT INTO own_1 VALUES ($round + 1, 'b'); CHECKPOINT");
}
is(identity_files($lister), '0',
	"and the save's folded identity spill files are gone two checkpoints later"
);

# A 1MB standby replays one transaction of a thousand tables.
$node->safe_psql('postgres',
		"SET statement_timeout = '300s'; BEGIN; "
	  . made('standby', 1000)
	  . "; COMMIT;");
$node->wait_for_replay_catchup($standby);
my $pairs =
  q{SELECT string_agg(relname || '=' || doltgres_test_identity(2, oid, 0), ' ' ORDER BY relname) FROM pg_class WHERE relname LIKE 'standby\_%'};
is( $standby->safe_psql('postgres', $pairs),
	$node->safe_psql('postgres', $pairs),
	"a 1MB standby replays a transaction of a thousand tables, each pair as its primary's"
);

# Another transaction's lookup of a running transaction's spilled pair waits
# for it, as it waits for an unspilled one, and then reads the pair.
my $holder = $node->background_psql('postgres');
$holder->query_safe("SET statement_timeout = '300s'");
my $holding = $holder->query_safe('BEGIN; SELECT txid_current()');
$holder->query_safe(made('held', 400));
cmp_ok(identity_files($holding),
	'>', 0, "a running transaction's identity pairs spill");
my $held = $holder->query_safe("SELECT 'held_1'::regclass::oid");
my $held_id =
  $holder->query_safe("SELECT doltgres_test_identity(2, $held, 0)");
my (undef, undef, $waited) = $node->psql('postgres',
	"SET statement_timeout = '2s'; SELECT doltgres_test_identity(2, $held, 0)"
);
like(
	$waited,
	qr/canceling statement due to statement timeout/,
	"another transaction's lookup of its spilled pair waits for it");
$holder->query_safe('COMMIT');
$holder->quit;
is( $node->safe_psql(
		'postgres', "SELECT doltgres_test_identity(2, $held, 0)"),
	$held_id,
	"and reads the pair once it commits");

# A 1MB standby that restarts at a restartpoint, while a running transaction's
# replayed identity versions sit in its listed spills on both sides of the
# restartpoint's redo point, pushes none of them again as it replays.
my $replaying = $node->background_psql('postgres');
$replaying->query_safe("SET statement_timeout = '300s'");
$replaying->query_safe('BEGIN; SELECT txid_current()');
$replaying->query_safe(made('early', 400));
$node->safe_psql('postgres', 'CHECKPOINT');
$replaying->query_safe(made('late', 400));
my $oids = $replaying->query_safe(
	"SELECT string_agg(oid::text, ',') FROM pg_class WHERE relname LIKE 'early\\_%' OR relname LIKE 'late\\_%'"
);
$node->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
$standby->stop('immediate');
$standby->start;
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql(
		'postgres',
		"SELECT count(*) FROM unnest('{$oids}'::oid[]) o WHERE doltgres_test_identity_versions(2, o) <> '1'"
	),
	'0',
	"a restarted 1MB standby's redo pushes no version its listed spills hold"
);
$replaying->query_safe('COMMIT');
$replaying->quit;

$standby->stop;
$node->stop;
done_testing();
