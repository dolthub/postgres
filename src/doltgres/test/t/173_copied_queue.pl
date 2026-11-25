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

# Bugs row 451: a whole copy of a repository's directory, by ALTER DATABASE
# SET TABLESPACE or by CREATE DATABASE from a template, never meets a removal
# of its repository's queued files. With the collector held after it retired
# and queued a dead pack, and the copy held after it listed the directory, a
# checkpoint's removal pass skips the repository rather than waiting on the
# copy, the copy finishes and leaves the queued files out, and a later pass
# removes them.
use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('copied');
$node->init;
$node->append_conf('postgresql.conf', 'allow_in_place_tablespaces = on');
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION doltgres_test');
$node->safe_psql('postgres', "CREATE TABLESPACE moved LOCATION ''");

# queued makes a database whose first pack is dead, asks the collector to
# collect it, and holds the collector before its removal pass, so the dead
# pack is retired and queued with its files still on disk; it waits at the
# collection's retirement first, so the pass held is the one that retired it,
# and holds the removal pass only from then, so a periodic pass that began
# before the ask goes by and the collector comes back to the ask (bugs row
# 520). A periodic pass that retires the pack first is held the same way, and
# then it is the one held before its removal, which is all the case needs.
# It returns the database's oid.
sub queued
{
	my ($db) = @_;
	$node->safe_psql('postgres', "CREATE DATABASE $db");
	my $oid = $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = '$db'");
	$node->safe_psql($db,
		'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
	$node->safe_psql($db, 'CREATE TABLE gone (k int4 PRIMARY KEY, v text)');
	$node->safe_psql($db,
		"SELECT doltgres_test_write('gone', true, 'INSERT INTO gone SELECT g, repeat(md5(g::text), 20) FROM generate_series(1, 500) g')"
	);
	$node->safe_psql($db, "SELECT doltgres_test_fold('gone')");
	$node->safe_psql($db,
		"SELECT doltgres_test_write('gone', true, 'DELETE FROM gone')");
	$node->safe_psql($db, "SELECT doltgres_test_fold('gone')");
	$node->safe_psql($db,
		"SELECT doltgres_test_write('gone', true, 'INSERT INTO gone VALUES (1, ''kept'')')"
	);
	$node->safe_psql($db, "SELECT doltgres_test_fold('gone')");
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-collector-retired', 'wait')"
	);
	my $asker = $node->background_psql($db, on_error_stop => 0);
	$asker->query_until(qr/start/,
		"\\echo start\nSELECT doltgres_test_collect();\n");
	$node->wait_for_event('versioned collector',
		'doltgres-collector-retired');
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-collector-removing', 'wait'); SELECT injection_points_wakeup('doltgres-collector-retired'); SELECT injection_points_detach('doltgres-collector-retired')"
	);
	$node->wait_for_event('versioned collector',
		'doltgres-collector-removing');
	$node->safe_psql('postgres',
		"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE wait_event = 'versioned collection ask'"
	);
	$asker->quit;
	$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE datname = '$db'")
	  or die "a session stayed in $db";
	return $oid;
}

# files returns the names of the files a directory holds itself.
sub files
{
	my ($dir) = @_;
	return map { basename($_) } grep { -f $_ } glob("$dir/*");
}

# copied runs a statement that copies a repository's directory, held after the
# copy lists it; meanwhile it wakes the collector, whose removal pass runs,
# and asks it for a collection of another database, answered only after that
# pass ends, and then wakes the copy. A checkpoint can't run here, since its
# list writes wait on the files' flag the held copy holds. It runs what comes
# after the copy, if given, before the statement ends, and returns the copy's
# error output and the names the source held when the copy listed it.
sub copied
{
	my ($statement, $source, $after) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-packs-copy-listed', 'wait')"
	);
	my $copier = $node->background_psql('postgres', on_error_stop => 0);
	$copier->query_until(qr/start/,
		"\\echo start\n$statement;\n\\echo done\n");
	$node->wait_for_event('client backend', 'doltgres-packs-copy-listed');
	my @listed = files($source);
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('doltgres-collector-removing'); SELECT injection_points_detach('doltgres-collector-removing')"
	);
	$node->safe_psql('postgres', 'SELECT doltgres_test_collect()');
	is( scalar(grep { !-e "$source/$_" } @listed),
		0,
		"the removal passes leave the files of a repository a copy holds ($statement)"
	);
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('doltgres-packs-copy-listed'); SELECT injection_points_detach('doltgres-packs-copy-listed')"
	);
	$after->() if $after;
	$copier->query_until(qr/done/, '');
	my $err = $copier->{stderr};
	$copier->quit;
	return ($err, @listed);
}

# waiting returns how many processes wait at a wait event.
sub waiting
{
	my ($event) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE wait_event = '$event'");
}

# ALTER DATABASE SET TABLESPACE: the copy leaves out the queued packs' files;
# a removal pass held at an old-space entry whose directory it found completes
# after the move's finish removed that directory; and once the move removed
# its old directory, a removal pass passes over the old space's entries.
my ($moving) = queued('mover');
my $old = $node->data_dir . "/pg_versioned/$moving";

# finished holds the move before its finish, holds a removal pass at an
# old-space entry whose directory it found, lets the finish remove the old
# directory, and then wakes the pass at each entry until it ends.
my $finished = sub {
	$node->wait_for_event('client backend', 'doltgres-store-move-logged');
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('doltgres-removal-checked', 'wait')");
	my $asker = $node->background_psql('postgres', on_error_stop => 0);
	$asker->query_until(qr/start/,
		"\\echo start\nSELECT doltgres_test_collect();\n");
	$node->wait_for_event('versioned collector', 'doltgres-removal-checked');
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('doltgres-store-move-logged'); SELECT injection_points_detach('doltgres-store-move-logged')"
	);
	my $waited = 0;
	while (-d $old)
	{
		die 'the finish never removed the old directory'
		  if $waited++ > 10 * $PostgreSQL::Test::Utils::timeout_default;
		Time::HiRes::usleep(100_000);
	}
	ok(1,
		'the finish removes the old directory while a removal pass holds at one of its entries'
	);
	while (waiting('doltgres-removal-checked'))
	{
		$node->safe_psql('postgres',
			"SELECT injection_points_wakeup('doltgres-removal-checked')");
		die 'the pass never ended'
		  if $waited++ > 20 * $PostgreSQL::Test::Utils::timeout_default;
		Time::HiRes::usleep(100_000);
	}
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('doltgres-removal-checked')");
	$asker->quit;
};

$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-store-move-logged', 'wait')");
my ($err, @listed) =
  copied('ALTER DATABASE mover SET TABLESPACE moved', $old, $finished);
unlike($err, qr/ERROR/, 'the move whose removal pass met its copy succeeds');
is( $node->safe_psql(
		'postgres',
		"SELECT spcname FROM pg_database d JOIN pg_tablespace t ON t.oid = d.dattablespace WHERE datname = 'mover'"
	),
	'moved',
	'the database is in its new tablespace');
my @moved_dirs =
  glob($node->data_dir . "/pg_tblspc/*/*/pg_versioned/$moving");
my %moved = map { $_ => 1 } files($moved_dirs[0]);
my @left = grep { !$moved{$_} } @listed;
ok( @left > 0 && !grep({ !/\.(pack|idx|\d+\.dead)$/ } @left),
	'the move leaves only queued packs\' files out of its copy'
) or diag "left out: @left";
like(
	$node->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	qr/^\d+ \d+ \d+$/,
	'a removal pass after the move passes over the old space\'s entries');
is( $node->safe_psql(
		'mover',
		"SELECT doltgres_test_standing('gone', 'SELECT k || '' '' || v FROM gone')"
	),
	'1 kept',
	'the moved database reads its row');

# CREATE DATABASE from a template: the template's queued files, which a
# removal pass removes once the copy is done, are none of the copy's.
my ($template) = queued('pattern');
my $pattern = $node->data_dir . "/pg_versioned/$template";
($err, @listed) = copied('CREATE DATABASE made TEMPLATE pattern', $pattern);
unlike($err, qr/ERROR/,
	'the creation whose removal pass met its copy succeeds');
my $asked = 0;
while (!grep { !-e "$pattern/$_" } @listed)
{
	last if $asked++ > 10 * $PostgreSQL::Test::Utils::timeout_default;
	$node->safe_psql('postgres', 'SELECT doltgres_test_collect()');
	Time::HiRes::usleep(100_000);
}
my @queued = grep { !-e "$pattern/$_" } @listed;
ok(@queued > 0, 'the template had queued files');
my $made = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'made'");
my %copy = map { $_ => 1 } files($node->data_dir . "/pg_versioned/$made");
is(scalar(grep { $copy{$_} } @queued),
	0, 'the creation leaves the template\'s queued files out of its copy');
is( $node->safe_psql(
		'made',
		"SELECT doltgres_test_standing('gone', 'SELECT k || '' '' || v FROM gone')"
	),
	'1 kept',
	'the new database reads its row');

$node->stop;
done_testing();
