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
#
# Checks a vendored library's names header against the library's symbols
# (design item 15), given its names.h and the `nm -g` listing of the library
# built with it. Every external name the library defines starts dg_, so no
# shared library's reference to the library's own names can bind to the fork's
# copy. The names names.h prefixes are exactly those names without the prefix,
# so it can't rename a macro of the pinned sources or a function they call,
# which would change what they do; the names of its assembler section may all
# be missing, where the assembly source is empty for the machine, and so may
# those of a section for one architecture (#if defined(..) or #elif
# defined(..)), where its sources aren't built for the machine. And the
# library asks for no name that starts as its own do, which another library
# could answer. An instrumented build's own names are read for what they are:
# AddressSanitizer's indicator of a global (__odr_asan.<name> from gcc,
# __odr_asan_gen_<name> from clang, which puts its mark of a renamed symbol,
# the byte 1, before the name) stands for that name, and the flags clang
# defines in every object it instruments (___asan_globals_registered and
# __llvm_profile_raw_version) are its runtime's, one for the whole program.
#
#   names.pl <names.h> <listing> [<stamp>]

use strict;
use warnings FATAL => 'all';

my ($names, $listing, $stamp) = @ARGV;
die "usage: names.pl <names.h> <listing> [<stamp>]\n"
  unless defined $listing;

my (%prefixed, %section);
my $section = '';
for my $line (split /\n/, slurp($names) =~ s{/\*.*?\*/}{}gsr)
{
	$section =
	    $line =~ /^#ifdef __ASSEMBLER__/ ? 'assembler'
	  : $line =~ /^#(?:if|elif) (defined\(\w+\))/ ? $1
	  : ''
	  if $line =~ /^#(?:ifdef|if|elif|else|endif)/;
	next unless $line =~ /^#(?:define|pragma redefine_extname) _?(\w+) /;
	$prefixed{$1} = 1;
	$section{$1} = $section if $section;
}

my (@defined, @asked);
for my $line (split /\n/, slurp($listing))
{
	push @asked, $1 if $line =~ /^\s+[A-Za-z]\s+(\S+)\s*$/;
	next unless $line =~ /^[0-9a-fA-F]+\s+[A-Za-z]\s+(\S+)\s*$/;
	my $name = $1 =~ s/^_?__odr_asan(?:\.|_gen_)\x01?//r;
	push @defined, $name
	  unless $name =~
	  /^_?(?:___asan_globals_registered|__llvm_profile_raw_version)$/;
}

my @problems =
  map { "the library defines $_ without the prefix dg_" }
  grep { !/^_?dg_/ } @defined;
my %have = map { s/^_?dg_//r => 1 } grep { /^_?dg_/ } @defined;
push @problems, map { "names.h doesn't prefix $_, which the library defines" }
  grep { !$prefixed{$_} } sort keys %have;
my %built = map { $have{$_} ? ($section{$_} => 1) : () } keys %section;
push @problems,
  map { "names.h prefixes $_, which the library doesn't define" }
  grep { !$have{$_} && (!$section{$_} || $built{ $section{$_} }) }
  sort keys %prefixed;
my %own = map { /^([A-Za-z0-9]+)_/ ? ($1 => 1) : () } keys %have;
push @problems,
  map { "the library asks for $_, which starts as its own names do" }
  grep { /^_?([A-Za-z0-9]+)_/ && $own{$1} } @asked;

print STDERR "names.pl: $names: $_\n" for @problems;
exit 1 if @problems;
if (defined $stamp)
{
	open my $fh, '>', $stamp or die "names.pl: could not write $stamp: $!\n";
	close $fh;
}

# slurp returns a file's text.
sub slurp
{
	my ($path) = @_;
	open my $fh, '<', $path or die "names.pl: could not open $path: $!\n";
	local $/;
	my $text = <$fh>;
	close $fh;
	return $text;
}
