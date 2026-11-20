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

# An insert's cost against the size of its storage's delta (bugs row 83): the
# search for a block with room starts at its category's hint, as heap's free
# space map searches by category, so an insert's cost stays the same as the
# delta grows, where a walk of the delta's free space map from its first block
# made it grow with every page the delta held.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

my $node = PostgreSQL::Test::Cluster->new('cost');
$node->init;
$node->append_conf('postgresql.conf', 'delta_memory = 1GB');
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test; CALL doltgres_test_twins();
CREATE TABLE source_small (a int4, v text);
INSERT INTO source_small SELECT a, repeat('x', 980) FROM generate_series(1, 10000) a;
CREATE TABLE source_large (a int4, v text);
INSERT INTO source_large SELECT a, repeat('x', 980) FROM generate_series(10001, 110000) a;
CREATE TABLE costed (a int4, v text);
});

# inserted inserts a source table's rows into the costed table, one row at a
# time through the fork's routine, and returns the seconds each row took.
sub inserted
{
	my ($source, $rows) = @_;
	my $start = time();
	$node->safe_psql('postgres',
		"SELECT count(*) FROM doltgres_test_insert('costed', '$source', false, 0)"
	);
	return (time() - $start) / $rows;
}

# Bugs row 83's case: 10,000 rows of about 1 kB, then 100,000 more into the
# same storage; each of the second's rows costs within a small factor of the
# first's.
my $first = inserted('source_small', 10000);
my $second = inserted('source_large', 100000);
cmp_ok($second, '<', 3 * $first,
	"an insert into a delta ten times larger costs about the same (row 83): $first s, then $second s a row"
);

$node->stop;
done_testing();
