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

# A database's repository (item 6's "Create from a template, drop, and
# move"): initdb's databases have theirs, CREATE DATABASE makes one and DROP
# DATABASE removes it, on a standby too, an id whose repository's directory
# stands isn't given again, a drop and a creation of the same id keep the new
# database's rows through a crash, a checkpoint keeps the WAL a marked
# manifest's fallback needs, a start goes on past a repository it can't
# settle, and template1's lost repository isn't made again quietly.

use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('database');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf',
	"checkpoint_timeout = 1h\nallow_in_place_tablespaces = on");
$node->start;
my $pgdata = $node->data_dir;
my $versioned = "$pgdata/pg_versioned";

# oid returns a database's id.
sub oid
{
	my ($name) = @_;
	return $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = '$name'");
}

# initdb's cluster has template1's, template0's, and postgres' repositories.
for my $name ('template1', 'template0', 'postgres')
{
	my $id = oid($name);
	ok(-f "$versioned/$id/table", "initdb's $name has a repository");
}

# A new database has a repository, which a backend connected to it opens,
# and its template's repository is unchanged.
my $template = slurp_file("$versioned/1/table");
$node->safe_psql('postgres', 'CREATE DATABASE made');
my $made = oid('made');
ok(-f "$versioned/$made/table", 'a new database has a repository');
$node->safe_psql('made', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
is($node->safe_psql('made', 'SELECT doltgres_test_session(false)'),
	'0', "a new database's session repository opens empty");
is(slurp_file("$versioned/1/table"),
	$template, "its template's repository is unchanged");

# A database in another tablespace has its repository there, and its drop
# removes it with the tablespace's directory of repositories, so the
# tablespace drops as in Postgres; a dropped database's repository in the
# default tablespace is gone at once.
my $location = PostgreSQL::Test::Utils::tempdir_short();
$node->safe_psql('postgres', "CREATE TABLESPACE elsewhere LOCATION '$location'");
$node->safe_psql('postgres', 'CREATE DATABASE away TABLESPACE elsewhere');
my $away = oid('away');
my ($version) = glob("$location/PG_*");
ok(-f "$version/pg_versioned/$away/table",
	"a database in another tablespace has its repository there");
$node->safe_psql('postgres', 'DROP DATABASE away');
ok(!-e "$version/pg_versioned",
	"a dropped database's repository is gone at once, with its tablespace's directory of repositories");
$node->safe_psql('postgres', 'DROP TABLESPACE elsewhere');
$node->safe_psql('postgres', 'CREATE DATABASE gone');
my $gone = oid('gone');
$node->safe_psql('postgres', 'DROP DATABASE gone');
ok(!-e "$versioned/$gone", "a dropped database's repository is gone at once");

# A database whose repository is already gone is dropped.
$node->safe_psql('postgres', 'CREATE DATABASE lost');
rmtree("$versioned/" . oid('lost'));
$node->safe_psql('postgres', 'DROP DATABASE lost');
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_database WHERE datname = 'lost'"),
	'0', 'a database whose repository is already gone is dropped');

# An id whose repository's directory stands isn't given to a new database:
# CREATE DATABASE ... OID of it gets Postgres' own error, and a plain CREATE
# DATABASE passes over it.
mkdir "$versioned/70000" or die;
my ($result, $stdout, $stderr) =
  $node->psql('postgres', 'CREATE DATABASE taken OID = 70000');
like(
	$stderr,
	qr/ERROR:  data directory with the specified OID 70000 already exists/,
	"an id whose repository's directory stands is refused");
$node->safe_psql('postgres', 'CREATE DATABASE probe');
my $next = oid('probe') + 1;
$node->safe_psql('postgres', 'DROP DATABASE probe');
mkdir "$versioned/$next" or die;
$node->safe_psql('postgres', 'CREATE DATABASE passed');
isnt(oid('passed'), $next, 'a plain CREATE DATABASE passes over a standing id');
rmdir "$versioned/70000";
rmdir "$versioned/$next";

# A database dropped and created again with the same id keeps its rows
# through a crash: the drop's checkpoint after its removal keeps every
# replay after the drop's record.
$node->safe_psql('postgres', 'CREATE DATABASE reused');
my $reused = oid('reused');
$node->safe_psql('postgres', 'DROP DATABASE reused');
$node->safe_psql('postgres', "CREATE DATABASE reused OID = $reused");
$node->safe_psql('reused', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
is($node->safe_psql('reused', 'SELECT doltgres_test_session(true)'),
	'5', 'the database made again is saved');
$node->stop('immediate');
$node->start;
is($node->safe_psql('reused', 'SELECT doltgres_test_session(false)'),
	'5', 'a database dropped and created again with the same id keeps its rows through a crash');

# A checkpoint keeps the WAL a marked manifest's fallback needs, from the
# redo point of the checkpoint before, warns of a kept segment cut short, and
# recycles the WAL once the manifest is evened: a save into the database made
# again marks its manifest after the checkpoint's evening.
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CHECKPOINT');
my $kept = $node->safe_psql('postgres',
	'SELECT redo_wal_file FROM pg_control_checkpoint()');
$node->safe_psql('postgres', 'CREATE TABLE written (i int)');
my @segments;
for my $i (1 .. 3)
{
	push @segments,
	  $node->safe_psql('postgres',
		"INSERT INTO written VALUES ($i); SELECT pg_walfile_name(pg_current_wal_insert_lsn())"
	  );
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
}
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-store-evened', 'wait')");
my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_until(qr/start/, "\\echo start\nCHECKPOINT;\n");
$node->wait_for_event('checkpointer', 'doltgres-store-evened');
$node->safe_psql('reused', 'SELECT doltgres_test_session(true)');
truncate($pgdata . "/pg_wal/$segments[1]", 4096) or die;
my $offset = -s $node->logfile;
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-store-evened'); SELECT injection_points_detach('doltgres-store-evened')"
);
$checkpoint->quit;
$node->wait_for_log(qr/could not read the WAL kept for a manifest's fallback in segment "$segments[1]"/,
	$offset);
pass('a checkpoint warns of a kept segment cut short');
is( $node->safe_psql(
		'postgres',
		"SELECT min(name) FROM pg_ls_waldir() WHERE name ~ '^[0-9A-F]{24}\$'"),
	$kept,
	"a checkpoint keeps the WAL a marked manifest's fallback needs, from the redo point of the checkpoint before");
$node->safe_psql('postgres', 'CHECKPOINT');
is( $node->safe_psql(
		'postgres',
		"SELECT min(name) = (SELECT redo_wal_file FROM pg_control_checkpoint()) FROM pg_ls_waldir() WHERE name ~ '^[0-9A-F]{24}\$'"
	),
	't',
	'a checkpoint recycles the WAL once the manifest is evened');

# A start whose settle of a repository is refused goes on with it unopened,
# and its use raises.
$node->stop;
unlink "$versioned/$made/table", "$versioned/$made/table.prev";
$node->start;
like(
	slurp_file($node->logfile),
	qr/WARNING:  .*database $made: required file ".*table" does not exist/,
	'a start warns of a repository it cannot settle');
($result, $stdout, $stderr) =
  $node->psql('made', 'SELECT doltgres_test_session(false)');
like($stderr, qr/required file ".*table" does not exist/,
	"the refused repository's use raises");
is($node->safe_psql('reused', 'SELECT doltgres_test_session(false)'),
	'5', 'the other repositories open');
$node->safe_psql('postgres', 'DROP DATABASE made');

# A moved database's repository is in its new tablespace and gone from the
# old, and its session repository opens after the move with what it saved.
my $there = PostgreSQL::Test::Utils::tempdir_short();
$node->safe_psql('postgres', "CREATE TABLESPACE there LOCATION '$there'");
$node->safe_psql('reused', 'SELECT doltgres_test_session(true)');
$node->safe_psql('postgres', 'ALTER DATABASE reused SET TABLESPACE there');
my ($there_version) = glob("$there/PG_*");
ok(-f "$there_version/pg_versioned/$reused/table" && !-e "$versioned/$reused",
	"a moved database's repository is in its new tablespace and gone from the old");
ok(!glob("$there_version/pg_versioned/*.moving"), 'a move leaves no copy behind');
is($node->safe_psql('reused', 'SELECT doltgres_test_session(false)'),
	'5', "a moved database's session repository opens after the move");
$node->safe_psql('postgres', 'ALTER DATABASE reused SET TABLESPACE pg_default');
ok(-f "$versioned/$reused/table" && !-e "$there_version/pg_versioned",
	'a move back leaves the tablespace holding nothing of the fork\'s');
$node->safe_psql('postgres', 'DROP TABLESPACE there');

# A crash in a move: after the copy and before the commit, the old directory
# serves and the next move removes the leftover copy; after the commit and
# before the finish, the first open finishes it; inside the finish after its
# mark, the start finishes it.
my %version;
for my $name ('crashed', 'next', 'last')
{
	$node->safe_psql('postgres', "CREATE TABLESPACE $name LOCATION ''");
	my $id = $node->safe_psql('postgres',
		"SELECT oid FROM pg_tablespace WHERE spcname = '$name'");
	($version{$name}) = glob("$pgdata/pg_tblspc/$id/PG_*");
}
$node->safe_psql('postgres', 'CREATE DATABASE moving');
my $moving = oid('moving');
$node->safe_psql('moving', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('moving', 'SELECT doltgres_test_session(true)');

# crash runs the move of database moving to a tablespace and stops the server
# at once where an injection point holds it, then starts it again.
sub crash
{
	my ($point, $tablespace) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	my $mover = $node->background_psql('postgres', on_error_stop => 0);
	$mover->query_until(qr/start/,
		"\\echo start\nALTER DATABASE moving SET TABLESPACE $tablespace;\n");
	$node->wait_for_event('client backend', $point);
	$node->stop('immediate');
	eval { $mover->quit };
	$node->start;
}

crash('doltgres-store-move-copied', 'crashed');
ok( -f "$versioned/$moving/table"
	  && -d "$version{crashed}/pg_versioned/$moving.moving",
	'a crash after the copy leaves the old directory and the copy');
is($node->safe_psql('moving', 'SELECT doltgres_test_session(false)'),
	'5', 'after a crash before the commit the old directory serves');
$node->safe_psql('postgres', 'ALTER DATABASE moving SET TABLESPACE next');
ok( !-e "$version{crashed}/pg_versioned/$moving.moving"
	  && -f "$version{next}/pg_versioned/$moving/table"
	  && !-e "$versioned/$moving",
	'the next move removes the leftover copy and moves the repository');

crash('doltgres-store-move-finishing', 'pg_default');
ok( -d "$versioned/$moving.moving"
	  && -f "$version{next}/pg_versioned/$moving/table",
	'a crash after the commit leaves the copy unfinished');
is($node->safe_psql('moving', 'SELECT doltgres_test_session(false)'),
	'5', 'after a crash after the commit the first open finishes the move');
ok( -f "$versioned/$moving/table"
	  && !-e "$versioned/$moving.moving"
	  && !-e "$version{next}/pg_versioned",
	"the first open's finish puts the repository in its new tablespace");

crash('doltgres-store-move-marked', 'last');
ok( -f "$version{last}/pg_versioned/$moving/table"
	  && !-e "$versioned/$moving"
	  && !-e "$version{last}/pg_versioned/$moving.moving",
	'after a crash inside the finish the start finishes the move');
is($node->safe_psql('moving', 'SELECT doltgres_test_session(false)'),
	'5', 'the moved repository opens after the start finished the move');
$node->safe_psql('postgres', 'DROP DATABASE moving');
$node->safe_psql('postgres', 'DROP TABLESPACE next');
$node->safe_psql('postgres', 'DROP TABLESPACE last');

# A checkpoint during a move writes nothing into the old directory: the copy
# left the pack record owing nothing, and no session is connected.
$node->safe_psql('postgres', 'CREATE DATABASE paused');
my $paused = oid('paused');
$node->safe_psql('paused', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('paused', 'SELECT doltgres_test_session(true)');
$node->safe_psql('postgres', "CREATE TABLESPACE during LOCATION ''");

# files returns each file of a directory with its size and modification time.
sub files
{
	my ($dir) = @_;
	opendir my $handle, $dir or die "could not open $dir: $!";
	my @names = sort grep { !/^\./ } readdir $handle;
	closedir $handle;
	return join ' ', map { my @st = stat("$dir/$_"); "$_:$st[7]:$st[9]" } @names;
}
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-store-move-copied', 'wait')");
my $pauser = $node->background_psql('postgres');
$pauser->query_until(qr/start/,
	"\\echo start\nALTER DATABASE paused SET TABLESPACE during;\n");
$node->wait_for_event('client backend', 'doltgres-store-move-copied');
my $before = files("$versioned/$paused");
sleep 1;
$node->safe_psql('postgres', 'CHECKPOINT');
is(files("$versioned/$paused"), $before,
	'a checkpoint during a move writes nothing into the old directory');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-store-move-copied'); SELECT injection_points_detach('doltgres-store-move-copied')"
);
$pauser->quit;
is($node->safe_psql('paused', 'SELECT doltgres_test_session(false)'),
	'5', 'the move after the checkpoint keeps what was saved');

# A dropped database's repository is made and gone on a standby too.
$node->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('database_standby');
$node->stop;
$standby->init_from_backup($node, 'backup', has_streaming => 1);
$node->start;
$standby->start;
$node->safe_psql('postgres', 'CREATE DATABASE replayed');
my $replayed = oid('replayed');
$node->wait_for_replay_catchup($standby);
ok(-f $standby->data_dir . "/pg_versioned/$replayed/table",
	"a new database's repository is made on a standby");
$node->safe_psql('postgres', "CREATE TABLESPACE replayed_there LOCATION ''");
$node->safe_psql('postgres', 'ALTER DATABASE replayed SET TABLESPACE replayed_there');
$node->wait_for_replay_catchup($standby);
my $standby_dir = $standby->data_dir;
my $replayed_there = $node->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'replayed_there'");
my ($standby_version) = glob("$standby_dir/pg_tblspc/$replayed_there/PG_*");
ok(-f "$standby_version/pg_versioned/$replayed/table"
	  && !-e "$standby_dir/pg_versioned/$replayed",
	'a move replays on a standby');

# A hot standby session of a database whose move waits before its finish
# reads the repository's old directory, and the finish's replay ends it by the
# conflict Postgres' own drop of a database raises, never by a missing file.
$node->safe_psql('postgres', 'CREATE DATABASE standing');
$node->safe_psql('standing', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('standing', 'SELECT doltgres_test_session(true)');
$node->safe_psql('postgres', "CREATE TABLESPACE standing_there LOCATION ''");
$node->wait_for_replay_catchup($standby);
my $standing = $standby->safe_psql('standing', 'SELECT doltgres_test_session(false)');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-store-move-finishing', 'wait')");
my $standing_mover = $node->background_psql('postgres');
$standing_mover->query_until(qr/start/,
	"\\echo start\nALTER DATABASE standing SET TABLESPACE standing_there;\n");
$node->wait_for_event('client backend', 'doltgres-store-move-finishing');
my $standing_id = oid('standing');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->wait_for_replay_catchup($standby);
ok(!-e $standby->data_dir . "/base/$standing_id",
	"the standby replayed the move's drop record before its finish");
my $reader = $standby->background_psql('standing', on_error_stop => 0);
is($reader->query_safe('SELECT doltgres_test_session(false)'),
	$standing, "a hot standby session reads the moving repository's old directory");
my $standby_offset = -s $standby->logfile;
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-store-move-finishing'); SELECT injection_points_detach('doltgres-store-move-finishing')"
);
$standing_mover->quit;
$node->wait_for_replay_catchup($standby);
$standby->wait_for_log(qr/terminating connection due to conflict with recovery/,
	$standby_offset);
pass("the finish's replay ends the standby session by the conflict");
eval { $reader->quit };
is($standby->safe_psql('standing', 'SELECT doltgres_test_session(false)'),
	$standing, 'after the finish a new standby session reads the moved repository');
$node->safe_psql('postgres', 'DROP DATABASE replayed');
$node->wait_for_replay_catchup($standby);
ok(!-e $standby->data_dir . "/pg_versioned/$replayed",
	"a dropped database's repository is gone on a standby");
$standby->stop;

# A cluster whose template1's repository was removed starts without making
# one, and template1's use raises.
$node->stop;
rmtree("$versioned/1");
$node->start;
ok(!-e "$versioned/1", "template1's lost repository isn't made again");
$node->safe_psql('template1', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
($result, $stdout, $stderr) =
  $node->psql('template1', 'SELECT doltgres_test_session(false)');
like($stderr, qr/ERROR:  required file ".*table" does not exist/,
	"template1's use raises");

# Bugs row 182: a database whose fork table holds folded rows and rows in the
# delta moves to another tablespace, its relations' storages put under the new
# one, and reads every row, after a checkpoint and a restart too; a crash before
# the move's finish, and one inside it after its mark, finish the move with
# every row read after.
my $rekeyed = PostgreSQL::Test::Cluster->new('rekeyed');
$rekeyed->init;
$rekeyed->append_conf('postgresql.conf',
	"allow_in_place_tablespaces = on\nfold_threshold = 1GB\nfold_timeout = 3600s");
$rekeyed->start;
$rekeyed->safe_psql('postgres',
	"CREATE EXTENSION injection_points; CREATE TABLESPACE near LOCATION ''; CREATE TABLESPACE far LOCATION ''; CREATE TABLESPACE farther LOCATION ''"
);
$rekeyed->safe_psql('postgres', 'CREATE DATABASE kept TABLESPACE near');
$rekeyed->safe_psql('kept', 'CREATE EXTENSION doltgres_test; CREATE TABLE rows_kept (k int4 PRIMARY KEY, v text)');
$rekeyed->safe_psql('kept',
	"SELECT doltgres_test_write('rows_kept', true, 'INSERT INTO rows_kept SELECT g, g::text FROM generate_series(1, 50) g')"
);
$rekeyed->safe_psql('kept', "SELECT doltgres_test_fold('rows_kept')");
$rekeyed->safe_psql('kept',
	"SELECT doltgres_test_write('rows_kept', true, 'INSERT INTO rows_kept SELECT g, g::text FROM generate_series(51, 60) g')"
);
my $kept_rows = "SELECT doltgres_test_standing('rows_kept', 'SELECT count(*) || '' '' || sum(k) FROM rows_kept')";
is($rekeyed->safe_psql('kept', $kept_rows), '60 1830', 'the folded rows and the delta\'s read before the move');
$rekeyed->safe_psql('postgres', 'ALTER DATABASE kept SET TABLESPACE far');
is($rekeyed->safe_psql('kept', $kept_rows),
	'60 1830', 'a database moved to another tablespace reads its folded rows and its delta\'s');
$rekeyed->safe_psql('postgres', 'CHECKPOINT');
$rekeyed->stop('immediate');
$rekeyed->start;
is($rekeyed->safe_psql('kept', $kept_rows), '60 1830', 'and reads them after a checkpoint and a restart');
for my $crashed (['doltgres-store-move-finishing', 'farther', 'before'], ['doltgres-store-move-marked', 'near', 'inside'])
{
	my ($point, $tablespace, $where) = @$crashed;
	$rekeyed->safe_psql('postgres', "SELECT injection_points_attach('$point', 'wait')");
	my $mover = $rekeyed->background_psql('postgres', on_error_stop => 0);
	$mover->query_until(qr/start/, "\\echo start\nALTER DATABASE kept SET TABLESPACE $tablespace;\n");
	$rekeyed->wait_for_event('client backend', $point);
	$rekeyed->stop('immediate');
	eval { $mover->quit };
	$rekeyed->start;
	is($rekeyed->safe_psql('kept', $kept_rows), '60 1830', "after a crash $where the move's finish every row reads");
}
$rekeyed->stop;

# Bugs row 99: DROP TABLESPACE of a location a fork table left, moved out by
# SET TABLESPACE or dropped, empties the fork's packs out of it into the
# database's own directory and removes the tablespace's directory of them, so
# the tablespace drops as in Postgres, after the checkpoint that removes a
# dropped table's files, and the moved table reads every row.
$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS doltgres_test');
for my $left ('moved', 'dropped')
{
	$node->safe_psql('postgres', "CREATE TABLESPACE left_$left LOCATION ''");
	my $space = $node->safe_psql('postgres',
		"SELECT oid FROM pg_tablespace WHERE spcname = 'left_$left'");
	$node->safe_psql('postgres',
		"CREATE TABLE in_$left (k int4 PRIMARY KEY, v text) TABLESPACE left_$left; INSERT INTO in_$left SELECT g, repeat('v', 40) || g FROM generate_series(1, 500) g"
	);
	$node->safe_psql('postgres',
		"SELECT doltgres_test_fold('in_$left'); CHECKPOINT");
	my @packs = glob("$pgdata/pg_tblspc/$space/PG_*/pg_versioned/*/*.pack");
	ok(@packs > 0,
		"a fold of a table in tablespace left_$left writes its packs there");
	$node->safe_psql('postgres',
		$left eq 'moved'
		? "ALTER TABLE in_$left SET TABLESPACE pg_default"
		: "DROP TABLE in_$left");
	my ($ret, $out, $err) =
	  $node->psql('postgres', "DROP TABLESPACE left_$left");
	is("$ret|$err", '0|',
		$left eq 'moved'
		? 'DROP TABLESPACE of a location a fork table moved out of empties the fork\'s packs out of it'
		: 'DROP TABLESPACE of a location a fork table left empties the fork\'s packs out of it'
	);
	ok(!-e "$pgdata/pg_tblspc/$space",
		"the dropped tablespace left_$left leaves no directory");
}
is( $node->safe_psql(
		'postgres', 'SELECT count(*) || \' \' || sum(k) FROM in_moved'),
	'500 125250',
	"the moved table reads every row after its old tablespace drops");

# Bugs row 99: a base backup keeps every collection from running, so DROP
# TABLESPACE of a location that holds the fork's packs waits for the backup's
# end, where Postgres drops it at once: a cancel ends the wait with Postgres'
# error and keeps the tablespace, and a DROP waiting at pg_backup_stop then
# drops it.
$node->safe_psql('postgres',
	"CREATE TABLESPACE backed LOCATION ''; CREATE TABLE in_backed (k int4) TABLESPACE backed; INSERT INTO in_backed SELECT generate_series(1, 100)"
);
$node->safe_psql('postgres',
	"SELECT doltgres_test_fold('in_backed'); ALTER TABLE in_backed SET TABLESPACE pg_default; CHECKPOINT"
);
my $backed = $node->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'backed'");
my $backup = $node->background_psql('postgres');
$backup->query_safe("SELECT pg_backup_start('row99', true)");
my $waiting =
  q{SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE query = 'DROP TABLESPACE backed;' AND wait_event_type = 'Extension')};
my $dropper = $node->background_psql('postgres', on_error_stop => 0);
$dropper->query_safe('\\set VERBOSITY verbose');
$dropper->query_until(qr/start/, "\\echo start\nDROP TABLESPACE backed;\n");
$node->poll_query_until('postgres', $waiting);
$node->safe_psql('postgres',
	"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query = 'DROP TABLESPACE backed;'"
);
$dropper->query('SELECT 1');
like(
	$dropper->{stderr},
	qr/ERROR:  57014: canceling statement due to user request/,
	"a cancel ends DROP TABLESPACE's wait for a base backup with Postgres' error"
);
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_tablespace WHERE spcname = 'backed'"),
	'1',
	"a canceled DROP TABLESPACE keeps the tablespace");
$dropper->{stderr} = '';
$dropper->query_until(qr/again/, "\\echo again\nDROP TABLESPACE backed;\n");
$node->poll_query_until('postgres', $waiting);
$backup->query('SELECT 1 FROM pg_backup_stop()');
$dropper->query('SELECT 1');
is( $dropper->{stderr}
	  . (
		$node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_tablespace WHERE spcname = 'backed'")),
	'0',
	"DROP TABLESPACE during a base backup drops the tablespace once pg_backup_stop ends the backup"
);
ok(!-e "$pgdata/pg_tblspc/$backed",
	"the tablespace dropped after the backup leaves no directory");
$_->quit for $backup, $dropper;

# Bugs row 99 on a hot standby: its replay of DROP TABLESPACE empties its own
# packs out of the tablespace, so the replay removes the tablespace's
# directories and the moved table reads every row; and when a reader holds
# the packs through the replay, Postgres' replay logs what it couldn't
# remove, and the collector's sweep removes the fork's directory of packs
# once the reader ends, with no restart.
$node->backup('spaced');
my $spaced = PostgreSQL::Test::Cluster->new('spaced_standby');
$spaced->init_from_backup($node, 'spaced', has_streaming => 1);
$spaced->append_conf('postgresql.conf', 'max_standby_streaming_delay = 1s');
$spaced->start;
my $spaced_dir = $spaced->data_dir;
# standby_space makes a tablespace with a table whose fold the standby
# replays into packs of its own there, and returns the tablespace's id.
sub standby_space
{
	my ($held) = @_;
	$node->safe_psql('postgres',
		"CREATE TABLESPACE standby_$held LOCATION ''; CREATE TABLE on_$held (k int4 PRIMARY KEY, v text) TABLESPACE standby_$held; INSERT INTO on_$held SELECT g, repeat('w', 40) || g FROM generate_series(1, 300) g"
	);
	$node->safe_psql('postgres',
		"SELECT doltgres_test_fold('on_$held'); CHECKPOINT");
	$node->wait_for_replay_catchup($spaced);
	my $space = $node->safe_psql('postgres',
		"SELECT oid FROM pg_tablespace WHERE spcname = 'standby_$held'");
	my $made = time + $PostgreSQL::Test::Utils::timeout_default;
	sleep 1
	  while !(() =
		glob("$spaced_dir/pg_tblspc/$space/PG_*/pg_versioned/*/*.pack"))
	  && time < $made;
	return $space;
}

# standby_moved moves a tablespace's table out to pg_default and drops the
# tablespace, each replayed on the standby.
sub standby_moved
{
	my ($held) = @_;
	$node->safe_psql('postgres',
		"ALTER TABLE on_$held SET TABLESPACE pg_default; CHECKPOINT");
	$node->wait_for_replay_catchup($spaced);
	$node->safe_psql('postgres', "DROP TABLESPACE standby_$held");
	$node->wait_for_replay_catchup($spaced);
}
my $free = standby_space('free');
standby_moved('free');
ok( !-e "$spaced_dir/pg_tblspc/$free",
	"a hot standby's replay of DROP TABLESPACE removes the tablespace's directories with the fork's packs"
);
is( $spaced->safe_psql(
		'postgres', 'SELECT count(*) || \' \' || sum(k) FROM on_free'),
	'300 45150',
	"the standby reads every row of the table moved out of the dropped tablespace"
);
my $read = standby_space('read');
$spaced->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-shared-reading', 'wait')");
my $holder = $spaced->background_psql('postgres', on_error_stop => 0);
$holder->query_until(qr/reading/,
	"\\echo reading\nSELECT doltgres_test_shared(11, 0, 0);\n");
$spaced->wait_for_event('client backend', 'doltgres-shared-reading');
standby_moved('read');
$spaced->psql('postgres',
	"SELECT injection_points_wakeup('doltgres-shared-reading')");
$holder->query('SELECT 1');
like(
	$holder->{stderr},
	qr/ERROR:  canceling statement due to conflict with recovery/,
	"a standby reader that holds the packs through the replay of DROP TABLESPACE is canceled as Postgres' replay cancels a tablespace's users"
);
ok( !-e "$spaced_dir/pg_tblspc/$read",
	"and the replay's retry after the cancel empties the fork's packs out of the tablespace and removes its directories, with no restart"
);
$spaced->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-shared-reading')");
$holder->quit;
is( $spaced->safe_psql(
		'postgres', 'SELECT count(*) || \' \' || sum(k) FROM on_read'),
	'300 45150',
	"the standby reads every row of the table moved out of that tablespace");
$spaced->stop;

# Bugs row 99 on a standby without hot standby: no collector worker runs to
# empty its packs at the replay of DROP TABLESPACE, so Postgres' replay logs
# what it couldn't remove after leaving the dropped mark, and once the
# standby starts again with hot standby, after a crash, the collector's
# sweep empties the fork's packs out of the marked tablespace.
$node->backup('cold');
my $cold = PostgreSQL::Test::Cluster->new('cold_standby');
$cold->init_from_backup($node, 'cold', has_streaming => 1);
$cold->append_conf('postgresql.conf', 'hot_standby = off');
$cold->start;
my $cold_dir = $cold->data_dir;
# cold_replayed waits until the standby without hot standby has replayed the
# primary's WAL to its current end, read from the primary's view of it.
sub cold_replayed
{
	my $lsn = $node->lsn('insert');
	$node->poll_query_until('postgres',
		"SELECT replay_lsn >= '$lsn' FROM pg_stat_replication WHERE application_name = 'cold_standby'"
	) or die "the standby without hot standby didn't replay to $lsn";
}
$node->safe_psql('postgres',
	"CREATE TABLESPACE cold_kept LOCATION ''; CREATE TABLE on_cold (k int4 PRIMARY KEY, v text); INSERT INTO on_cold SELECT g, repeat('c', 40) || g FROM generate_series(1, 300) g"
);
$node->safe_psql('postgres',
	"SELECT doltgres_test_fold('on_cold'); ALTER TABLE on_cold SET TABLESPACE cold_kept; CHECKPOINT"
);
my $cold_space = $node->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'cold_kept'");
cold_replayed();
my @cold_packs =
  glob("$cold_dir/pg_tblspc/$cold_space/PG_*/pg_versioned/*/*.pack");
ok( @cold_packs > 0,
	"a standby without hot standby replays a move into a tablespace as packs of its own there"
);
$node->safe_psql('postgres',
	'ALTER TABLE on_cold SET TABLESPACE pg_default; CHECKPOINT');
$node->safe_psql('postgres', 'DROP TABLESPACE cold_kept');
cold_replayed();
my @cold_left = glob("$cold_dir/pg_tblspc/$cold_space/PG_*/pg_versioned");
ok( @cold_left == 1,
	"a standby without hot standby keeps the fork's packs through its replay of DROP TABLESPACE"
);
ok(-e "$cold_left[0]/dropped",
	"and its replay leaves the dropped mark on them");
$cold->stop('immediate');
$cold->append_conf('postgresql.conf', 'hot_standby = on');
$cold->start;
my $cold_swept = time + $PostgreSQL::Test::Utils::timeout_default;
sleep 1 while -e $cold_left[0] && time < $cold_swept;
ok( !-e $cold_left[0],
	"the collector's sweep empties the fork's packs out of a tablespace whose catalog row is gone"
);
is( $cold->safe_psql(
		'postgres', 'SELECT count(*) || \' \' || sum(k) FROM on_cold'),
	'300 45150',
	"the swept standby reads every row of the table moved out of the tablespace"
);
$cold->stop;

# Bugs row 99: a table another session creates in the tablespace while DROP
# TABLESPACE waits for a base backup stops the DROP's emptying at its next
# round, before any of the new table's packs could be moved, so Postgres
# refuses the DROP as not empty, as it would with no wait.
$node->safe_psql('postgres',
	"CREATE TABLESPACE raced LOCATION ''; CREATE TABLE in_raced (k int4) TABLESPACE raced; INSERT INTO in_raced SELECT generate_series(1, 100)"
);
$node->safe_psql('postgres',
	"SELECT doltgres_test_fold('in_raced'); ALTER TABLE in_raced SET TABLESPACE pg_default; CHECKPOINT"
);
my $raced_backup = $node->background_psql('postgres');
$raced_backup->query_safe("SELECT pg_backup_start('raced', true)");
my $racing = $node->background_psql('postgres', on_error_stop => 0);
$racing->query_safe('\\set VERBOSITY verbose');
$racing->query_until(qr/raced/, "\\echo raced\nDROP TABLESPACE raced;\n");
$node->poll_query_until('postgres',
	q{SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE query = 'DROP TABLESPACE raced;' AND wait_event_type = 'Extension')}
);
$node->safe_psql('postgres',
	'CREATE TABLE in_raced_late (k int4) TABLESPACE raced');
$racing->query('SELECT 1');
like(
	$racing->{stderr},
	qr/ERROR:  55000: tablespace "raced" is not empty/,
	"a table created in the tablespace while DROP TABLESPACE waits stops its emptying, and Postgres refuses the DROP"
);
$raced_backup->query('SELECT 1 FROM pg_backup_stop()');
$_->quit for $racing, $raced_backup;

$node->stop;
done_testing();
