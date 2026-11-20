#!/usr/bin/perl
#
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
# Compares the symbols the fork's postgres exports with those of a stock
# Postgres 19 postgres built with the same options (design item 17): every
# stock export must be present, and the only additions the fork's entry thunks
# and the variables hooks.h declares, whose names start with doltgres_ or, for a
# variable in Postgres' own style, Doltgres. Extensions built for stock Postgres
# resolve against these exports.
#
#   exports.pl <fork postgres> <stock postgres>

use strict;
use warnings FATAL => 'all';

my ($fork, $stock) = @ARGV;
die "usage: exports.pl <fork postgres> <stock postgres>\n"
  unless defined $stock;

my %fork = map { $_ => 1 } exports($fork);
my %stock = map { $_ => 1 } exports($stock);
my @missing = sort grep { !$fork{$_} } keys %stock;
my @added = sort grep { !$stock{$_} && !/^_?(?:doltgres_|Doltgres[A-Z])/ } keys %fork;

print "missing from the fork: $_\n" for @missing;
print "added by the fork: $_\n" for @added;
printf "%d stock exports, %d fork exports, %d missing, %d unexpected\n",
  scalar(keys %stock), scalar(keys %fork), scalar(@missing), scalar(@added);
exit(@missing || @added ? 1 : 0);

# exports returns the names of the symbols a binary exports.
sub exports
{
	my ($binary) = @_;
	my @lines;
	if ($^O eq 'MSWin32')
	{
		@lines = `dumpbin /nologo /exports "$binary"`;
		die "exports.pl: dumpbin failed on $binary\n" if $?;
		return map { /^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)/ ? $1 : () }
		  @lines;
	}
	my $flags = $^O eq 'darwin' ? '-gU' : '-D --defined-only';
	@lines = `nm $flags "$binary"`;
	die "exports.pl: nm failed on $binary\n" if $?;
	return map { /^\S*\s+[A-Za-z]\s+(\S+)$/ ? $1 : () } @lines;
}
