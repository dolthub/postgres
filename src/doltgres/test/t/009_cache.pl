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

# The contract of host's node cache (design item 14's S5) in the server,
# through the test module's steps, whose kept pins last until their
# transaction's resource owner releases them, as a cursor's do, with injection
# points in the shared-memory crate's install forcing each interleaving:
# node_cache_size is a memory setting of 128MB by default, set at server start;
# the versioned node cache is listed with its size; two backends read one
# node; a backend holding a pin keeps its frame while another fills the cache;
# a cursor's pin survives a rollback to a savepoint, and a subtransaction's
# abort releases only its own pins; a terminated backend's pins are released;
# a temporary node is cached in the backend's own cache; a pin with no
# resource owner is refused; two installs of one key end with one frame; two
# installers reaching one victim claim it once; a reader that pins the old key
# between the claim and the partition locks keeps its bytes; a reader of a
# frame being filled gets a miss; a failed allocation growing the pin table is
# refused; a number past the frame count changes nothing; a dropped pin is
# forgotten by its resource owner; a frame a failed fill left is filled by its
# key's next installs; a resource owner remembers more pins than its array
# holds; and a key is asked about without a pin or a use.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('cache');
$node->init;
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
is( $node->safe_psql(
		'postgres',
		"SELECT boot_val, unit, context FROM pg_settings WHERE name = 'node_cache_size'"),
	'131072|kB|postmaster',
	'node_cache_size is a memory setting of 128MB by default, set at server start');
$node->append_conf('postgresql.conf', 'node_cache_size = 2MB');
$node->restart;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# A cache of 2MB holds 102 frames of 8kB, 51 of 16kB, 7 of 32kB, and 1 of
# 64kB.
cmp_ok(
	$node->safe_psql(
		'postgres',
		"SELECT size FROM pg_shmem_allocations WHERE name = 'versioned node cache'"),
	'>=', 102 * 8192 + 51 * 16384 + 7 * 32768 + 65536,
	'the versioned node cache is listed with its size');

# One backend installs a node and keeps its pin; another reads it, pinning the
# same frame.
my $holder = $node->background_psql('postgres');
$holder->query_safe('BEGIN');
is($holder->query_safe('SELECT doltgres_test_cache(0, 1, 7)'),
	'1', 'a node is installed');
is( $node->safe_psql(
		'postgres', 'BEGIN;
SELECT doltgres_test_cache(1, 1, 0);
SELECT doltgres_test_cache(2, 1, 0);
ROLLBACK;'),
	"7\n2",
	'two backends read one node');
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 1, 0)'),
	'1', "a reader's pin is released at its transaction's end");

# Another backend fills the cache many times over while the holder keeps its
# pin; the holder's node stays.
$node->safe_psql('postgres',
	'SELECT doltgres_test_cache(3, k, k % 200) FROM generate_series(1000, 1400) k'
);
is($holder->query_safe('SELECT doltgres_test_cache(1, 1, 0)'),
	'7', 'a backend holding a pin keeps its frame while another fills the cache');
$holder->query_safe('ROLLBACK');
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 1, 0)'),
	'0', "an aborted transaction's pins are released");

# A pin kept before a savepoint survives a rollback to it and resumes with its
# node's bytes; the rollback releases only the pins taken since, which resume
# with nothing.
is( $node->safe_psql(
		'postgres', 'BEGIN;
SELECT doltgres_test_cache(0, 300, 3);
SAVEPOINT s;
SELECT doltgres_test_cache(0, 301, 3);
SELECT doltgres_test_cache(2, 301, 0);
ROLLBACK TO s;
SELECT doltgres_test_cache(2, 301, 0);
SELECT doltgres_test_cache(8, 301, 0);
SELECT doltgres_test_cache(8, 300, 0);
SELECT doltgres_test_cache(2, 300, 0);
ROLLBACK;'),
	"1\n1\n1\n0\n-1\n3\n1",
	"a cursor's pin survives a rollback to a savepoint, and a subtransaction's abort releases only its own pins"
);
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 300, 0)'),
	'0', "the outer pin is released at its transaction's end");

# A terminated backend's pins are released as it exits.
my $doomed = $node->background_psql('postgres');
$doomed->query_safe('BEGIN');
$doomed->query_safe('SELECT doltgres_test_cache(0, 400, 4)');
my $pid = $doomed->query_safe('SELECT pg_backend_pid()');
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 400, 0)'),
	'1', 'a backend pins a node');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
  or die 'the terminated backend never exited';
$doomed->quit;
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 400, 0)'),
	'0', "a terminated backend's pins are released");

# A backend's own cache holds its temporary nodes, which no other backend
# sees.
is( $node->safe_psql(
		'postgres', 'SELECT doltgres_test_cache(4, 0, 1024);
SELECT doltgres_test_cache(5, 500, 9);
SELECT doltgres_test_cache(6, 500, 0);'),
	"0\n1\n9",
	"a temporary node is cached in the backend's own cache");
is( $node->safe_psql(
		'postgres', 'SELECT doltgres_test_cache(4, 0, 1024);
SELECT doltgres_test_cache(6, 500, 0);'),
	"0\n-1",
	"another backend's own cache doesn't hold it");

# A pin is remembered by the current resource owner, so with none it is
# refused.
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_cache(7, 1, 0)");
like(
	$stderr,
	qr/ERROR:  XX000: no resource owner for the fork's versioned state/,
	'a pin with no resource owner is refused');
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 1, 0)'),
	'0', 'a refused pin pins nothing');

# Two installs of one key interleave: the first claims a victim and pauses
# before its partition locks, the second installs the key, and the first then
# finds the key's frame and pins it, so both read the second's bytes.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-cache-claimed', 'wait')");
my $first = $node->background_psql('postgres');
$first->query_until(qr/start/,
	"\\echo start\nBEGIN;\nSELECT doltgres_test_cache(0, 600, 6);\n");
$node->wait_for_event('client backend', 'doltgres-cache-claimed');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-cache-claimed')");
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(3, 600, 9)'),
	'1', 'a second install of a key runs while the first is paused');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-cache-claimed')");
ok( pump_until(
		$first->{run}, $first->{timeout}, \$first->{stdout}, qr/^1$/m),
	"the first install pins the second's frame");
$first->{stdout} = '';
is($first->query_safe('SELECT doltgres_test_cache(8, 600, 0)'),
	'9', 'two installs of one key end with one frame');
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 600, 0)'),
	'1', "the first install's victim has no pin left and the key's frame has one");
$first->query_safe('ROLLBACK');
$first->quit;

# The one 64kB frame is claimed by an installer paused before its partition
# locks; a second installer's sweep reaches it and doesn't take it.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-cache-claimed', 'wait')");
my $claimer = $node->background_psql('postgres');
$claimer->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_cache(9, 700, 7);\n");
$node->wait_for_event('client backend', 'doltgres-cache-claimed');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-cache-claimed')");
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(9, 701, 8)'),
	'0', 'two installers reaching one victim claim it once');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-cache-claimed')");
ok( pump_until(
		$claimer->{run}, $claimer->{timeout},
		\$claimer->{stdout}, qr/^1$/m),
	'the claimant installs its node in the frame');
$claimer->{stdout} = '';
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_cache(2, 700, 0), doltgres_test_cache(2, 701, 0)'),
	'0|-1',
	"the frame holds the claimant's key, and the other installer's key is in no frame");

# An installer claims that frame, which holds a node, and pauses before its
# partition locks; a reader pins the old key meanwhile, so the installer's
# recheck gives the frame up, and the reader's bytes stay.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-cache-claimed', 'wait')");
$claimer->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_cache(9, 702, 8);\n");
$node->wait_for_event('client backend', 'doltgres-cache-claimed');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-cache-claimed')");
my $reader = $node->background_psql('postgres');
$reader->query_safe('BEGIN');
is($reader->query_safe('SELECT doltgres_test_cache(1, 700, 0)'),
	'7', 'a reader pins the old key of a claimed frame');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-cache-claimed')");
ok( pump_until(
		$claimer->{run}, $claimer->{timeout},
		\$claimer->{stdout}, qr/^0$/m),
	'an installer gives up a victim a reader pinned after its claim');
$claimer->quit;
is($reader->query_safe('SELECT doltgres_test_cache(8, 700, 0)'),
	'7',
	'a reader that pins the old key between the claim and the partition locks keeps its bytes'
);
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_cache(2, 700, 0), doltgres_test_cache(2, 702, 0)'),
	'1|-1',
	"the frame keeps the reader's pin alone, and the installer's key is in no frame");
$reader->query_safe('ROLLBACK');
$reader->quit;

# An installer pauses after naming its frame and before filling it; a reader
# of the key gets a miss without waiting or counting as a use, and another
# install of the key reads uncached.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-cache-retagged', 'wait')");
my $filler = $node->background_psql('postgres');
$filler->query_until(qr/start/,
	"\\echo start\nSELECT doltgres_test_cache(3, 800, 5);\n");
$node->wait_for_event('client backend', 'doltgres-cache-retagged');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-cache-retagged')");
is( $node->safe_psql(
		'postgres', 'SELECT doltgres_test_cache(10, 800, 0);
BEGIN;
SELECT doltgres_test_cache(1, 800, 0);
ROLLBACK;
SELECT doltgres_test_cache(10, 800, 0);
SELECT doltgres_test_cache(2, 800, 0);
SELECT doltgres_test_cache(13, 800, 0);'),
	"1\n-1\n1\n1\n1",
	'a reader of a frame being filled gets a miss');
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(3, 800, 6)'),
	'0', 'an install of a key whose frame is being filled reads uncached');
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('doltgres-cache-retagged')");
ok( pump_until(
		$filler->{run}, $filler->{timeout}, \$filler->{stdout}, qr/^1$/m),
	'the installer fills its frame');
$filler->quit;
is( $node->safe_psql(
		'postgres', 'BEGIN;
SELECT doltgres_test_cache(1, 800, 0);
ROLLBACK;'),
	'5',
	'a filled frame is a hit');

# With every allocation failing, the pin that needs the pin table to grow is
# refused, and the pins taken before it are released at the abort.
($ret, $stdout, $stderr) = $node->psql(
	'postgres', "\\set VERBOSITY verbose
BEGIN;
SELECT doltgres_test_cache(0, 900, 9);
SELECT doltgres_test_cache(11, 900, 0);");
like(
	$stderr,
	qr/ERROR:  53200: out of memory/,
	'a failed allocation growing the pin table is refused');
is($node->safe_psql('postgres', 'SELECT doltgres_test_cache(2, 900, 0)'),
	'0', "a refused pin leaves every pin to its owner's release");

# The host unpins by a frame's number; one past the cache's 161 frames, and
# the largest there is, change nothing.
$holder->query_safe('BEGIN');
$holder->query_safe('SELECT doltgres_test_cache(0, 1000, 1)');
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_cache(12, 161, 0) + doltgres_test_cache(12, 4294967295, 0) + doltgres_test_cache(2, 1000, 0)'
	),
	'1',
	'a number past the frame count changes nothing');
$holder->query_safe('ROLLBACK');

# A pin dropped before its transaction commits is forgotten by its resource
# owner, which warns of what it still remembers at a commit.
($ret, $stdout, $stderr) =
  $node->psql('postgres', 'SELECT doltgres_test_cache(3, 1100, 1)');
is($stderr, '', 'a dropped pin is forgotten by its resource owner');

# An install that stops before its fill leaves its frame named by its key with
# no pin, a miss for a reader; the key's next installs fill it once the clock
# hands it to one of them, within two passes of the 102 frames of its size.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-cache-retagged', 'error')");
($ret, $stdout, $stderr) =
  $node->psql('postgres', 'SELECT doltgres_test_cache(3, 1200, 5)');
like(
	$stderr,
	qr/error triggered for injection point doltgres-cache-retagged/,
	'an install stops before its fill');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-cache-retagged')");
is( $node->safe_psql(
		'postgres', 'BEGIN;
SELECT doltgres_test_cache(2, 1200, 0);
SELECT doltgres_test_cache(1, 1200, 0);
ROLLBACK;'),
	"0\n-1",
	'a frame a failed fill left has no pin and is a miss');
is( $node->safe_psql(
		'postgres',
		'SELECT max(doltgres_test_cache(3, 1200, 5)) FROM generate_series(1, 204);
BEGIN;
SELECT doltgres_test_cache(1, 1200, 0);
ROLLBACK;'),
	"1\n5",
	"a frame a failed fill left is filled by its key's next installs");

# One resource owner remembers more pins than the 32 its array holds.
is( $node->safe_psql(
		'postgres', 'BEGIN;
SELECT count(doltgres_test_cache(1, 1200, 0)) FROM generate_series(1, 40);
SELECT doltgres_test_cache(2, 1200, 0);
ROLLBACK;
SELECT doltgres_test_cache(2, 1200, 0);'),
	"40\n1\n0",
	'a resource owner remembers more pins than its array holds');

# The cache says whether it holds a key without pinning its frame or counting
# a use: the one 64kB frame, asked about ten times after its install, still
# has the usage count of its install and is taken by the next key's install,
# after which the cache holds that key and not the first.
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_cache(13, 1300, 0);
SELECT max(doltgres_test_cache(9, 1300, 3)) FROM generate_series(1, 4);
SELECT sum(doltgres_test_cache(13, 1300, 0)) FROM generate_series(1, 10);
SELECT doltgres_test_cache(10, 1300, 0), doltgres_test_cache(2, 1300, 0);
SELECT doltgres_test_cache(9, 1301, 3);
SELECT doltgres_test_cache(13, 1300, 0), doltgres_test_cache(13, 1301, 0);'),
	"0\n1\n10\n1|0\n1\n0|1",
	'a key is asked about without a pin or a use');

# Two keys that differ only in their last byte are two nodes, in the shared
# cache and in a backend's own: each is read back with the byte it was
# installed with, where a table that compared fewer bytes than a key holds
# would give the first node for the second.
is( $node->safe_psql(
		'postgres',
		'SELECT doltgres_test_cache(3, 1500, 4), doltgres_test_cache(3, 1501, 5);
BEGIN;
SELECT doltgres_test_cache(1, 1500, 0), doltgres_test_cache(1, 1501, 0);
ROLLBACK;
SELECT doltgres_test_cache(4, 0, 1024);
SELECT doltgres_test_cache(5, 1500, 6), doltgres_test_cache(5, 1501, 7);
SELECT doltgres_test_cache(6, 1500, 0), doltgres_test_cache(6, 1501, 0);'),
	"1|1\n4|5\n0\n1|1\n6|7",
	'two keys that differ only in their last byte are two nodes');

$holder->quit;
$node->stop;
done_testing();
