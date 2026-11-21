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

# The fold's automatic trigger (PLAN item 6's Fold workers; the fold trigger's
# design text): a fold launcher starts fold workers that fold each storage due
# a fold, one past fold_threshold, one older than fold_timeout, and every one
# a writer refused at the delta's budget asks for, with no call from a test.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('trigger');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
	"fold_threshold = 16kB\nfold_timeout = 3600s");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');
$node->backup('early');

# rows returns a fork table's newest base's row count, which a fold moves.
sub rows
{
	my ($on, $table) = @_;
	return $on->safe_psql('postgres',
		"SELECT split_part(doltgres_test_base_sizes('$table'), ' ', 2)");
}

# The fold's settings, in Postgres' form: their built-in values, units, and
# contexts.
is( $node->safe_psql(
		'postgres',
		"SELECT string_agg(name || ' ' || boot_val || ' ' || coalesce(unit, '-') || ' ' || context, ', ' ORDER BY name) FROM pg_settings WHERE name LIKE 'fold\\_%'"
	),
	'fold_threshold -1 kB sighup, fold_timeout 30 s sighup, fold_workers 2 - postmaster',
	"the fold's settings have Postgres' units, defaults, and contexts");

# A storage whose delta passes fold_threshold is folded with no call from a
# test.
$node->safe_psql('postgres', 'CREATE TABLE past (k int4, v text)');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('past', true, 'INSERT INTO past SELECT n, repeat(''p'', 200) FROM generate_series(1, 200) n')"
);
ok( $node->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('past'), ' ', 2) = '200'"
	),
	'a storage whose delta passes fold_threshold is folded with no call from a test'
);

# A storage under fold_threshold stays unfolded while its changes are younger
# than fold_timeout, and is folded once they are older than it, the setting
# read again at a reload.
$node->safe_psql('postgres', 'CREATE TABLE young (k int4, v text)');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('young', true, 'INSERT INTO young VALUES (1, ''y'')')"
);
sleep(3);
is(rows($node, 'young'),
	'0', 'a small delta younger than fold_timeout stays unfolded');
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_timeout = '1s'; SELECT pg_reload_conf()");
ok( $node->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('young'), ' ', 2) = '1'"),
	'a small delta older than fold_timeout is folded');
$node->safe_psql('postgres',
	"ALTER SYSTEM RESET fold_timeout; ALTER SYSTEM SET fold_threshold = '1GB'; SELECT pg_reload_conf()"
);

# MacPF 12's 047 scenario: a server restored from a backup taken before a
# table's folds replays them without their packs, so its delta holds the
# table's versions; once promoted, its own launcher folds the table, and
# VACUUM frees the deleted rows' tuple ids with the primary's horizon, so the
# next inserts land where the primary's do.
$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$node->safe_psql('postgres', 'CREATE TABLE ended (k int4, v text)');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('ended', true, 'INSERT INTO ended SELECT n, ''e'' || n FROM generate_series(1, 20) n')"
);
$node->safe_psql('postgres', "SELECT doltgres_test_fold('ended')");
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('ended', true, 'DELETE FROM ended WHERE k <= 5')"
);
$node->safe_psql('postgres', "SELECT doltgres_test_fold('ended')");
my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup($node, 'early', has_streaming => 1);
$restored->append_conf('postgresql.conf', "fold_threshold = 1kB");
$restored->start;
$node->wait_for_replay_catchup($restored);
$restored->promote;
ok( $restored->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('ended'), ' ', 2) = '15'"
	),
	"the promoted server's launcher folds the replayed table");
my $freed = sub {
	my ($on) = @_;
	my $from =
	  $on->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$on->safe_psql('postgres',
		"SELECT doltgres_test_vacuum('ended', 'auto')");
	$on->safe_psql('postgres',
		"SELECT doltgres_test_write('ended', true, 'INSERT INTO ended VALUES (21, ''x''), (22, ''y'')')"
	);
	my $horizons = $on->safe_psql('postgres',
		"SELECT string_agg(substring(description from 'horizon (\\d+)'), ' ') FROM pg_get_wal_records_info('$from', pg_current_wal_insert_lsn()) WHERE record_type = 'TidsFreed'"
	);
	return [
		$horizons,
		$on->safe_psql(
			'postgres',
			"SELECT doltgres_test_standing('ended', 'SELECT string_agg(ctid::text, '' '' ORDER BY ctid) FROM ended WHERE k > 20')"
		)
	];
};
is_deeply($freed->($restored), $freed->($node),
	"the promoted server's VACUUM frees the folded deletes with the primary's horizon, and the next inserts land as the primary's"
);

# An extension holding every slot max_worker_processes allows leaves the fold
# workers theirs: its next dynamic worker is refused, and a storage past
# fold_threshold is still folded.
$node->append_conf('postgresql.conf', "max_worker_processes = 4");
$node->restart;
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_threshold = '16kB'; SELECT pg_reload_conf()");
$node->safe_psql('postgres', 'CREATE EXTENSION worker_spi');
ok( $node->safe_psql('postgres',
		"SELECT count(*) FROM generate_series(1, 8) n WHERE worker_spi_launch(n) IS NOT NULL"
	) > 0,
	"the extension's dynamic workers start");
is( $node->safe_psql('postgres', 'SELECT worker_spi_launch(9) IS NULL'),
	't',
	"with max_worker_processes in use, the extension's next worker is refused"
);
$node->safe_psql('postgres', 'CREATE TABLE crowded (k int4, v text)');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('crowded', true, 'INSERT INTO crowded SELECT n, repeat(''c'', 200) FROM generate_series(1, 200) n')"
);
ok( $node->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('crowded'), ' ', 2) = '200'"
	),
	'with max_worker_processes in use, a storage past fold_threshold is still folded'
);

# A fold runs user functions with every user and superuser setting at its
# built-in value, whatever the server's configuration says (PLAN item 6's User
# code in folds): a key compared by a function that orders the other way
# under any other DateStyle, TimeZone, or extra_float_digits is folded in the
# order the sessions that wrote it saw at those built-in values.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET DateStyle = 'SQL, DMY'; ALTER SYSTEM SET TimeZone = 'Asia/Tokyo'; ALTER SYSTEM SET extra_float_digits = 0; SELECT pg_reload_conf()"
);
my $builtin =
  "SET DateStyle = 'ISO, MDY'; SET TimeZone = 'GMT'; SET extra_float_digits = 1;";
$node->safe_psql(
	'postgres', $builtin . q{
CREATE FUNCTION styled_cmp(a int4, b int4) RETURNS int4 LANGUAGE plpgsql AS $$
BEGIN
	IF current_setting('DateStyle') = 'ISO, MDY' AND current_setting('TimeZone') = 'GMT'
		AND current_setting('extra_float_digits') = '1' THEN
		RETURN btint4cmp(a, b);
	END IF;
	RETURN btint4cmp(b, a);
END $$;
CREATE OPERATOR CLASS styled_ops FOR TYPE int4 USING btree AS
	OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 styled_cmp(int4, int4);
CREATE TYPE styled AS RANGE (subtype = int4, subtype_opclass = styled_ops);
CREATE TABLE styled_keys (k styled PRIMARY KEY, v text);
SELECT doltgres_test_write('styled_keys', true, 'INSERT INTO styled_keys SELECT styled(n, n + 1), repeat(''s'', 1000) FROM generate_series(1, 20) n');
});
ok( $node->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('styled_keys'), ' ', 2) = '20'"
	),
	'a storage keyed by a user comparator is folded with no call from a test'
);
is( $node->safe_psql(
		'postgres',
		$builtin
		  . "SELECT doltgres_test_standing('styled_keys', 'SELECT string_agg(lower(k)::text, '' '') FROM styled_keys')"
	),
	join(' ', 1 .. 20),
	"a fold orders a user comparator's keys under the built-in settings, not the server's"
);
$node->safe_psql('postgres',
	"ALTER SYSTEM RESET DateStyle; ALTER SYSTEM RESET TimeZone; ALTER SYSTEM RESET extra_float_digits; SELECT pg_reload_conf()"
);

# A fold reports its errors in C's messages whatever lc_messages the server
# sets, since a session can't set its empty built-in value (PLAN item 6's User
# code in folds): a comparator that raises only under a fold's search_path
# names the lc_messages it ran under.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET lc_messages = 'en_US.UTF-8'; SELECT pg_reload_conf()");
is($node->safe_psql('postgres', 'SHOW lc_messages'),
	'en_US.UTF-8', "the server's sessions take its lc_messages");
my $offset = -s $node->logfile;
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION told_cmp(a int4, b int4) RETURNS int4 LANGUAGE plpgsql AS $$
BEGIN
	IF current_setting('search_path') = 'pg_catalog, pg_temp' THEN
		RAISE EXCEPTION 'folding under lc_messages %', current_setting('lc_messages');
	END IF;
	RETURN btint4cmp(a, b);
END $$;
CREATE OPERATOR CLASS told_ops FOR TYPE int4 USING btree AS
	OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 told_cmp(int4, int4);
CREATE TYPE told AS RANGE (subtype = int4, subtype_opclass = told_ops);
CREATE TABLE told_keys (k told PRIMARY KEY, v text);
SELECT doltgres_test_write('told_keys', true, 'INSERT INTO told_keys SELECT told(n, n + 1), repeat(''t'', 1000) FROM generate_series(1, 20) n');
});
ok($node->wait_for_log(qr/ERROR:  folding under lc_messages C\b/, $offset),
	"a fold's user function runs under lc_messages C");
$node->safe_psql('postgres',
	"DROP TABLE told_keys; ALTER SYSTEM RESET lc_messages; SELECT pg_reload_conf()"
);

# A fold runs user functions under search_path pg_catalog, pg_temp, set after
# every setting's reset to its built-in value, so no reset puts the default
# back (PLAN item 6's User code in folds): a comparator called by a fold worker
# logs the search_path it runs under, and a function of the public schema it
# names unqualified isn't found there. A fold worker takes no transaction id,
# so the comparator reports to the server's log rather than to a table.
$offset = -s $node->logfile;
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION fold_probe() RETURNS int4 LANGUAGE sql AS 'SELECT 1';
CREATE FUNCTION pathed_cmp(a int4, b int4) RETURNS int4 LANGUAGE plpgsql AS $$
BEGIN
	IF (SELECT backend_type FROM pg_catalog.pg_stat_activity WHERE pid = pg_catalog.pg_backend_pid()) = 'fold worker' THEN
		RAISE LOG 'fold search_path %', current_setting('search_path');
		BEGIN
			PERFORM fold_probe();
			RAISE LOG 'fold found fold_probe';
		EXCEPTION WHEN undefined_function THEN
			RAISE LOG 'fold could not find fold_probe';
		END;
	END IF;
	RETURN btint4cmp(a, b);
END $$;
CREATE OPERATOR CLASS pathed_ops FOR TYPE int4 USING btree AS
	OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 pathed_cmp(int4, int4);
CREATE TYPE pathed AS RANGE (subtype = int4, subtype_opclass = pathed_ops);
CREATE TABLE pathed_keys (k pathed PRIMARY KEY, v text);
SELECT doltgres_test_write('pathed_keys', true, 'INSERT INTO pathed_keys SELECT pathed(n, n + 1), repeat(''p'', 1000) FROM generate_series(1, 20) n');
});
ok( $node->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('pathed_keys'), ' ', 2) = '20'"
	),
	'a storage keyed by a comparator that logs its search_path is folded');
ok( $node->log_contains(
		qr/LOG:  fold search_path pg_catalog, pg_temp$/m, $offset),
	"a fold's user function runs under search_path pg_catalog, pg_temp");
ok( $node->log_contains(qr/LOG:  fold could not find fold_probe/, $offset)
	  && !$node->log_contains(qr/LOG:  fold found fold_probe/, $offset),
	"a fold's user function doesn't find a function of the public schema unqualified"
);

# A hot standby folds at each of its primary's fold points, in order (PLAN item
# 8's "Standbys' roots"; bugs row 158), since its tuple id, free space, and
# pending trees depend on which points were folded. While its launcher is held,
# the startup folds a byte-ordered table's reached point itself when the next
# fold's record arrives, so of the primary's three folds it folds the first
# two, and the launcher's worker folds the newest once it runs, each fold's
# roots the record's.
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->backup('hot');
my $hot = PostgreSQL::Test::Cluster->new('hot');
$hot->init_from_backup($node, 'hot', has_streaming => 1);
$hot->start;
$hot->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
ok( $hot->poll_query_until(
		'postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'"
	),
	"the standby's launcher is held");
$node->safe_psql('postgres',
	'CREATE TABLE several (k int4 PRIMARY KEY, v text)');

for my $fold (1 .. 3)
{
	my $from = 5 * $fold - 4;
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('several', true, 'INSERT INTO several SELECT n, ''s'' FROM generate_series($from, $from + 4) n')"
	);
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('several')");
}
$node->wait_for_replay_catchup($hot);
is( rows($hot, 'several'),
	'10',
	"while its launcher is held, the standby's startup folds a byte-ordered table's reached points in order as the next record arrives"
);
$offset = -s $hot->logfile;
$hot->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
);
ok( $hot->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('several'), ' ', 2) = '15'"
	),
	"the standby's worker folds the newest reached point once its launcher runs"
);
is( $hot->safe_psql('postgres', "SELECT doltgres_test_base_sizes('several')"),
	$node->safe_psql(
		'postgres', "SELECT doltgres_test_base_sizes('several')"),
	"the standby's folded base is the primary's");
ok(!$hot->log_contains(qr/does not match its primary's/, $offset),
	"the standby's roots match the record's");

# The injection point doltgres-fold-record-root is the only way a fold's
# record and its install differ: with it attached, the primary installs its
# fold and logs another rows root. A standby's fold of a comparator's tree
# whose root differs from the record's warns, naming the relation and both
# roots, and keeps and reads its own root, since a user function can answer
# differently on two servers.
my $altered = sub {
	my ($table) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-fold-record-root', 'notice')"
	);
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-fold-record-root')");
};
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION ranged_cmp(a int4, b int4) RETURNS int4 LANGUAGE sql IMMUTABLE AS 'SELECT btint4cmp(a, b)';
CREATE OPERATOR CLASS ranged_ops FOR TYPE int4 USING btree AS
	OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 ranged_cmp(int4, int4);
CREATE TYPE ranged AS RANGE (subtype = int4, subtype_opclass = ranged_ops);
CREATE TABLE ranged_keys (k ranged PRIMARY KEY, v text);
SELECT doltgres_test_write('ranged_keys', true, 'INSERT INTO ranged_keys SELECT ranged(n, n + 1), ''r'' FROM generate_series(1, 10) n');
});
$offset = -s $hot->logfile;
$altered->('ranged_keys');
ok( $hot->wait_for_log(
		qr/WARNING:  standby's fold of relation "ranged_keys" does not match its primary's\n.*DETAIL:  The rows tree of storage \d+\/\d+\/\d+ has root [0-9a-v]+ on this standby and [0-9a-v]+ on the primary\./,
		$offset),
	"a standby's comparator tree whose root differs from its primary's warns, naming the relation and both roots"
);
is( $hot->safe_psql(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('ranged_keys'), ' ', 2), doltgres_test_standing('ranged_keys', 'SELECT count(*) FROM ranged_keys')"
	),
	'10|10',
	'the standby keeps and reads its own root');

# A standby's fold of a byte-ordered tree whose root differs from the record's
# installs nothing and ends recovery with a FATAL naming the storage and both
# roots, on a hot standby through its fold worker, and the primary goes on.
my $ended = sub {
	my ($standby, $from) = @_;
	my $fatal = $standby->wait_for_log(
		qr/FATAL:  standby's fold does not match its primary's\n.*DETAIL:  The rows tree of storage \d+\/\d+\/\d+ has root [0-9a-v]+ on this standby and [0-9a-v]+ on the primary\.\n.*HINT:  Rebuild the standby from a new base backup of the primary\./,
		$from);
	my $pid = $standby->data_dir . '/postmaster.pid';
	for (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		last if !-e $pid;
		usleep(100_000);
	}
	$standby->_update_pid(0);
	return $fatal;
};
$node->safe_psql('postgres',
	'CREATE TABLE mismatched (k int4 PRIMARY KEY, v text)');
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('mismatched', true, 'INSERT INTO mismatched SELECT n, repeat(''m'', 100) FROM generate_series(1, 20) n')"
);
$offset = -s $hot->logfile;
$altered->('mismatched');
ok( $ended->($hot, $offset),
	"a hot standby's byte-ordered tree whose root differs from its primary's ends recovery with a FATAL naming both roots"
);
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('mismatched', true, 'INSERT INTO mismatched VALUES (21, ''m'')')"
);
is( $node->safe_psql(
		'postgres',
		"SELECT doltgres_test_standing('mismatched', 'SELECT count(*) FROM mismatched')"
	),
	'21',
	"the primary goes on past its standby's refusal");

# A transaction's spills count toward its storage's unfolded bytes (CommPF 9's
# ruling on the trigger and the delta's drain), so a storage whose memtable
# holds less than fold_threshold but whose spills pass it is folded with no
# call from a test, and its folds take its spills out, a drop taking them at
# the point its install is done with, which a later fold may reach, and the
# second checkpoint after that removes their files: 1,100 rows of 8,000 bytes
# of hex spill as they are written, which leaves the memtable under
# fold_threshold.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_threshold = '1GB'; SELECT pg_reload_conf()");
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_spilled (v text);
INSERT INTO source_spilled SELECT string_agg(md5(i || '-' || j), '' ORDER BY j) FROM generate_series(1, 1100) i, generate_series(1, 250) j GROUP BY i;
CREATE TABLE spilled (v text STORAGE PLAIN);
SELECT count(*) FROM doltgres_test_insert('spilled', 'source_spilled', true, 0);
});
my $spills =
  "SELECT count(*) FROM pg_ls_dir('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = 'postgres') || '/delta/spill', true, false)";
cmp_ok($node->safe_psql('postgres', $spills),
	'>=', 1, 'an insert past an eighth of delta_memory spills');
is(rows($node, 'spilled'),
	'0', 'a storage under fold_threshold stays unfolded');
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_threshold = '2MB'; SELECT pg_reload_conf()");
ok( $node->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('spilled'), ' ', 2) = '1100'"
	),
	'a storage whose spills pass fold_threshold is folded with no call from a test'
);
my $spills_left;
for (1 .. $PostgreSQL::Test::Utils::timeout_default)
{
	$node->safe_psql('postgres', 'CHECKPOINT; CHECKPOINT');
	$spills_left = $node->safe_psql('postgres', $spills);
	last if $spills_left eq '0';
	sleep(1);
}
is($spills_left, '0', "the trigger's folds take the storage's spills out");

# A hot standby folds a comparator-keyed table's reached points in order too,
# though only a fold worker can fold one (bugs row 158): with its launcher held
# while the primary folds three times, each fold after a delete and followed by
# a VACUUM that frees its tuple id, the standby's slot keeps the first point and
# leaves the later records to the WAL. Once the launcher runs, its worker folds
# the first point, then reads the WAL past it for the frees' take-outs and the
# next fold's record, and so on, each fold's roots the record's.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_threshold = '1GB'; ALTER SYSTEM SET fold_timeout = '3600s'; SELECT pg_reload_conf()"
);
my $held_launcher = sub {
	my ($on) = @_;
	$on->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
	return $on->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'"
	);
};
my $run_launcher = sub {
	my ($on) = @_;
	$on->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
	);
};
my $folds = sub {
	my ($table, $count) = @_;
	for my $fold (1 .. $count)
	{
		my $from = 10 * $fold - 9;
		$node->safe_psql('postgres',
			"SELECT doltgres_test_write('$table', true, 'INSERT INTO $table SELECT ranged(n, n + 1), ''o'' FROM generate_series($from, $from + 9) n')"
		);
		$node->safe_psql('postgres',
			"SELECT doltgres_test_write('$table', true, 'DELETE FROM $table WHERE lower(k) = $from')"
		);
		$node->safe_psql('postgres', "SELECT doltgres_test_fold('$table')");
		$node->safe_psql('postgres',
			"SELECT doltgres_test_vacuum('$table', 'auto')");
	}
};
my $same_base = sub {
	my ($on, $table) = @_;
	my $primary = $node->safe_psql('postgres',
		"SELECT doltgres_test_base_sizes('$table')");
	return $on->poll_query_until('postgres',
		"SELECT doltgres_test_base_sizes('$table') = '$primary'");
};
$node->backup('ordered');
my $ordered = PostgreSQL::Test::Cluster->new('ordered');
$ordered->init_from_backup($node, 'ordered', has_streaming => 1);
$ordered->start;
ok($held_launcher->($ordered), "the ordered standby's launcher is held");
$node->safe_psql('postgres',
	'CREATE TABLE ordered_keys (k ranged PRIMARY KEY, v text)');
$folds->('ordered_keys', 3);
$node->wait_for_replay_catchup($ordered);
is( rows($ordered, 'ordered_keys'),
	'0',
	"while its launcher is held, a standby's slot keeps a comparator-keyed table's first reached point unfolded"
);
$offset = -s $ordered->logfile;
$run_launcher->($ordered);
ok( $same_base->($ordered, 'ordered_keys'),
	"a hot standby folds a comparator table's reached points in order, each root its primary's"
);
ok(!$ordered->log_contains(qr/does not match its primary's/, $offset),
	"the ordered standby's roots match each record's");

# A restartpoint never passes the record of a reached point the standby hasn't
# folded, so a standby restarted between two of its primary's folds replays
# their records again and folds both in order (bugs row 158): with its
# launcher held, the standby makes a restartpoint at its primary's checkpoint
# after replaying two folds of a comparator-keyed table and stops at once, and
# its restart folds both.
ok($held_launcher->($ordered),
	"the ordered standby's launcher is held again");
$node->safe_psql('postgres',
	'CREATE TABLE restarted_keys (k ranged PRIMARY KEY, v text)');
$folds->('restarted_keys', 2);
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($ordered);
$ordered->safe_psql('postgres', 'CHECKPOINT');
$ordered->stop('immediate');
$offset = -s $ordered->logfile;
$ordered->start;
ok( $same_base->($ordered, 'restarted_keys'),
	"a standby restarted between two reached points folds both in order, each root its primary's"
);
ok(!$ordered->log_contains(qr/does not match its primary's/, $offset),
	"the restarted standby's roots match each record's");

# Replay goes on past delta_memory while a comparator-keyed table's slot holds
# the standby's restartpoint (bugs row 158): with its launcher held, the
# comparator table's reached point stays unfolded, and a byte-ordered table
# written past delta_memory is folded by the startup at each of its primary's
# folds, in order, as the next one's record arrives.
$ordered->append_conf('postgresql.conf', "delta_memory = 8MB");
$ordered->restart;
ok($held_launcher->($ordered),
	"the ordered standby's launcher is held at delta_memory 8MB");
$node->safe_psql(
	'postgres', q{
CREATE TABLE stuck_keys (k ranged PRIMARY KEY, v text);
SELECT doltgres_test_write('stuck_keys', true, 'INSERT INTO stuck_keys SELECT ranged(n, n + 1), ''s'' FROM generate_series(1, 10) n');
SELECT doltgres_test_fold('stuck_keys');
CREATE TABLE past_budget (k int4 PRIMARY KEY, v text STORAGE PLAIN);
});
for my $batch (0 .. 29)
{
	my $from = 100 * $batch + 1;
	$node->safe_psql('postgres',
		"SELECT doltgres_test_write('past_budget', true, 'INSERT INTO past_budget SELECT n, repeat(''b'', 4000) FROM generate_series($from, $from + 99) n')"
	);
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('past_budget')");
}
$node->wait_for_replay_catchup($ordered);
pass(
	"replay goes on past delta_memory while a comparator table's unfolded slot holds the restartpoint"
);
is( rows($ordered, 'stuck_keys'),
	'0',
	"the comparator table's reached point stays unfolded while the launcher is held"
);
$run_launcher->($ordered);
$ordered->stop;

# With hot_standby off a standby has no fold workers, so a comparator-keyed
# table's slot waits, its restartpoint held, until the standby is promoted, and
# the promoted server then folds each of its primary's points in order before a
# point of its own (bugs row 158).
$node->backup('promoted');
my $promoted = PostgreSQL::Test::Cluster->new('promoted');
$promoted->init_from_backup($node, 'promoted', has_streaming => 1);
$promoted->append_conf('postgresql.conf', "hot_standby = off");
$promoted->start;
$node->safe_psql('postgres',
	'CREATE TABLE promoted_keys (k ranged PRIMARY KEY, v text)');
$folds->('promoted_keys', 2);
$node->wait_for_catchup('promoted', 'replay');
$offset = -s $promoted->logfile;
$promoted->promote;
ok( $same_base->($promoted, 'promoted_keys'),
	"a standby without hot standby, once promoted, folds a comparator table's reached points in order, each root its primary's"
);
ok(!$promoted->log_contains(qr/does not match its primary's/, $offset),
	"the promoted server's roots match each record's");
$promoted->stop;

# A restartpoint a reached point holds is reported, as Postgres keeps a slot's
# WAL with no limit (ORev's T1): on a standby whose fold of a comparator-keyed
# table always raises, the point stays unfolded past checkpoint_timeout, and
# each restartpoint it holds back after that logs a WARNING naming its storage,
# while the standby's redo point stays before it and its WAL grows.
$node->backup('walheld');
my $walheld = PostgreSQL::Test::Cluster->new('walheld');
$walheld->init_from_backup($node, 'walheld', has_streaming => 1);
$walheld->append_conf('postgresql.conf', "checkpoint_timeout = 30s");
$walheld->start;
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION walheld_cmp(a int4, b int4) RETURNS int4 LANGUAGE plpgsql AS $$
BEGIN
	IF current_setting('cluster_name') = 'walheld' AND current_setting('search_path') = 'pg_catalog, pg_temp' THEN
		RAISE EXCEPTION 'folding on the walheld standby';
	END IF;
	RETURN btint4cmp(a, b);
END $$;
CREATE OPERATOR CLASS walheld_ops FOR TYPE int4 USING btree AS
	OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 walheld_cmp(int4, int4);
CREATE TYPE walheld AS RANGE (subtype = int4, subtype_opclass = walheld_ops);
CREATE TABLE walheld_keys (k walheld PRIMARY KEY, v text);
SELECT doltgres_test_write('walheld_keys', true, 'INSERT INTO walheld_keys SELECT walheld(n, n + 1), ''w'' FROM generate_series(1, 10) n');
SELECT doltgres_test_fold('walheld_keys');
});
$node->wait_for_replay_catchup($walheld);
my $redo = "SELECT redo_lsn FROM pg_control_checkpoint()";
my $held_redo = $walheld->safe_psql('postgres', $redo);
my $relation =
  $node->safe_psql('postgres', "SELECT pg_relation_filenode('walheld_keys')");
sleep(31);
$offset = -s $walheld->logfile;
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($walheld);
$walheld->safe_psql('postgres', 'CHECKPOINT');
ok( $walheld->wait_for_log(
		qr/WARNING:  restartpoint held back by storage \d+\/\d+\/$relation\n.*DETAIL:  The storage's replayed fold point waits for its fold, for \d+ s, keeping \d+ bytes of WAL past it\./,
		$offset),
	"a standby's restartpoint held past checkpoint_timeout is reported at WARNING, naming its storage"
);
is($walheld->safe_psql('postgres', $redo),
	$held_redo,
	"the held standby's redo point stays before the reached point");
$walheld->stop;

# The fold of a storage's value map (the fold's owed maps' fold, F1): a fork
# table's out-of-line values are entries of its TOAST relation's value map,
# which the fold workers fold into the TOAST relation's base with no call from
# a test, past fold_timeout here since the map's entries are small; then the
# TOAST relation's delta holds no map version and its words are clear, and the
# rows read their values back from that base, after a restart too.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_threshold = '1GB'; ALTER SYSTEM SET fold_timeout = '3600s'; SELECT pg_reload_conf()"
);
$node->safe_psql(
	'postgres', q{
CREATE TABLE toasted (id int4, v text);
ALTER TABLE toasted ALTER COLUMN v SET STORAGE EXTERNAL;
CREATE TABLE source_toasted (id int4, v text);
INSERT INTO source_toasted SELECT i, string_agg(md5(i || '-' || n), '') FROM generate_series(1, 10) i, generate_series(1, 400) n GROUP BY i;
SELECT count(*) FROM doltgres_test_insert('toasted', 'source_toasted', false, 0);
});
my $toast =
  q{(SELECT reltoastrelid::regclass FROM pg_class WHERE oid = 'toasted'::regclass)};
my $values = sub {
	return $node->safe_psql('postgres',
		"SELECT doltgres_test_standing('toasted', 'SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(v) IS NOT NULL) || '' '' || md5(string_agg(md5(v), '''' ORDER BY id)) FROM toasted')"
	);
};
my $expected = $node->safe_psql('postgres',
	"SELECT count(*) || ' ' || md5(string_agg(md5(v), '' ORDER BY id)) FROM source_toasted"
);
is($values->(), $expected, "the rows store their values out of line");
is($node->safe_psql('postgres', "SELECT doltgres_test_mapped($toast)"),
	't', "the TOAST relation's value map holds the values' entries");
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_timeout = '1s'; SELECT pg_reload_conf()");
ok( $node->poll_query_until(
		'postgres', "SELECT NOT doltgres_test_mapped($toast)"),
	"a TOAST relation's value map is folded into its base with no call from a test, its words clear"
);
is($values->(), $expected,
	"the rows read their values back from the TOAST relation's base");
$node->restart;
is($values->(), $expected,
	"the rows read their values back from the TOAST relation's base after a restart"
);

# A transaction that truncates a table holds its AccessExclusiveLock while the
# table's old storage's committed versions stay in the delta, and writes past
# delta_memory, asking for folds once it has spilled its own versions: the fold
# worker passes over the old storage whose lock it can't take, though it holds
# the most bytes, and folds another table's committed versions, so the writer
# finishes, where a worker waiting for that lock left the writer waiting for
# room the worker would never free, a deadlock the lock manager can't see
# (isolation's subxid-overflow on the switch).
my $locked = PostgreSQL::Test::Cluster->new('locked');
$locked->init;
$locked->append_conf('postgresql.conf',
	"delta_memory = 8MB\nfold_threshold = '1GB'\nfold_timeout = 3600s");
$locked->start;
$locked->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE truncated (k int4, v text);
CREATE TABLE spare (k int4, v text);
SELECT doltgres_test_write('truncated', true, 'INSERT INTO truncated SELECT n, repeat(''t'', 4000) FROM generate_series(1, 900) n');
SELECT doltgres_test_write('spare', true, 'INSERT INTO spare SELECT n, repeat(''s'', 4000) FROM generate_series(1, 700) n');
});
my ($code, $out) = $locked->psql(
	'postgres', q{
BEGIN;
LOCK TABLE truncated IN ACCESS EXCLUSIVE MODE;
SELECT doltgres_test_new_file('truncated');
SELECT doltgres_test_write('truncated', true, 'INSERT INTO truncated SELECT n, NULL FROM generate_series(1, 50000) n');
COMMIT;
SELECT doltgres_test_standing('truncated', 'SELECT count(*) FROM truncated');
},
	timeout => 120,
	timed_out => \my $late);
is( ($out // '') =~ s/.*\n//sr,
	'50000',
	"a transaction that truncated a table writes it past delta_memory and finishes, the fold worker passing over the old storage it can't lock"
);

# Bugs row 189: a fold worker chooses its fold point, waiting there for the
# commits delaying a checkpoint, before it begins its reader, so while it waits
# it holds no reader's epoch: the oldest epoch keeps up with the current one as
# another table's writes retire delta pages, where a reader begun before the
# wait kept every page retired since from being freed.
my $paused = PostgreSQL::Test::Cluster->new('paused');
$paused->init;
$paused->append_conf('postgresql.conf',
	"fold_threshold = '1GB'\nfold_timeout = 3600s");
$paused->start;
$paused->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE EXTENSION injection_points;
CREATE TABLE waited (k int4, v text);
CREATE TABLE moving (k int4, v text);
});
my $holder = $paused->background_psql('postgres');
$holder->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-delay-checkpoint', 'wait')"
);
$holder->query_until(qr/holding/,
	"\\echo holding\nSELECT doltgres_test_delay(0);\n");
$paused->wait_for_event('client backend', 'doltgres-delay-checkpoint');
$paused->safe_psql('postgres',
	"SELECT doltgres_test_write('waited', true, 'INSERT INTO waited VALUES (1, ''w'')')"
);
$paused->safe_psql('postgres',
	"ALTER SYSTEM SET fold_timeout = '1s'; SELECT pg_reload_conf()");
$paused->poll_query_until('postgres',
	"SELECT count(*) > 0 FROM pg_stat_activity WHERE backend_type = 'fold worker' AND wait_event = 'CheckpointDelayStart'"
) or die "no fold worker waited at its fold point";
my $before =
  (split ' ', $paused->safe_psql('postgres', 'SELECT doltgres_test_epochs()'))[1];
$paused->safe_psql('postgres',
	"SELECT doltgres_test_write('moving', true, 'INSERT INTO moving SELECT n, repeat(''m'', 100) FROM generate_series(1, 2000) n')"
);
my ($oldest, $current) =
  split ' ', $paused->safe_psql('postgres', 'SELECT doltgres_test_epochs()');
ok( $current > $before && $oldest == $current,
	"a fold worker waiting at its fold point for the commits delaying a checkpoint holds no reader epoch"
);
$paused->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-delay-checkpoint'); SELECT injection_points_wakeup('doltgres-delay-checkpoint')"
);
$holder->quit;

# 062's budget case (bugs rows 111 and 160) through the real commands: with
# delta_memory at 1 MB and neither of the fold's thresholds met, committed
# writes far past it finish, a writer refused at the budget asking for folds
# whose drops give the delta back and wake it. A count right after the last
# commit, held after it read the table's spill slots while a fold installs a
# base that applies the spilled rows and drops them from the delta, moves to
# that base at its next read and counts every row (bugs row 241); the fold
# launcher is held from before the count, so no fold but the case's own
# installs a base.
my $budget = PostgreSQL::Test::Cluster->new('budget');
$budget->init;
$budget->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = '1GB'\nfold_timeout = 3600s\nautovacuum = off"
);
$budget->start;
$budget->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test; CREATE TABLE committed (k int4, v text)'
);
my $refused = 0;
for my $batch (0 .. 49)
{
	my $from = 1000 * $batch + 1;
	my ($code, $out, $err) = $budget->psql('postgres',
		"SET statement_timeout = '300s'; INSERT INTO committed SELECT n, repeat('c', 100) FROM generate_series($from, $from + 999) n"
	);
	$refused++ if $code;
}
is($refused, 0,
	"fifty committed writes past a 1 MB delta_memory finish with neither of the fold's thresholds met");
$budget->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-launch', 'wait')");
$budget->poll_query_until('postgres',
	q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = 'doltgres-fold-launch'}
) or die 'the fold launcher never waited';
$budget->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'fold worker'}
) or die 'a fold worker kept running';
my $counter = $budget->background_psql('postgres');
$counter->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-spill-slots-read', 'wait')"
);
$counter->query_until(qr/held/,
	"\\echo held\nSELECT count(*) FROM committed;\n");
$budget->wait_for_event('client backend', 'doltgres-spill-slots-read');
is( $budget->safe_psql('postgres', "SELECT doltgres_test_fold('committed')"),
	't',
	'a fold under the held count installs a base');
$budget->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-launch'); SELECT injection_points_wakeup('doltgres-fold-launch')"
);
$budget->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-spill-slots-read'); SELECT injection_points_wakeup('doltgres-spill-slots-read')"
);
my $counted = $counter->query_safe('SELECT 1');
$counter->quit;
like($counted, qr/^50000$/m,
	'a count right after the last commit, held across a fold that applied its spilled rows, counts every row'
);
is($budget->safe_psql('postgres', 'SELECT count(*) FROM committed'),
	'50000', 'and every row reads back');
$budget->stop;

# Bugs row 306: a fold worker held inside its fold, its snapshot taken, keeps
# neither CREATE INDEX CONCURRENTLY nor REINDEX TABLE CONCURRENTLY of other
# tables waiting for it, as a VACUUM doesn't.
my $built = PostgreSQL::Test::Cluster->new('built');
$built->init;
$built->append_conf('postgresql.conf',
	"fold_threshold = 16kB\nfold_timeout = 3600s\nautovacuum = off");
$built->start;
$built->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION doltgres_test;
CREATE TABLE indexed (k int4);
INSERT INTO indexed SELECT generate_series(1, 100);
CREATE TABLE reindexed (k int4);
INSERT INTO reindexed SELECT generate_series(1, 100);
CREATE INDEX reindexed_k ON reindexed (k);
CREATE TABLE held (k int4, v text);
SELECT injection_points_attach('doltgres-fold-relation', 'wait');
});
$built->safe_psql('postgres',
	"SELECT doltgres_test_write('held', true, 'INSERT INTO held SELECT n, repeat(''h'', 200) FROM generate_series(1, 200) n')"
);
$built->wait_for_event('fold worker', 'doltgres-fold-relation');
is( $built->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'fold worker' AND wait_event = 'doltgres-fold-relation' AND backend_xmin IS NOT NULL}
	),
	'1',
	'the held fold worker has its snapshot');
my ($built_ret, $built_out, $built_err) = $built->psql('postgres',
	"SET lock_timeout = '30s'; CREATE INDEX CONCURRENTLY indexed_k ON indexed (k)"
);
is($built_ret, 0,
	"CREATE INDEX CONCURRENTLY doesn't wait for a fold worker held inside its fold"
);
($built_ret, $built_out, $built_err) = $built->psql('postgres',
	"SET lock_timeout = '30s'; REINDEX TABLE CONCURRENTLY reindexed");
is($built_ret, 0,
	"REINDEX TABLE CONCURRENTLY doesn't wait for a fold worker held inside its fold"
);
$built->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-relation'); SELECT injection_points_wakeup('doltgres-fold-relation')"
);
ok( $built->poll_query_until(
		'postgres',
		"SELECT split_part(doltgres_test_base_sizes('held'), ' ', 2) = '200'"
	),
	'the released fold installs its base');
is( $built->safe_psql(
		'postgres',
		'SET enable_seqscan = off; SELECT count(*) FROM indexed WHERE k <= 50; SELECT count(*) FROM reindexed WHERE k <= 50'
	),
	"50\n50",
	'and the built and the rebuilt index read their tables');

# A CREATE INDEX CONCURRENTLY of the very table a held fold worker folds, its
# snapshot older than the deletes and inserts made before the build, doesn't
# wait for it, and once the fold installs its base the new index gives every
# row a sequential scan gives.
$built->safe_psql(
	'postgres', q{
CREATE TABLE own (k int4, v text);
INSERT INTO own SELECT n, 'o' FROM generate_series(1, 100) n;
SELECT injection_points_attach('doltgres-fold-relation', 'wait');
});
$built->safe_psql('postgres',
	"SELECT doltgres_test_write('own', true, 'INSERT INTO own SELECT n, repeat(''o'', 200) FROM generate_series(101, 300) n')"
);
$built->wait_for_event('fold worker', 'doltgres-fold-relation');
$built->safe_psql('postgres',
	q{DELETE FROM own WHERE k <= 20; INSERT INTO own SELECT n, 'n' FROM generate_series(301, 350) n}
);
($built_ret, $built_out, $built_err) = $built->psql('postgres',
	"SET lock_timeout = '30s'; CREATE INDEX CONCURRENTLY own_k ON own (k)");
is($built_ret, 0,
	"CREATE INDEX CONCURRENTLY of a table a held fold worker folds doesn't wait for it"
);
$built->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-relation'); SELECT injection_points_wakeup('doltgres-fold-relation')"
);
ok( $built->poll_query_until(
		'postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'fold worker'}
	),
	"the table's released fold ends");
my $counted_rows = q{SELECT count(*) || ' ' || sum(k) FROM own WHERE k > 0};
my $by_index =
  "SET enable_seqscan = off; SET enable_bitmapscan = off; $counted_rows";
my $by_scan =
  "SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; $counted_rows";
is($built->safe_psql('postgres', $by_index),
	'330 61215', 'and the new index gives every row');
is($built->safe_psql('postgres', $by_scan),
	'330 61215', 'as a sequential scan does');
$built->safe_psql('postgres',
	"SELECT doltgres_test_fold('own'); SELECT doltgres_test_index_fold('own_k')"
);
is($built->safe_psql('postgres', $by_index),
	'330 61215',
	'and still does once the table and the index fold every row');
$built->stop;

# A replayed free's take-out passes over a later take of its tuple id (bugs
# row 320): with a standby's launcher held, the primary folds three rounds of
# a comparator-keyed table, each deleting a row and freeing its tuple id at a
# VACUUM, the next round's insert taking that tuple id again on another key,
# and a fourth round's take of it stays live. Once the launcher runs, the
# standby folds each point and does each free's deferred take-outs past it,
# which meet the later takes; every root matches its primary's at each fold,
# and after a last VACUUM every row reads back as on the primary.
$node->safe_psql('postgres',
	"ALTER SYSTEM SET fold_threshold = '1GB'; ALTER SYSTEM SET fold_timeout = '3600s'; SELECT pg_reload_conf()"
);
$node->backup('retaken');
my $retaken = PostgreSQL::Test::Cluster->new('retaken');
$retaken->init_from_backup($node, 'retaken', has_streaming => 1);
$retaken->start;
ok($held_launcher->($retaken), "the retaken standby's launcher is held");
$node->safe_psql('postgres',
	'CREATE TABLE retaken_keys (k ranged PRIMARY KEY, v text)');
$folds->('retaken_keys', 3);
$node->safe_psql('postgres',
	"SELECT doltgres_test_write('retaken_keys', true, 'INSERT INTO retaken_keys SELECT ranged(n, n + 1), ''r'' FROM generate_series(31, 40) n')"
);
$node->safe_psql('postgres', "SELECT doltgres_test_fold('retaken_keys')");
$node->safe_psql('postgres',
	"SELECT doltgres_test_vacuum('retaken_keys', 'auto')");
$node->wait_for_replay_catchup($retaken);
$offset = -s $retaken->logfile;
$run_launcher->($retaken);
ok( $same_base->($retaken, 'retaken_keys'),
	"a lagging standby whose deferred take-outs meet later takes of their tuple ids folds each point to its primary's roots"
);
ok( !$retaken->log_contains(qr/does not match its primary's/, $offset),
	"the retaken standby's rows, tuple id, free space, and pending tuple id roots match each record's"
);
$node->safe_psql('postgres',
	"SELECT doltgres_test_vacuum('retaken_keys', 'auto')");
$node->wait_for_replay_catchup($retaken);
my $retaken_rows =
  q{SELECT doltgres_test_standing('retaken_keys', 'SELECT string_agg(lower(k)::text, '' '' ORDER BY lower(k)) FROM retaken_keys')};
is( $retaken->safe_psql('postgres', $retaken_rows),
	$node->safe_psql('postgres', $retaken_rows),
	"after a last VACUUM every row, the live retake's included, reads back on the standby as on its primary"
);
$retaken->stop;

done_testing();
