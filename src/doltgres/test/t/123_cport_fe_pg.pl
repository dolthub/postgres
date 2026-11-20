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

# The C port's pg frontend tests (the port text's section 7), run outside a
# backend by pg's frontend test binary: each test is a TAP test that passes
# when its outcome is ok.

use strict;
use warnings FATAL => 'all';
use IPC::Run;
use PostgreSQL::Test::Utils;
use Test::More;

my ($stdout, $stderr) = ('', '');
my $ran = IPC::Run::run([ $ENV{doltgres_cport_test_fe_pg} ],
	'>', \$stdout, '2>', \$stderr);
ok($ran, 'the binary exits 0');
is($stderr, '', 'the binary writes nothing to stderr');
my @tests = split /\n/, $stdout;
ok(@tests > 0, 'the binary runs its tests');
foreach my $row (@tests)
{
	my ($name, $outcome) = split /\t/, $row, 2;
	$outcome =~ s/\\(.)/$1 eq 'n' ? "\n" : $1/ge;
	is($outcome, 'ok', $name);
}

done_testing();
