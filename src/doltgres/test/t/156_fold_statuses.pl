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

# A standby folds each of its primary's fold points to the primary's base,
# deciding every version's writer as the primary's fold decided it, from the
# statuses the fold logged (bugs row 432). Before, a standby folding a point
# after a writer the primary's fold read as in progress had ended read the
# writer's later outcome, so its free space tree differed and the standby's
# recovery stopped.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $settings =
  "fold_threshold = 1GB\nfold_timeout = 3600s\nmax_prepared_transactions = 10";

# replayed waits until the standby has replayed the primary's WAL so far.
sub replayed
{
	my ($primary) = @_;
	my $lsn = $primary->lsn('insert');
	$primary->poll_query_until('postgres',
		"SELECT replay_lsn >= '$lsn' FROM pg_stat_replication")
	  or die 'the standby never replayed the primary';
	return;
}

# pair makes a primary with table t of ten committed rows and a standby that
# folds only when asked, the standby's settings the primary's unless given.
sub pair
{
	my ($name, $more, $standby_more) = @_;
	my $primary = PostgreSQL::Test::Cluster->new("primary_$name");
	$primary->init(allows_streaming => 1);
	$primary->append_conf('postgresql.conf', $settings . ($more // ''));
	$primary->start;
	$primary->safe_psql('postgres',
		'CREATE EXTENSION doltgres_test; CREATE TABLE t (id int4, v text)');
	$primary->backup('paired');
	my $standby = PostgreSQL::Test::Cluster->new("standby_$name");
	$standby->init_from_backup($primary, 'paired', has_streaming => 1);
	$standby->append_conf('postgresql.conf',
		$settings . ($standby_more // $more // ''));
	$standby->start;
	$primary->safe_psql('postgres',
		"INSERT INTO t SELECT g, 'committed ' || g FROM generate_series(1, 10) g");
	return ($primary, $standby);
}

# matched folds the standby at the primary's fold point it replayed, checks
# that its recovery goes on with the primary's free space entry of block 0,
# then promotes it and returns its rows.
sub matched
{
	my ($primary, $standby, $what) = @_;
	my $entry = q{SELECT doltgres_test_free_space('t', 0)};
	my $primary_entry = $primary->safe_psql('postgres', $entry);

	replayed($primary);
	my ($ret, $out, $err) =
	  $standby->psql('postgres', q{SELECT doltgres_test_fold('t')});
	my $folded = $ret == 0
	  && !$standby->log_contains(qr/standby's fold does not match its primary's/);
	ok($folded,
		"$what: the standby folds the primary's point to the primary's base");
	$primary->stop;
	return 'the standby stopped' unless $folded;
	is($standby->safe_psql('postgres', $entry),
		$primary_entry, "$what: block 0's free space entry is the primary's");
	$standby->promote;
	return $standby->safe_psql('postgres',
		q{SELECT count(*) || ' ' || string_agg(id::text, ',' ORDER BY id) FROM t});
}

my $committed = '10 1,2,3,4,5,6,7,8,9,10';

# (h) A prepared transaction the primary's fold read in progress, rolled back
# before the standby folds the point.
my ($primary, $standby) = pair('rolled_back_prepared');
$primary->safe_psql('postgres',
	"BEGIN; INSERT INTO t VALUES (21, 'p'), (22, 'p'); PREPARE TRANSACTION 'p'");
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->safe_psql('postgres', "ROLLBACK PREPARED 'p'");
is(matched($primary, $standby, 'ROLLBACK PREPARED after the fold'),
	$committed, 'ROLLBACK PREPARED after the fold: every committed row reads back');
$standby->stop('immediate', fail_ok => 1);

# (h) An open transaction the primary's fold read in progress, rolled back
# before the standby folds the point.
($primary, $standby) = pair('rolled_back_open');
my $open = $primary->background_psql('postgres');
$open->query_safe("BEGIN; INSERT INTO t VALUES (21, 'o'), (22, 'o')");
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$open->query_safe('ROLLBACK');
$open->quit;
is(matched($primary, $standby, 'ROLLBACK after the fold'),
	$committed, 'ROLLBACK after the fold: every committed row reads back');
$standby->stop('immediate', fail_ok => 1);

# (c) A prepared transaction whose newest version is its subtransaction's,
# rolled back before the standby folds the point.
($primary, $standby) = pair('rolled_back_sub');
$primary->safe_psql('postgres',
	"BEGIN; INSERT INTO t VALUES (21, 's'); SAVEPOINT s; INSERT INTO t VALUES (22, 's'); PREPARE TRANSACTION 's'"
);
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->safe_psql('postgres', "ROLLBACK PREPARED 's'");
is(matched($primary, $standby, 'a subtransaction rolled back with its top'),
	$committed,
	'a subtransaction rolled back with its top: every committed row reads back');
$standby->stop('immediate', fail_ok => 1);

# (g) A subtransaction committed into its running top, which commits after the
# fold; every row reads back.
($primary, $standby) = pair('sub_committed');
$open = $primary->background_psql('postgres');
$open->query_safe(
	"BEGIN; INSERT INTO t VALUES (21, 'g'); SAVEPOINT s; INSERT INTO t VALUES (22, 'g'); RELEASE SAVEPOINT s"
);
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$open->query_safe('COMMIT');
$open->quit;
is( matched($primary, $standby, 'a subtransaction committed into its running top'),
	'12 1,2,3,4,5,6,7,8,9,10,21,22',
	'a subtransaction committed into its running top: every row reads back');
$standby->stop('immediate', fail_ok => 1);

# (f) A subtransaction rolled back inside a top still running at the fold; this
# guards the logged abort of a rolled-back subtransaction and passed before.
($primary, $standby) = pair('sub_rolled_back');
$open = $primary->background_psql('postgres');
$open->query_safe(
	"BEGIN; INSERT INTO t VALUES (21, 'f'); SAVEPOINT s; INSERT INTO t VALUES (22, 'f'); ROLLBACK TO SAVEPOINT s"
);
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$open->query_safe('COMMIT');
$open->quit;
is(matched($primary, $standby, 'a subtransaction rolled back in a running top'),
	'11 1,2,3,4,5,6,7,8,9,10,21',
	'a subtransaction rolled back in a running top: every row reads back');
$standby->stop('immediate', fail_ok => 1);

# logged returns the descriptions of the statuses records the primary's folds
# logged since a position, joined by '|', once a checkpoint flushed them.
sub logged
{
	my ($primary, $since) = @_;
	$primary->safe_psql('postgres', 'CHECKPOINT');
	return $primary->safe_psql('postgres',
		"SELECT string_agg(description, '|' ORDER BY start_lsn) FROM pg_get_wal_records_info('$since', 'FFFFFFFF/FFFFFFFF') WHERE resource_manager = 'Versioned2' AND record_type = 'FoldStatuses'"
	);
}

# aborted sums the writers the statuses records since a position logged aborted.
sub aborted
{
	my ($primary, $since) = @_;
	my $sum = 0;
	$sum += $_ for (logged($primary, $since) // '') =~ /(\d+) aborted/g;
	return $sum;
}

# A running top whose live subtransaction's version the fold reads first and
# 1,000 rolled-back siblings: a fold while the top runs reads none of them,
# the first fold after its commit logs and drops each once, and its standby
# matches.
($primary, $standby) = pair('siblings');
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$open = $primary->background_psql('postgres');
$open->query_safe(
	"BEGIN; SAVEPOINT live; INSERT INTO t VALUES (21, 'live'); RELEASE SAVEPOINT live");
$open->query_safe(
	q{DO $$BEGIN FOR i IN 1..1000 LOOP BEGIN INSERT INTO t VALUES (100 + i, 'rolled back'); RAISE EXCEPTION 'r'; EXCEPTION WHEN raise_exception THEN NULL; END; END LOOP; END$$}
);
my $since = $primary->lsn('insert');
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
is(aborted($primary, $since), 0,
	'a fold while the top runs logs none of the 1,000 rolled-back siblings');
$open->query_safe('COMMIT');
$open->quit;
$since = $primary->lsn('insert');
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
is(aborted($primary, $since), 1000,
	'the first fold after the commit logs each of the 1,000 rolled-back siblings once');
is( matched($primary, $standby, '1,000 rolled-back siblings of a live subtransaction'),
	'11 1,2,3,4,5,6,7,8,9,10,21',
	'1,000 rolled-back siblings of a live subtransaction: every row reads back');
$standby->stop('immediate', fail_ok => 1);

# One transaction rolling back 10,000 savepoints past a 1 MB delta_memory
# completes: the folds its writes start drop each rolled-back savepoint once
# it has rolled back, each logged aborted once, in records of a few bytes per
# savepoint, and the standby folds every point to the primary's base.
($primary, $standby) = pair('savepoints', "\ndelta_memory = 1MB");
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$since = $primary->lsn('insert');
$primary->safe_psql('postgres',
	q{DO $$BEGIN FOR i IN 1..10000 LOOP BEGIN INSERT INTO t VALUES (100 + i, repeat('s', 200)); RAISE EXCEPTION 'r'; EXCEPTION WHEN raise_exception THEN NULL; END; END LOOP; END$$}
);
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
is(aborted($primary, $since), 10000, 'every rolled-back savepoint is logged aborted once');
my $bytes = $primary->safe_psql('postgres',
	"CHECKPOINT; SELECT sum(record_length) FROM pg_get_wal_records_info('$since', 'FFFFFFFF/FFFFFFFF') WHERE resource_manager = 'Versioned2' AND record_type = 'FoldStatuses'"
);
note("the statuses records of 10,000 rolled-back savepoints take $bytes bytes");
cmp_ok($bytes, '<', 100000,
	'the statuses records of 10,000 rolled-back savepoints take under 10 bytes each');
is(matched($primary, $standby, '10,000 rolled-back savepoints'),
	$committed, '10,000 rolled-back savepoints: every committed row reads back');
$standby->stop('immediate', fail_ok => 1);

# A VACUUM inside a top whose savepoint rolled back four rows frees their
# tuple ids, as the reference's VACUUM finds an aborted inserter's tuples
# dead: the next rows land where the reference places them. VACUUM's fold
# logs the subtransaction for its rows, and the first fold after the top ends
# logs it again for its free space map takes, which stay until then above the
# top's own take: twice in all.
($primary, $standby) = pair('window');
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$since = $primary->lsn('insert');
$open = $primary->background_psql('postgres');
$open->query_safe(
	"BEGIN; INSERT INTO t VALUES (21, 'a'); SAVEPOINT s; INSERT INTO t SELECT g, 'b' FROM generate_series(22, 25) g; ROLLBACK TO SAVEPOINT s"
);
my $placed = q{SELECT string_agg(ctid || ':' || id, ',' ORDER BY ctid) FROM t WHERE id > 20};
$primary->safe_psql('postgres', 'VACUUM t');
$primary->safe_psql('postgres', "INSERT INTO t VALUES (26, 'c')");
is($primary->safe_psql('postgres', $placed),
	'(0,12):26', "a VACUUM inside the top frees the rolled-back rows' tuple ids");
$open->query_safe("INSERT INTO t VALUES (27, 'd'); COMMIT");
$open->quit;
$primary->safe_psql('postgres', "INSERT INTO t VALUES (28, 'e')");
is($primary->safe_psql('postgres', $placed),
	'(0,11):21,(0,12):26,(0,13):27,(0,14):28',
	'the rows after the commit land where the reference places them');
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
is(aborted($primary, $since), 2,
	"VACUUM's fold and the fold after the commit each log the rolled-back subtransaction once");
is( matched($primary, $standby, 'a VACUUM inside a top with a rolled-back savepoint'),
	'14 1,2,3,4,5,6,7,8,9,10,21,26,27,28',
	'a VACUUM inside a top with a rolled-back savepoint: every row reads back');
$standby->stop('immediate', fail_ok => 1);

# The top's spill while VACUUM's fold waits between its read of the top's
# rolled-back rows and their drop leaves those rows in the memtable, so the
# fold drops them and VACUUM frees their tuple ids as the reference's does;
# had the spill moved them, its tuple-id file would map them past VACUUM.
# The top's second insert crosses its own spill threshold but not the delta
# budget, since a writer over the budget waits for the paused fold instead
# of spilling.
($primary, $standby) = pair('rolling', "\ndelta_memory = 1MB");
$primary->safe_psql('postgres',
	'CREATE EXTENSION pg_walinspect; CREATE EXTENSION injection_points');
$since = $primary->lsn('insert');
$open = $primary->background_psql('postgres');
$open->query_safe(
	"BEGIN; INSERT INTO t VALUES (21, 'a'); SAVEPOINT s; INSERT INTO t SELECT g, repeat('r', 200) FROM generate_series(101, 400) g; ROLLBACK TO SAVEPOINT s"
);
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-installed', 'wait')");
my $vacuum = $primary->background_psql('postgres');
$vacuum->query_until(qr/start/, "\\echo start\nVACUUM t;\n");
$primary->wait_for_event('client backend', 'doltgres-fold-installed');
$open->query_safe(
	"INSERT INTO t SELECT g, repeat('k', 200) FROM generate_series(1001, 1400) g");
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-installed'); SELECT injection_points_wakeup('doltgres-fold-installed')"
);
$vacuum->quit;
$primary->safe_psql('postgres', "INSERT INTO t VALUES (2001, 'z')");
is($primary->safe_psql('postgres', q{SELECT ctid FROM t WHERE id = 2001}),
	'(0,12)', "a spill during VACUUM's fold leaves VACUUM the rolled-back rows' tuple ids");
$open->query_safe('COMMIT');
$open->quit;
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
cmp_ok(aborted($primary, $since), '<=', 2,
	"a spill during VACUUM's fold: the subtransaction is logged at most twice");
like(matched($primary, $standby, "a spill during VACUUM's fold"),
	qr/^412 1,2,3,4,5,6,7,8,9,10,21,1001,/,
	"a spill during VACUUM's fold: every row reads back");
$standby->stop('immediate', fail_ok => 1);

# rolling says whether the primary's state of t counts a fold reading
# rolled-back rows or a rows spill's walk.
sub rolling
{
	my ($primary) = @_;
	return $primary->safe_psql('postgres', q{SELECT doltgres_test_rolling('t')});
}

# A rows spill whose file's write fails while VACUUM's fold waits for its walk
# counts itself out on its way out, so the fold's wait ends and VACUUM
# completes. The writer's rolled-back savepoint leaves a tuple id only an
# aborted version maps, so VACUUM folds; its insert passes the 1 MB delta
# budget, so it spills.
($primary, $standby) = pair('walk_failed', "\ndelta_memory = 1MB");
$standby->stop('immediate', fail_ok => 1);
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-spill-walking', 'wait'); SELECT injection_points_attach('doltgres-spill-written', 'error'); SELECT injection_points_attach('doltgres-fold-rolling', 'wait')"
);
$open = $primary->background_psql('postgres', on_error_stop => 0);
$open->query_safe(
	"BEGIN; SAVEPOINT s; INSERT INTO t VALUES (21, 'r'); ROLLBACK TO SAVEPOINT s");
$open->query_until(qr/start/,
	"\\echo start\nINSERT INTO t SELECT g, repeat('k', 200) FROM generate_series(1001, 7000) g;\n");
$primary->wait_for_event('client backend', 'doltgres-spill-walking');
$vacuum = $primary->background_psql('postgres');
$vacuum->query_until(qr/start/, "\\echo start\nVACUUM t;\n");
$primary->wait_for_event('client backend', 'doltgres-fold-rolling');
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-rolling'); SELECT injection_points_wakeup('doltgres-fold-rolling')"
);
is(rolling($primary), 't', "VACUUM's fold waits for the walking spill");
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-spill-walking'); SELECT injection_points_wakeup('doltgres-spill-walking')"
);
$vacuum->quit;
$primary->safe_psql('postgres', "SELECT injection_points_detach('doltgres-spill-written')");
$open->quit;
is(rolling($primary), 'f',
	"a spill that failed in its write while VACUUM's fold waited left nothing counted");
$primary->stop;

# A VACUUM terminated during its fold's wait for a walking spill leaves no
# rolling fold counted, so every later spill of the table moves every version
# of its transaction. The writer is set up as in the case before.
($primary, $standby) = pair('rolling_ended', "\ndelta_memory = 1MB");
$standby->stop('immediate', fail_ok => 1);
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-spill-walking', 'wait'); SELECT injection_points_attach('doltgres-fold-rolling', 'wait')"
);
$open = $primary->background_psql('postgres');
$open->query_safe(
	"BEGIN; SAVEPOINT s; INSERT INTO t VALUES (21, 'r'); ROLLBACK TO SAVEPOINT s");
$open->query_until(qr/start/,
	"\\echo start\nINSERT INTO t SELECT g, repeat('k', 200) FROM generate_series(1001, 7000) g;\n");
$primary->wait_for_event('client backend', 'doltgres-spill-walking');
$vacuum = $primary->background_psql('postgres', on_error_stop => 0);
$vacuum->query_until(qr/start/, "\\echo start\nVACUUM t;\n");
$primary->wait_for_event('client backend', 'doltgres-fold-rolling');
my $pid = $primary->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-rolling'");
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-rolling'); SELECT injection_points_wakeup('doltgres-fold-rolling')"
);
$primary->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$primary->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
  or die 'the VACUUM never ended';
eval { $vacuum->quit; };
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-spill-walking'); SELECT injection_points_wakeup('doltgres-spill-walking')"
);
$open->quit;
is(rolling($primary), 'f',
	"a VACUUM terminated during its fold's wait leaves no rolling fold counted");
$primary->stop;

# A primary whose 1 MB delta spills a running top's rolled-back savepoints
# while its standby's 64 MB delta holds them in the memtable: the two split
# each block's versions differently at every fold, and the standby still
# folds each of the primary's points to the primary's base.
($primary, $standby) =
  pair('split', "\ndelta_memory = 1MB", "\ndelta_memory = 64MB");
$primary->safe_psql('postgres',
	q{DO $$BEGIN FOR i IN 1..7000 LOOP BEGIN INSERT INTO t VALUES (100 + i, repeat('s', 200)); RAISE EXCEPTION 'r'; EXCEPTION WHEN raise_exception THEN NULL; END; END LOOP; END$$}
);
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
is(matched($primary, $standby, 'savepoints split apart on the two sides'),
	$committed, 'savepoints split apart on the two sides: every committed row reads back');
$standby->stop('immediate', fail_ok => 1);

# Blocks whose older takes a running top's spill holds and whose newer takes
# its memtable holds, all of rolled-back savepoints, keep their entries
# through the folds while the top runs: no fold puts an older spilled entry
# back, which would hand out a rolled-back row's tuple id before VACUUM frees
# it. The first savepoint's rows pass the 1 MB delta budget, so its versions
# spill.
($primary, $standby) = pair('backward', "\ndelta_memory = 1MB");
$standby->stop('immediate', fail_ok => 1);
$open = $primary->background_psql('postgres');
$open->query_safe(
	"BEGIN; SAVEPOINT s1; INSERT INTO t SELECT g, repeat('o', 200) FROM generate_series(10001, 16000) g; ROLLBACK TO SAVEPOINT s1; SAVEPOINT s2; INSERT INTO t SELECT g, repeat('n', 200) FROM generate_series(20001, 20020) g; ROLLBACK TO SAVEPOINT s2"
);
my $entries =
  q{SELECT string_agg(doltgres_test_free_space('t', b::int4), '|' ORDER BY b) FROM generate_series(0, doltgres_test_blocks('t') - 1) b};
my $before = $primary->safe_psql('postgres', $entries);
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
is($primary->safe_psql('postgres', $entries),
	$before, "two folds while the top runs leave every block's free space entry");
$open->query_safe('COMMIT');
$open->quit;
$primary->stop;

# A standby's fold worker held at its fold of a replayed point while the
# standby replays the frees its primary's VACUUM wrote past that point reads
# the free space map at the point, as its primary's fold did, so it keeps the
# pending tuple ids the primary's fold kept and matches its base.
($primary, $standby) = pair('held_fold');
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres',
	"BEGIN; INSERT INTO t SELECT g, 'r' FROM generate_series(11, 20) g; ROLLBACK");
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->wait_for_catchup($standby);
$standby->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-relation' OR backend_type LIKE 'fold worker%'"
);
$standby->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-relation', 'wait')");
$primary->safe_psql('postgres', "INSERT INTO t VALUES (21, 'later')");
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->safe_psql('postgres', 'VACUUM t');
$primary->wait_for_catchup($standby);
ok( $standby->poll_query_until(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-relation'"),
	"the standby's fold of the point before VACUUM's frees waits");
$standby->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-relation'); SELECT injection_points_wakeup('doltgres-fold-relation')"
);
$standby->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-relation'");
is( matched($primary, $standby, "a standby's fold held past its point's frees"),
	'11 1,2,3,4,5,6,7,8,9,10,21',
	"a standby's fold held past its point's frees: every row reads back");
$standby->stop('immediate', fail_ok => 1);

# Bugs row 474: the same held fold over a committed DELETE's tuple ids, which
# the fold that applies the delete makes pending and VACUUM frees past the
# next fold's point.
($primary, $standby) = pair('held_delete');
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres', 'DELETE FROM t WHERE id > 5');
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->wait_for_catchup($standby);
$standby->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-relation' OR backend_type LIKE 'fold worker%'"
);
$standby->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-relation', 'wait')");
$primary->safe_psql('postgres', "INSERT INTO t VALUES (21, 'later')");
$primary->safe_psql('postgres', q{SELECT doltgres_test_fold('t')});
$primary->safe_psql('postgres', 'VACUUM t');
$primary->wait_for_catchup($standby);
ok( $standby->poll_query_until(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-relation'"),
	"the standby's fold of the point before VACUUM's frees of deleted rows waits");
$standby->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-relation'); SELECT injection_points_wakeup('doltgres-fold-relation')"
);
$standby->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-relation'");
is( matched($primary, $standby, "a standby's fold held past its point's frees of deleted rows"),
	'6 1,2,3,4,5,21',
	"a standby's fold held past its point's frees of deleted rows: every row reads back");
$standby->stop('immediate', fail_ok => 1);

done_testing();
