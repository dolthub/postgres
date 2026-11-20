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

# The server's host's file door against its contract (design item 14), through
# the test module: a file written and synced reads back, a short read and a
# missing or existing file raise Postgres' errors, a durable rename and removal
# show in the folder's listing, an error between opening and closing a file
# closes it, a prefetch never raises, an error raised inside a file write
# comes back unchanged, a rename shows in the folder once synced, the file
# door's wait events are registered, and the doors that ask whether a file or
# directory is there answer in their value with no error raised; and a scratch
# file is a temporary file of its transaction, as Postgres' own work files
# are: it reads back what was written, it is refused with no resource owner,
# it is deleted when it closes, after an error, at its backend's termination,
# and by the restart after a crash, a commit finds none still open, a write
# past temp_file_limit raises Postgres' error, log_temp_files logs it, and it
# is made in the temporary tablespace the transaction prepared.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('file');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins(); CREATE EXTENSION injection_points');

is( $node->safe_psql(
		'postgres',
		"SELECT name FROM pg_wait_events WHERE type = 'Extension' AND name LIKE 'versioned file%' ORDER BY name"
	),
	"versioned file read\nversioned file sync\nversioned file write",
	"the file door's wait events are registered");

my $session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe('\set VERBOSITY verbose');

# raised runs a step that raises, returning what it printed and clearing it.
sub raised
{
	my ($step) = @_;
	$session->query("SELECT doltgres_test_file($step)");
	my $stderr = $session->{stderr};
	$session->{stderr} = '';
	return $stderr;
}

is($session->query_safe('SELECT doltgres_test_file(0)'),
	'versioned file 14', 'a file written and synced reads back');
like(
	raised(1),
	qr/ERROR:  XX001: could not read file "doltgres_test_files\/a": read 14 of 20/,
	'a read past the end of a file is a short read');
is($session->query_safe('SELECT doltgres_test_file(2)'),
	'versioned 9', 'a truncated file has the length it was cut to');
is($session->query_safe('SELECT doltgres_test_file(3)'),
	'a,c;b,c;b', 'a durable rename moves the file');
ok(-f $node->data_dir . '/doltgres_test_files/b'
	  && !-e $node->data_dir . '/doltgres_test_files/c',
	'a durable removal removes the file');
like(
	raised(4),
	qr/ERROR:  58P01: could not open file "doltgres_test_files\/missing": No such file or directory/,
	'opening a missing file raises 58P01');
my $number = $session->query_safe('SELECT doltgres_test_file(19)');
like(
	raised(5),
	qr/ERROR:  54000: file offset 18446744073709551615 is too large/,
	'a read past the range of offsets fails with its file open');
is($session->query_safe('SELECT doltgres_test_file(19)'),
	$number, 'an error between opening and closing a file closes it');
is($session->query_safe('SELECT doltgres_test_file(6)'),
	'prefetched', 'a prefetch never raises');
like(
	raised(8),
	qr/ERROR:  58P02: could not open file "doltgres_test_files\/b": File exists/,
	'creating a file that exists raises 58P02');

# An error raised inside a file write comes back unchanged, and the file is
# closed, so the next write goes through.
$session->query_safe('SELECT injection_points_set_local()');
$session->query_safe(
	"SELECT injection_points_attach('doltgres-file-write', 'error')");
like(
	raised(7),
	qr/ERROR:  XX000: error triggered for injection point doltgres-file-write/,
	'an error raised inside a file write comes back unchanged');
$session->query_safe("SELECT injection_points_detach('doltgres-file-write')");
is($session->query_safe('SELECT doltgres_test_file(7)'),
	'versioned again 15', 'the next write goes through');
is($session->query_safe('SELECT doltgres_test_file(9)'),
	'd;versioned again 15', 'a rename shows in the folder once synced');

# The doors that ask whether something is there answer in their value, with no
# error raised in the server, and every other failure raises as before.
my $answered = -s $node->logfile;
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(14)'),
	'true versioned again 15',
	"a file that isn't there is answered, and one that is there is opened");
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(15)'),
	'true true',
	'a file is created once, and the file already there is answered');
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(16)'),
	'true d,e',
	"a directory that isn't there is answered, and one that is there is listed");
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(17)'),
	'true false d',
	'a file is removed once, and the file no longer there is answered');
ok(!$node->log_contains(qr/ERROR/, $answered),
	'the answered conditions raise no error in the server');
like(
	raised(18),
	qr/ERROR:  58P01: could not open file "doltgres_test_files\/nowhere\/e": No such file or directory/,
	"a file created in a directory that isn't there raises 58P01");

# The doors that tidy up after a failure raise nothing: a removal or rename that
# didn't happen is one line in the server's log.
my $tidied = -s $node->logfile;
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(20)'),
	'd',
	"a leftover file is removed, and one that isn't there is reported nowhere");
ok(!$node->log_contains(qr/could not remove file/, $tidied),
	"a leftover that isn't there is reported nowhere in the server's log");
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(21)'),
	'd', "a leftover that can't be removed raises nothing");
ok( $node->log_contains(
		qr/LOG:  could not remove file "doltgres_test_files": /, $tidied),
	"a leftover that can't be removed is reported at LOG");
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(22)'),
	'true false g',
	"a rename taken back answers whether it took, and one that can't is reported at LOG with no error"
);
ok( $node->log_contains(
		qr/LOG:  could not open file "doltgres_test_files\/d": No such file or directory/,
		$tidied)
	  && !$node->log_contains(qr/ERROR|PANIC/, $tidied),
	"a rename that can't be taken back is reported at LOG, with no error");

# A report inside a door that can't raise takes no pending cancel: the door
# returns, the code after it runs, and the cancel ends the statement after.
sub cancelled
{
	my ($step) = @_;
	$session->query(
		"SELECT pg_cancel_backend(pg_backend_pid()), doltgres_test_file($step)"
	);
	my $stderr = $session->{stderr};
	$session->{stderr} = '';
	return $stderr;
}
my $held = -s $node->logfile;
like(
	cancelled(23),
	qr/ERROR:  57014: canceling statement due to user request/,
	"a cancel pending when a leftover's removal is reported ends the statement");
ok( -f $node->data_dir . '/doltgres_test_files/after'
	  && $node->log_contains(
		qr/LOG:  could not remove file "doltgres_test_files": /, $held),
	"the removal is reported and the code after it runs before the cancel");
like(
	cancelled(24),
	qr/ERROR:  57014: canceling statement due to user request/,
	"a cancel pending when a rename can't be taken back ends the statement");
ok(-f $node->data_dir . '/doltgres_test_files/after-false',
	"the rename answers that it didn't take and the code after it runs before the cancel"
);
$session->query_safe('SELECT injection_points_set_local()');
$session->query_safe(
	"SELECT injection_points_attach('doltgres-file-close', 'notice')");
like(
	cancelled(25),
	qr/NOTICE:  00000: notice triggered for injection point doltgres-file-close.*ERROR:  57014: canceling statement due to user request/s,
	"a cancel pending when a close reports ends the statement (bugs row 27)");
$session->query_safe("SELECT injection_points_detach('doltgres-file-close')");
ok(-f $node->data_dir . '/doltgres_test_files/closed',
	'the close returns and the code after it runs before the cancel');
like(
	cancelled(26),
	qr/ERROR:  57014: canceling statement due to user request/,
	"a cancel pending when a call that doesn't raise reports ends the statement");
ok( -f $node->data_dir . '/doltgres_test_files/reported'
	  && $node->log_contains(
		qr/LOG:  a report inside a wrapper that does not raise/, $held),
	'the report comes out and the code after the call runs before the cancel');
is($session->query_safe('SELECT 1'), '1',
	'the backend that was cancelled at each door still runs');
ok(!$node->log_contains(qr/PANIC/, $held),
	'a report with a cancel pending panics nowhere');

# A tree is removed with everything in it, and a directory that isn't there
# answers that it is gone, with no report.
is($session->query('SELECT doltgres_test_file(27)'),
	'true true false', 'a tree is removed with everything in it');
unlike($session->{stderr}, qr/WARNING/,
	"a tree that isn't there is gone, with no report");
$session->{stderr} = '';

# A file that can't be removed stays, with the directories above it: the
# removal reports it as Postgres' removal of a dropped database's directory
# does, and answers that something was left. Once the file can be removed,
# the tree goes. Each removal runs in a session of its own, with no timeout,
# since Windows tries a refused removal again for ten seconds.
$session->query_safe('SELECT doltgres_test_file(28)');
$session->quit;
my $kept = $node->data_dir . '/doltgres_test_files/kept';
my $open;
if ($windows_os)
{
	open($open, '<', "$kept/held") or die "could not open $kept/held: $!";
}
else
{
	chmod(0555, $kept) or die "could not change the mode of $kept: $!";
}
SKIP:
{
	skip 'a superuser removes a file whatever its directory permits', 2
	  if !$windows_os && $> == 0;
	my ($status, $left, $warned) = $node->psql('postgres',
		"\\set VERBOSITY verbose\nSELECT doltgres_test_file(29)");
	is($left, 'false',
		"a tree with a file that can't be removed answers that something was left"
	);
	like(
		$warned,
		qr/WARNING:  01000: could not remove file "doltgres_test_files\/kept\/held": .*WARNING:  01000: some useless files may be left behind in old database directory "doltgres_test_files\/kept"/s,
		"what a tree's removal leaves is reported as Postgres reports a dropped database's"
	);
}
if ($windows_os)
{
	close($open) or die "could not close $kept/held: $!";
}
else
{
	chmod(0755, $kept) or die "could not change the mode of $kept: $!";
}
is($node->safe_psql('postgres', 'SELECT doltgres_test_file(29)'),
	'true', 'a tree whose file can be removed again is removed');

# A scratch file is a temporary file of the database, deleted when it closes.
$session = $node->background_psql('postgres', on_error_stop => 0);
$session->query_safe('\set VERBOSITY verbose');
my $temporary = 'SELECT count(*) FROM pg_ls_tmpdir()';
like(
	raised(12),
	qr/ERROR:  XX000: no resource owner for a versioned scratch file/,
	'a scratch file with no resource owner is refused');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-scratch-open', 'wait')");

# holds starts a backend that writes a scratch file and holds it open at the
# injection point.
sub holds
{
	my ($before) = @_;
	my $holder = $node->background_psql('postgres', on_error_stop => 0);
	$holder->query_safe($before) if $before ne '';
	$holder->query_until(qr/start/,
		"\\echo start\nSELECT doltgres_test_file(10);\n");
	$node->wait_for_event('client backend', 'doltgres-scratch-open');
	return $holder;
}
my $holder = holds('');
is($node->safe_psql('postgres', $temporary),
	'1', "an open scratch file is among the database's temporary files");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-scratch-open')");
ok( pump_until(
		$holder->{run}, $holder->{timeout},
		\$holder->{stdout}, qr/^versioned scratch 17$/m),
	'a scratch file reads back what was written to it');
is($node->safe_psql('postgres', $temporary),
	'0', 'a scratch file is deleted when it closes');
$holder->quit;

# It is made in the temporary tablespace its transaction prepared, as a sort
# that spills prepares it.
my $space = $node->basedir . '/scratch_space';
mkdir($space);
$node->safe_psql('postgres', "CREATE TABLESPACE scratch_space LOCATION '$space'");
$holder = holds(
	"BEGIN; SET LOCAL temp_tablespaces = scratch_space; SET LOCAL work_mem = '64kB'; SELECT count(*) FROM (SELECT g FROM generate_series(1, 100000) g ORDER BY g) s"
);
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_ls_tmpdir((SELECT oid FROM pg_tablespace WHERE spcname = 'scratch_space'))"
	),
	'1',
	"a scratch file is made in its transaction's temporary tablespace");
is($node->safe_psql('postgres', $temporary),
	'0', "and not among the database's own temporary files");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-scratch-open')");
ok( pump_until(
		$holder->{run}, $holder->{timeout},
		\$holder->{stdout}, qr/^versioned scratch 17$/m),
	'the scratch file in the temporary tablespace reads back');
$holder->quit;

# A terminated backend's scratch file is deleted as it exits.
$holder = holds('');
my $holder_pid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE wait_event = 'doltgres-scratch-open'");
$node->safe_psql('postgres', "SELECT pg_terminate_backend($holder_pid)");
ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $holder_pid"),
	'the holder is terminated');
is($node->safe_psql('postgres', $temporary),
	'0', "a terminated backend's scratch file is deleted");
$holder->{run}->finish;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-scratch-open')");

# An error with a scratch file open leaves none, a write past temp_file_limit
# raises Postgres' error, and a commit finds no scratch file still open.
like(
	raised(13),
	qr/ERROR:  XX000: the test failed with its scratch file open/,
	'a step fails with its scratch file open');
is($node->safe_psql('postgres', $temporary),
	'0', 'an error with a scratch file open leaves none');
$session->query_safe("SET temp_file_limit = '1MB'");
like(
	raised(11),
	qr/ERROR:  53400: temporary file size exceeds "temp_file_limit" \(1024kB\)/,
	"a write past temp_file_limit raises Postgres' error");
$session->query_safe('RESET temp_file_limit');
is($node->safe_psql('postgres', $temporary),
	'0', 'a write refused past temp_file_limit leaves no file');
$session->query('BEGIN; SELECT doltgres_test_file(10); COMMIT');
is($session->{stderr}, '',
	"a commit after a scratch file's use finds none still open");
$session->{stderr} = '';

# log_temp_files logs a scratch file when it closes.
$node->append_conf('postgresql.conf', 'log_temp_files = 0');
$node->reload;
my $logged = -s $node->logfile;
$node->safe_psql('postgres', 'SELECT doltgres_test_file(10)');
$node->wait_for_log(qr/temporary file: path "[^"]*pgsql_tmp[^"]*", size 17/,
	$logged);
pass('log_temp_files logs a scratch file');
$session->quit;

# The restart after a crash deletes a scratch file its backend left.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-scratch-open', 'wait')");
$holder = holds('');
$node->stop('immediate');
$holder->{run}->finish;
my @left = glob($node->data_dir . '/base/pgsql_tmp/pgsql_tmp*');
is(scalar(@left), 1, 'a crash leaves an open scratch file behind');
$node->start;
@left = glob($node->data_dir . '/base/pgsql_tmp/pgsql_tmp*');
is(scalar(@left), 0, 'the restart after a crash deletes the scratch file');

# A database's versioned files are in pg_versioned in the data directory for
# pg_default, and in a tablespace's version directory for any other.
my $version = $node->safe_psql('postgres',
	"SELECT 'PG_' || current_setting('server_version_num')::int / 10000 || '_' || catalog_version_no FROM pg_control_system()"
);
my $default = $node->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default'");
(my $dirs = $node->safe_psql('postgres', 'SELECT doltgres_test_file(30)')) =~
  s{\\}{/}g;
is( $dirs,
	"$default;pg_versioned/5;pg_tblspc/16400/$version/pg_versioned/5",
	"a database's versioned files are in pg_versioned in the data directory and in a tablespace's version directory"
);

done_testing();
