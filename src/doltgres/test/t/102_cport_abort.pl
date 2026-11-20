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

# The C port's deaths (the port text's section 6): intrinsics::abort's trap
# kills the backend with the trap's signal or exception, and a panic in a
# drop that unwinding runs is panic_in_cleanup, whose report and line come
# before process::abort's death, each as the postmaster logs it.

use strict;
use warnings FATAL => 'all';
use POSIX ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('cport_abort');
$node->init;
$node->append_conf('postgresql.conf', 'restart_after_crash = on');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_cport_test');

# The trap is ud2 on x86-64, an illegal instruction, and brk on arm64, a
# trace trap; process::abort is Windows' fail-fast, elsewhere SIGABRT.
my $arm64 = (POSIX::uname())[4] =~ /^(aarch64|arm64)$/;
my $trapped =
	$windows_os ? qr/terminated by exception 0xC000001D/
  : $arm64      ? qr/terminated by signal 5:/
  :               qr/terminated by signal 4:/;
my $aborted =
  $windows_os
  ? qr/terminated by exception 0xC0000409/
  : qr/terminated by signal 6:/;

# die_by runs a function that kills its backend and waits for the restart, giving the log it wrote.
sub die_by
{
	my ($function) = @_;
	my $offset = -s $node->logfile;
	my ($ret) = $node->psql('postgres', "SELECT $function()");
	isnt($ret, 0, "$function kills its backend");
	$node->poll_query_until('postgres', 'SELECT 1', '1')
	  or die "the server didn't restart after $function";
	return substr(slurp_file($node->logfile), $offset);
}

my $log = die_by('doltgres_cport_test_trap');
like($log, $trapped, 'intrinsics::abort dies of the trap');

# Off Windows the cleanup's terminate edge is panic_in_cleanup, which writes
# that the panic can't unwind; on Windows the C++ runtime fails fast.
$log = die_by('doltgres_cport_test_panic_in_cleanup');
if ($windows_os)
{
	unlike(
		$log,
		qr/non-unwinding panic/,
		'a panic out of a cleanup writes no more on Windows');
	unlike(
		$log,
		qr/the inner drop ran/,
		'a panic out of a cleanup unwinds no frame on Windows');
}
else
{
	like(
		$log,
		qr/thread caused non-unwinding panic\. aborting\./,
		'panic_in_cleanup writes that the panic can\'t unwind');
	like(
		$log,
		qr/the inner drop ran.*thread caused non-unwinding panic\. aborting\./s,
		'the second panic unwinds the panicking drop\'s frame before panic_in_cleanup'
	);
}
like($log, $aborted, 'a panic out of a cleanup aborts');

is( $node->safe_psql(
		'postgres', 'SELECT doltgres_cport_test_catch_in_cleanup()'),
	'the first panic',
	'a drop that catches a panic of its own leaves the unwind it runs for');

# Host's tests run with std's own hooks, as `cargo test -p host` does: std's
# default panic hook reports a panic on stderr, and std's default alloc
# error hook reports a failed allocation before process::abort. $log is
# still panic_in_cleanup's.
like(
	$log,
	qr/thread 'main' \(\d+\) panicked at doltgres_cport_test\.rs:1:1:\nthe first panic\n/,
	'std\'s default hook reports a panic in host\'s tests');
$log = die_by('doltgres_cport_test_alloc_failure');
like(
	$log,
	qr/memory allocation of 9223372036854775808 bytes failed\n/,
	'std\'s default alloc error hook reports a failed allocation in host\'s tests'
);
like($log, $aborted, 'a failed allocation in host\'s tests aborts');

$node->stop;
done_testing();
