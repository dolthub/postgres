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

# A sequential read of a folded table decodes a leaf's stamps once, when the
# node cache brings the leaf in, and never again for each row it reads, so
# the read decodes fewer trailers than it reads rows (bugs row 477).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 5000;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
$node->safe_psql('postgres',
	"CREATE EXTENSION doltgres_test; CREATE TABLE flat (k int4 PRIMARY KEY, v int4); INSERT INTO flat SELECT g, g FROM generate_series(1, $rows) g"
);
$node->safe_psql('postgres', "SELECT doltgres_test_fold('flat')");

my ($before, $read, $after) = split /\n/,
  $node->safe_psql('postgres',
	"SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SELECT doltgres_test_stamps_decoded(); SELECT count(*) FROM flat; SELECT doltgres_test_stamps_decoded()"
  );
note("the read of $rows rows decoded ", $after - $before, ' trailers');
is($read, $rows, 'the sequential read reads every row of the folded table');
cmp_ok($after - $before,
	'<', $rows,
	'the read decodes fewer trailers than it reads rows (row 477)');

$node->stop;

done_testing();
