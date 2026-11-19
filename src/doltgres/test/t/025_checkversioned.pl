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

# pg_checkversioned, the check of a stopped cluster's versioned storage
# (design item D12): a cluster with only initdb's repositories, after a clean stop and
# after a crash's recovery and a clean stop, exits 0 and prints nothing; a
# dropped repository's directory prints what the next start settles only with
# --verbose; a repository the server made finds a flipped pack byte and a
# damaged newest manifest, whose fallback's WAL it reads; a running cluster, or no data directory, exits 1, as pg_checksums
# refuses.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# slurp_binary returns a file's bytes as they are, on every platform.
sub slurp_binary
{
	my ($path) = @_;
	open my $fh, '<:raw', $path or die "could not read $path: $!";
	local $/;
	my $bytes = <$fh>;
	close $fh;
	return $bytes;
}

# append_binary appends bytes to a file as they are, on every platform.
sub append_binary
{
	my ($path, $bytes) = @_;
	open my $fh, '>>:raw', $path or die "could not write $path: $!";
	print $fh $bytes;
	close $fh or die "could not write $path: $!";
	return;
}

program_help_ok('pg_checkversioned');
program_version_ok('pg_checkversioned');
program_options_handling_ok('pg_checkversioned');

my $node = PostgreSQL::Test::Cluster->new('checkversioned');
$node->init;
my $pgdata = $node->data_dir;

# A cluster with only initdb's repositories, cleanly stopped, finds nothing.
$node->start;
$node->safe_psql('postgres', 'CREATE TABLE written (i int); INSERT INTO written VALUES (1)');
$node->stop;
my ($stdout, $stderr);
IPC::Run::run [ 'pg_checkversioned', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
is($? >> 8, 0, 'a clean cluster exits 0');
is($stdout, '', 'a clean cluster prints nothing');

# After a crash, recovery, and a clean stop, it still finds nothing.
$node->start;
$node->safe_psql('postgres', 'INSERT INTO written VALUES (2)');
$node->stop('immediate');
$node->start;
$node->stop;
IPC::Run::run [ 'pg_checkversioned', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
is($? >> 8, 0, 'a cluster recovered and cleanly stopped exits 0');

# A dropped repository's directory is what the next start removes: a line
# printed only with --verbose, and no finding.
mkdir "$pgdata/pg_versioned" unless -d "$pgdata/pg_versioned";
mkdir "$pgdata/pg_versioned/99999" or die;
open my $mark, '>', "$pgdata/pg_versioned/99999/dropped" or die;
close $mark;
IPC::Run::run [ 'pg_checkversioned', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
is($? >> 8, 0, 'a dropped repository exits 0');
is($stdout, '', 'a dropped repository prints nothing without --verbose');
IPC::Run::run [ 'pg_checkversioned', '-v', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
like($stdout,
	qr/^database 99999: is dropped, whose directories the next start removes$/m,
	'--verbose prints what the next start settles');

# A tablespace whose location is missing is a note, printed only with
# --verbose.
mkdir "$pgdata/pg_tblspc/77777" or die;
IPC::Run::run [ 'pg_checkversioned', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
is($? >> 8, 0, 'a tablespace whose location is missing exits 0');
unlike($stdout, qr/tablespace 77777/, 'a missing location prints nothing without --verbose');
IPC::Run::run [ 'pg_checkversioned', '-v', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
like($stdout,
	qr/^tablespace 77777's location "[^"]*" is missing, so the versioned files in it are not checked$/m,
	'--verbose notes a tablespace whose location is missing');
rmdir "$pgdata/pg_tblspc/77777" or die;

# A repository the server made, through the test module, finds nothing after
# a clean stop; a flipped byte of its pack is found, naming the pack; and a
# newest manifest that isn't whole is found, with the WAL its fallback would
# replay read.
SKIP:
{
	skip 'Injection points not supported by this build', 5
	  if $ENV{enable_injection_points} ne 'yes';

	mkdir "$pgdata/pg_versioned" unless -d "$pgdata/pg_versioned";
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
	$node->safe_psql('postgres', 'SELECT doltgres_test_repository(88888)');
	$node->stop;
	IPC::Run::run [ 'pg_checkversioned', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
	is($? >> 8, 0, 'a repository the server made exits 0');

	my $repository = "$pgdata/pg_versioned/88888";
	opendir my $dir, $repository or die;
	my ($pack) = grep { /\.pack$/ } readdir $dir;
	closedir $dir;
	my $whole = slurp_binary("$repository/$pack");
	my $flipped = $whole;
	my $at = int(length($whole) / 2);
	substr($flipped, $at, 1) = chr(ord(substr($flipped, $at, 1)) ^ 0xff);
	append_binary("$repository/$pack.new", $flipped);
	rename "$repository/$pack.new", "$repository/$pack" or die;
	IPC::Run::run [ 'pg_checkversioned', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
	is($? >> 8, 2, 'a flipped pack byte exits 2 naming the pack');
	like($stdout, qr/^database 88888: .*\Q$pack\E/m, 'the finding names the pack');
	unlink "$repository/$pack";
	append_binary("$repository/$pack", $whole);

	my $manifest = slurp_binary("$repository/manifest");
	my $damaged = $manifest;
	substr($damaged, 0, 1) = chr(ord(substr($damaged, 0, 1)) ^ 0xff);
	unlink "$repository/manifest";
	append_binary("$repository/manifest", $damaged);
	IPC::Run::run [ 'pg_checkversioned', '-v', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
	is($? >> 8, 2, 'a damaged newest manifest exits 2, its fallback range read');
	like($stdout, qr/^database 88888: its newest manifest /m, 'the finding names the newest manifest');
	unlink "$repository/manifest";
	append_binary("$repository/manifest", $manifest);
}

# On a promoted cluster, a range a fallback would replay that starts on the
# timeline before the promotion is read through the timeline history: its
# previous manifest put back as the save left it, the generations differ, and
# the WAL from the newest's redo point reads across the switch.
SKIP:
{
	skip 'Injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';

	my $promoted = PostgreSQL::Test::Cluster->new('promoted');
	$promoted->init(allows_streaming => 1);
	$promoted->append_conf('postgresql.conf', "wal_keep_size = '1GB'");
	my $data = $promoted->data_dir;
	mkdir "$data/pg_versioned" unless -d "$data/pg_versioned";
	$promoted->start;
	$promoted->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
	$promoted->safe_psql('postgres', 'SELECT doltgres_test_repository(66666)');
	my $saved = slurp_binary("$data/pg_versioned/66666/manifest.prev");
	$promoted->stop;
	$promoted->set_standby_mode;
	$promoted->start;
	$promoted->promote;
	$promoted->safe_psql('postgres', 'CHECKPOINT');
	$promoted->stop;
	unlink "$data/pg_versioned/66666/manifest.prev";
	append_binary("$data/pg_versioned/66666/manifest.prev", $saved);
	IPC::Run::run [ 'pg_checkversioned', '-v', '-D', $data ], '>', \$stdout, '2>', \$stderr;
	is($? >> 8, 0, 'a range that starts before the promotion exits 0');
	like($stdout, qr/^database 66666: its manifest's two generations differ/m,
		'the differing generations are what the next start settles');
	unlike($stdout, qr/could not read the WAL kept/, 'the range reads across the timeline switch');
}

# A running cluster is refused, as pg_checksums refuses one.
$node->start;
command_fails_like([ 'pg_checkversioned', '-D', $pgdata ],
	qr/cluster must be shut down/, 'a running cluster exits 1');
IPC::Run::run [ 'pg_checkversioned', '-D', $pgdata ], '>', \$stdout, '2>', \$stderr;
is($? >> 8, 1, 'a running cluster exits with 1');
$node->stop;

# No data directory is refused.
{
	local $ENV{PGDATA};
	delete $ENV{PGDATA};
	command_fails_like([ 'pg_checkversioned' ],
		qr/no data directory specified/, 'no data directory exits 1');
}

done_testing();
