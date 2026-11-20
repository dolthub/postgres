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

# The C port's std tests (the port text's section 13), run in a backend
# through the test module: each of core's tests is a TAP test that passes
# when its outcome is ok. The runner's cases show a failing test's captured
# output after its panic and a passing test's as nothing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# The sort tests' patterns draw from a seed the runner sets and prints, so that a failing run can be repeated with
# the same OVERRIDE_SEED.
$ENV{OVERRIDE_SEED} //= int(rand(2**32));
note "OVERRIDE_SEED=$ENV{OVERRIDE_SEED}";

my $node = PostgreSQL::Test::Cluster->new('cport_std');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_cport_test');

my $rows = $node->safe_psql('postgres',
	"SELECT name || E'\\t' || replace(outcome, E'\\n', ' ') FROM doltgres_cport_test_std()"
);
my @tests = split /\n/, $rows;
ok(@tests > 0, 'the module runs its tests');
foreach my $row (@tests)
{
	my ($name, $outcome) = split /\t/, $row, 2;
	is($outcome, 'ok', $name);
}

my $failing = $node->safe_psql('postgres',
	"SELECT outcome FROM doltgres_cport_test_std_runner() WHERE name = 'prints_then_fails'"
);
like(
	$failing,
	qr/\Apanicked: failed\n---- stdout ----\nline one\nline two\n\nthread 'main' \(\d+\) panicked at [^\n]*:\nfailed\n/s,
	'a failing test shows its captured output, std\'s default hook\'s report of its panic last'
);
my $passing = $node->safe_psql('postgres',
	"SELECT outcome FROM doltgres_cport_test_std_runner() WHERE name = 'prints_then_passes'"
);
is($passing, 'ok', 'a passing test shows no captured output');

# A #[should_panic] test passes on any panic, or on one whose message holds
# the expected text, and fails, as libtest reports it, on another text or no
# panic.
my %should = map {
	$_ => $node->safe_psql('postgres',
		"SELECT outcome FROM doltgres_cport_test_std_runner() WHERE name = '$_'"
	)
  } qw(should_panic_any should_panic_expected should_panic_other should_panic_none
  should_panic_any_payload should_panic_expected_payload);
is($should{should_panic_any}, 'ok',
	'a should_panic test passes on any panic');
is($should{should_panic_expected},
	'ok', 'a should_panic test passes on a panic holding its text');
like(
	$should{should_panic_other},
	qr/\Apanic did not contain expected string\n      panic message: "boom"\n expected substring: "bang"/,
	'a should_panic test fails on a panic without its text');
like(
	$should{should_panic_none},
	qr/\Atest did not panic as expected/,
	'a should_panic test fails when it does not panic');
is($should{should_panic_any_payload},
	'ok', 'a should_panic test passes on a payload that is not a string');
like(
	$should{should_panic_expected_payload},
	qr/\Aexpected panic with string value,\n found non-string value\n     expected substring: "boom"/,
	'a should_panic test expecting a text fails on a payload that is not a string'
);

$node->stop;
done_testing();
