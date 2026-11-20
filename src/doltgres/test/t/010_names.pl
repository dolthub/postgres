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

# The cases of names.pl (design item 15), the build's check of a vendored
# library's names header against the library's symbols: it passes a names
# header that prefixes exactly the names its library defines, with the
# assembler section's names all there or all missing, each architecture
# section's likewise, and with a sanitizer's
# or a profiler's own names beside them, and refuses a name defined without
# the prefix, a sanitizer's indicator of one, a prefixed name the library
# doesn't define, half of a section's names, a defined name the header
# doesn't prefix, and a reference to a name that starts as the library's own
# do.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use PostgreSQL::Test::Utils;
use Test::More;

my $script = dirname(__FILE__) . '/../../names.pl';
my $dir = PostgreSQL::Test::Utils::tempdir;

my $names = <<'EOF';
/* A comment, whose lines aren't the header's:
#define zl_hidden dg_zl_hidden
*/
#ifdef __ASSEMBLER__
#define zl_loop dg_zl_loop
#define _zl_loop _dg_zl_loop
#define zl_tail dg_zl_tail
#define _zl_tail _dg_zl_tail
#else
#define zl_early dg_zl_early
#pragma redefine_extname zl_loop dg_zl_loop
#pragma redefine_extname zl_one dg_zl_one
#pragma redefine_extname zl_tail dg_zl_tail
#endif
EOF
my $objects = <<'EOF';

libzl.a(one.o):
0000000000000040 T dg_zl_one
0000000000000000 T _dg_zl_early
                 U memcpy
                 U _pthread_create
EOF
my $assembly = <<'EOF';

libzl.a(loop.o):
0000000000000000 T dg_zl_loop
0000000000000000 T _dg_zl_loop
0000000000000080 T dg_zl_tail
0000000000000080 T _dg_zl_tail
EOF

# check returns the command that runs names.pl over a names header and a
# listing holding the given text.
sub check
{
	my ($header, $listing) = @_;
	my %text = ("$dir/names.h" => $header, "$dir/listing" => $listing);
	for my $path (keys %text)
	{
		open my $fh, '>', $path or die "could not write $path: $!";
		print $fh $text{$path};
		close $fh;
	}
	return [ $^X, $script, "$dir/names.h", "$dir/listing" ];
}

command_ok(check($names, $objects . $assembly),
	'a names header that prefixes what its library defines passes');
command_ok(check($names, $objects),
	"a library whose machine has no assembly passes");
command_ok(
	check(
		$names,
		$objects
		  . "0000000000000000 B __odr_asan.dg_zl_one\n"
		  . "0000000000000000 B __odr_asan_gen_dg_zl_one\n"
		  . "0000000000000000 S ___odr_asan_gen_\x01dg_zl_one\n"
		  . "0000000000000008 C ___asan_globals_registered\n"
		  . "0000000000000000 R __llvm_profile_raw_version\n"),
	"an instrumented library's own names pass");
command_fails_like(
	check($names, $objects . "0000000000000000 B __odr_asan.zl_one\n"),
	qr/the library defines zl_one without the prefix dg_/,
	"a sanitizer's indicator of a name without the prefix fails");
command_fails_like(
	check($names, $objects . $assembly =~ s/^.*zl_tail\n//gmr),
	qr/names\.h prefixes zl_tail, which the library doesn't define/,
	"half of the assembler section's names fails");
command_fails_like(
	check($names, $objects =~ s/dg_zl_one/zl_one/r),
	qr/the library defines zl_one without the prefix dg_/,
	'a name defined without the prefix fails');
command_fails_like(
	check($names . "#define ZL_THREADS dg_ZL_THREADS\n", $objects),
	qr/names\.h prefixes ZL_THREADS, which the library doesn't define/,
	"a prefixed name the library doesn't define fails");
command_fails_like(
	check($names, $objects . "0000000000000090 T dg_zl_two\n"),
	qr/names\.h doesn't prefix zl_two, which the library defines/,
	"a defined name the header doesn't prefix fails");
command_fails_like(
	check($names, $objects . "                 w zl_trace_begin\n"),
	qr/the library asks for zl_trace_begin, which starts as its own names do/,
	'a reference to a name that starts as its own do fails');

my $sections = <<'EOF';
#pragma redefine_extname zl_one dg_zl_one
#if defined(__x86_64__)
#pragma redefine_extname zl_wide dg_zl_wide
#pragma redefine_extname zl_wider dg_zl_wider
#elif defined(__aarch64__)
#pragma redefine_extname zl_neon dg_zl_neon
#endif
EOF
my $one = "\nlibzl.a(one.o):\n0000000000000040 T dg_zl_one\n";
command_ok(
	check(
		$sections,
		$one
		  . "0000000000000000 T dg_zl_wide\n"
		  . "0000000000000080 T dg_zl_wider\n"),
	"one architecture's section, there whole, and the other's missing pass");
command_ok(check($sections, $one . "0000000000000000 T dg_zl_neon\n"),
	"the other architecture's section there whole passes");
command_fails_like(
	check($sections, $one . "0000000000000000 T dg_zl_wide\n"),
	qr/names\.h prefixes zl_wider, which the library doesn't define/,
	"half of an architecture section's names fails");
command_fails_like(
	check($sections, "\nlibzl.a(one.o):\n0000000000000000 T dg_zl_wide\n"
		  . "0000000000000080 T dg_zl_wider\n"),
	qr/names\.h prefixes zl_one, which the library doesn't define/,
	"a name outside the sections missing fails");

done_testing();
