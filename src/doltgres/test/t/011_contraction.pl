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

# Floating-point contraction (design item 15): each floating-point operation
# rounds on its own, so a multiply and an add give the same result on a
# machine that can fuse them into one rounding as on one that can't,
# interpreted and compiled by the JIT alike. The expected results are x86-64's,
# which fuses nothing by default; fused, the first interval would end in
# 00:00:00.007674, the second in 16:47:59.998105, and the variance would be
# 1.21. A compiled case first reads from EXPLAIN that the JIT compiled the
# query's expressions, which then give the interpreted results; inlining is
# asked for and finds nothing of the server's, whose build makes no bitcode.
# The server says it has a JIT only to a session with jit on, and EXPLAIN
# prints the JIT's section only with costs. An
# extension built with PGXS gets the same: the makefile it builds from carries
# the flag for its bitcode wherever it does for its compiled code, and where
# the build has clang the bitcode of a function that multiplies and adds holds
# no fused operation. Results can't show the flag where nothing fuses, so the
# build's own list of compile commands is read: with gcc or clang every C and
# C++ command carries it, the vendored libraries' included, and with MSVC,
# which rounds each operation unless told otherwise, none asks for fast
# floating point.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use JSON::PP;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('contraction');
$node->init;
$node->start;

# The operands come from rows the executor reads, since the planner evaluates
# an expression of constants itself, through the interpreter.
my @cases = (
	[
		"SELECT i * f FROM (VALUES (interval '1000000000 days', 1.1::float8), (interval '123456789 days', 3.3::float8)) v(i, f)",
		"1100000000 days\n407407403 days 16:47:59.99897"
	],
	[
		'SELECT var_samp(x) FROM (VALUES (1.1::float8), (2.2), (3.3)) v(x)',
		'1.2099999999999995'
	]);
my $compiled =
  'SET jit = on; SET jit_above_cost = 0; SET jit_inline_above_cost = 0; SET jit_optimize_above_cost = 0; ';
my $jit =
  $node->safe_psql('postgres', 'SET jit = on; SELECT pg_jit_available()') eq
  't';

for my $case (@cases)
{
	my ($query, $expected) = @$case;
	is($node->safe_psql('postgres', $query),
		$expected, "$query rounds each operation");
  SKIP:
	{
		skip 'this build has no JIT', 2 unless $jit;
		like(
			$node->safe_psql(
				'postgres',
				$compiled
				  . "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF) $query"),
			qr/Functions: [1-9]\d*\n\s*Options: Inlining true/,
			"the JIT compiles $query");
		is($node->safe_psql('postgres', $compiled . $query),
			$expected,
			"$query gives the interpreted result when compiled by the JIT");
	}
}

$node->stop;

my ($pgxs) = run_command([ 'pg_config', '--pgxs' ]);
my $global = slurp_file(dirname(dirname($pgxs)) . '/Makefile.global');
my %set = $global =~ /^(\w+) = (.*)$/mg;
for my $flags ('CFLAGS', 'CXXFLAGS')
{
	is( $set{"BITCODE_$flags"} =~ /-ffp-contract=off/ ? 1 : 0,
		$set{$flags} =~ /-ffp-contract=off/           ? 1 : 0,
		"an extension's BITCODE_$flags carries the flag exactly when its $flags does"
	);
}

# bitcode builds the extension's bitcode with PGXS's own command and returns
# its bytes.
sub bitcode
{
	my ($dir, @override) = @_;
	unlink "$dir/contracted.bc";
	command_ok([ 'make', '-C', $dir, 'contracted.bc', @override ],
		"PGXS builds an extension's bitcode");
	return slurp_file("$dir/contracted.bc");
}

SKIP:
{
	skip 'this build has no clang to make bitcode with', 4
	  unless $set{CLANG} =~ /\S/;
	my $dir = PostgreSQL::Test::Utils::tempdir;
	append_to_file("$dir/contracted.c",
		"double contracted(double a, double b, double c) { return a * b + c; }\n"
	);
	append_to_file("$dir/Makefile",
			"PGXS := \$(shell pg_config --pgxs)\n"
		  . "include \$(PGXS)\n"
		  . "contracted.bc: contracted.c\n"
		  . "\t\$(COMPILE.c.bc) -o \$@ \$<\n");
	unlike(bitcode($dir), qr/fmuladd/,
		"an extension's bitcode holds no fused multiply-add");
	like(
		bitcode($dir, 'BITCODE_CFLAGS=-ffp-contract=on'),
		qr/fmuladd/,
		'the same bitcode built with contraction on holds one, so the check sees them'
	);
}

my $commands =
  decode_json(slurp_file("$ENV{top_builddir}/compile_commands.json"));
my @compiles = grep { $_->{file} =~ /\.(c|cc|cpp)$/ } @$commands;
if ($set{GCC} eq 'yes')
{
	is( join("\n",
			map  { $_->{file} }
			grep { $_->{command} !~ /-ffp-contract=off/ } @compiles),
		'',
		"every C and C++ command of the build carries the flag");
}
else
{
	is( join("\n",
			map  { $_->{file} }
			grep { $_->{command} =~ m{[-/]fp:fast} } @compiles),
		'',
		"no C or C++ command of the build asks for fast floating point");
}

done_testing();
