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

# The C port's access unit tests (the port text's section 13), run in a
# backend through the test module: each test is a TAP test that passes when
# its outcome is ok.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('cport_access');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_cport_test');

my $rows = $node->safe_psql('postgres',
	"SELECT name || E'\\t' || replace(outcome, E'\\n', ' ') FROM doltgres_cport_test_access()"
);
my @tests = split /\n/, $rows;
ok(@tests > 0, 'the module runs its tests');
foreach my $row (@tests)
{
	my ($name, $outcome) = split /\t/, $row, 2;
	is($outcome, 'ok', $name);
}

$node->stop;
done_testing();
