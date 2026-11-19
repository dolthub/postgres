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

# The C port's compile-fail cases (the port text's section 7): each crate's
# c/fail/expected.tsv names a case, a compiler family, and the diagnostic that
# family gives, and the case's snippet, c/fail/<case>.c, passes only when the
# build's own compiler, with the build's flags, refuses it with that
# diagnostic, as each Rust fail crate's case is refused with its error.
#
# Each crate's c/fail/opaque.tsv names its opaque-storage types, a public
# struct of fixed size whose layout one file alone knows, with that file and,
# for a type whose value a door moves out, the clone the C must not have. C
# can't refuse a value of a complete type made from bytes, so no other file of
# the fork's writes one with an initializer list or a compound literal, casts
# another pointer to its pointer, or copies or sets bytes into one; a
# whole-value assignment moves a value already made, and stays allowed but
# for a type whose value a door moves out, where a copy would leave two
# storages holding one value, so no other file copies such a value into a
# variable or through a pointer from anything but a call's result.
#
# verify/tokens.tsv accounts for each Rust compile-fail case of a crate with a
# C port: one row each, naming the C form that keeps its token and the C case
# that shows it, a subtest of this test or a C or Rust test case, or the form
# outstanding for a case whose C form is still owed, which a case with a C
# form of its own can't keep. The port lands only with no row outstanding,
# which the environment's doltgres_tokens_landing makes this test require.

use strict;
use warnings FATAL => 'all';
use File::Basename qw(dirname);
use File::Find;
use IPC::Run qw(run);
use PostgreSQL::Test::Utils;
use Test::More;

my @cc = split /\n/, $ENV{doltgres_cc};
my $family = $ENV{doltgres_cc_family};
my $root = dirname($ENV{doltgres_pg_dir});
my $syntax = $family eq 'msvc' ? '/Zs' : '-fsyntax-only';
my $cases = 0;
my %named;

# named records a subtest's name and returns it.
sub named
{
	my ($name) = @_;
	$named{$name} = 1;
	return $name;
}

foreach my $expected (sort glob("$root/*/c/fail/expected.tsv"))
{
	my $dir = dirname($expected);
	open my $fh, '<', $expected or die "cannot read $expected: $!";
	while (my $line = <$fh>)
	{
		$line =~ s/\r?\n$//;
		next if $line eq '' || $line =~ /^#/;
		my ($case, $compiler, $diagnostic) = split /\t/, $line, 3;

		# A case another family's build runs is a check that exists.
		$named{"$case is refused"} = 1;
		$named{"$case is refused with its diagnostic"} = 1;
		next unless $compiler eq $family;
		my ($out, $err) = ('', '');
		my $compiled =
		  run([ @cc, $syntax, "$dir/$case.c" ], '>', \$out, '2>', \$err);
		ok(!$compiled, named("$case is refused"));
		like($out . $err,
			qr/\Q$diagnostic\E/,
			named("$case is refused with its diagnostic"));
		$cases++;
	}
	close $fh;
}
ok($cases > 0, "the $family compile-fail cases run");

# opaque_writes returns the lines of a file's text that make a value of an
# opaque-storage type from bytes: an initializer list, a compound literal, a
# cast of another pointer to its pointer, or a copy or set of bytes into it.
sub opaque_writes
{
	my ($text, $type) = @_;
	my %values = map { $_ => 1 } $text =~ /\b\Q$type\E\s+(\w+)/g;
	my %pointers = map { $_ => 1 } $text =~ /\b\Q$type\E\s*\*\s*(\w+)/g;
	my @found;
	foreach my $line (split /\n/, $text)
	{
		my $written =
			 $line =~ /\b\Q$type\E\s+\w+\s*(?:\[[^\]]*\]\s*)?=\s*\{/
		  || $line =~ /\(\s*(?:const\s+)?\Q$type\E\s*\)\s*\{/
		  || $line =~ /\(\s*(?:const\s+)?\Q$type\E\s*\*\s*\)/;
		foreach my $name (keys %values)
		{
			$written ||=
			  $line =~
			  /\bmem(?:cpy|move|set)\s*\(\s*(?:\(\s*void\s*\*\s*\)\s*)?&\s*(?:[\w.>-]*(?:->|\.))?\Q$name\E\s*,/;
		}
		foreach my $name (keys %pointers)
		{
			$written ||=
			  $line =~
			  /\bmem(?:cpy|move|set)\s*\(\s*(?:\(\s*void\s*\*\s*\)\s*)?(?:[\w.>-]*(?:->|\.))?\Q$name\E\s*,/;
		}
		push @found, $line if $written;
	}
	return @found;
}

my @sample = opaque_writes(
	join("\n",
		'ChunksPackRaw raw = {0};',
		'memcpy(&raw, bytes, sizeof(raw));',
		'raw = other;',
		'ChunksPackRaw *out = NULL;',
		'*out = raw;',
		'memset(out, 0, sizeof(*out));',
		'ChunksPackRaw kept;',
		'use(&kept);',
		'out = &kept;',
		'out = (ChunksPackRaw *) bytes;',
		'kept = (ChunksPackRaw) {0};'),
	'ChunksPackRaw');
is(scalar(@sample), 5,
	'the check finds an opaque value made by an initializer, a compound literal, a cast, or a copy of bytes, and lets an assignment move one'
);

# opaque_copies returns the statements of a file's text that copy a value of
# a type a door moves out into a variable or through a pointer from anything
# but a call's result, and, in a C file, each name it also declares a
# variable of another type by, whose writes it can't tell apart.
sub opaque_copies
{
	my ($text, $type, $code) = @_;
	my %values = map { $_ => 1 } $text =~ /\b\Q$type\E\s+(\w+)/g;
	my %pointers = map { $_ => 1 } $text =~ /\b\Q$type\E\s*\*\s*(\w+)/g;
	my $called = qr/(?!=)\s*+(?![\w.>-]+\s*\()/;
	my @found;
	foreach my $name ($code ? (sort keys %values, keys %pointers) : ())
	{
		while ($text =~
			/\b([A-Z]\w+|\w+_t|bool|char|int|size_t|u?int(?:8|16|32|64))\s+\**\s*\Q$name\E\s*(?:\[[^\]]*\]\s*)?[=;]/g
		  )
		{
			if ($1 ne $type)
			{
				push @found, "$name is also a $1's name";
				last;
			}
		}
	}
	foreach my $statement (split /;/, $text)
	{
		(my $flat = $statement) =~ s/\s+/ /g;
		my $copied = $flat =~ /\b\Q$type\E\s+\w+\s*=$called/;
		foreach my $name (keys %values)
		{
			$copied ||= $flat =~ /(?:^|[^\w.>*])\s*\Q$name\E\s*=$called/;
		}
		foreach my $name (keys %pointers)
		{
			$copied ||= $flat =~ /\*\s*\Q$name\E\s*=$called/
			  && $flat !~ /\b\Q$type\E\s*\*\s*\Q$name\E\s*=/;
		}
		$flat =~ s/^ //;
		push @found, $flat if $copied;
	}
	return @found;
}

my @copies = opaque_copies(
	join("\n",
		'ChunksFileTornTail a = chunks_file_tail(p, n, w);',
		'ChunksFileTornTail b = a;',
		'b = a;',
		'ChunksFileTornTail *p = &a;',
		'b = *p;',
		'b = s->t;',
		'*p = b;',
		'b = chunks_file_TornTail_of_generation(&a, 1);',
		"ChunksFileTornTail c =\n\tchunks_file_tail(p, n, w);"),
	'ChunksFileTornTail');
is(scalar(@copies), 5,
	'the check finds a moved-out value copied, and lets one be taken from a call'
);

my (%owners, %clones);
foreach my $listed (sort glob("$root/*/c/fail/opaque.tsv"))
{
	open my $fh, '<', $listed or die "cannot read $listed: $!";
	while (my $line = <$fh>)
	{
		$line =~ s/\r?\n$//;
		next if $line eq '' || $line =~ /^#/;
		my ($type, $file, $clone) = split /\t/, $line, 3;
		$owners{$type} = "$root/$file";
		$clones{$type} = $clone if $clone ne '-';
	}
	close $fh;
}
my @files;
find(
	sub {
		$File::Find::prune = 1 if $_ eq 'doltgrust' || $_ eq 'target';
		push @files, $File::Find::name if /\.[ch]$/;
	},
	$root,
	"$root/../include/doltgres");
foreach my $type (sort keys %owners)
{
	my @written;
	foreach my $file (@files)
	{
		next if $file eq $owners{$type};
		open my $fh, '<', $file or die "cannot read $file: $!";
		local $/;
		my $text = <$fh>;
		close $fh;
		push @written, map { "$file: $_" } opaque_writes($text, $type);
	}
	is(join("\n", @written),
		'', named("no file but its owner writes a ${type}'s opaque storage"));
}
foreach my $type (sort keys %clones)
{
	my @cloned;
	foreach my $file (@files)
	{
		open my $fh, '<', $file or die "cannot read $file: $!";
		local $/;
		my $text = <$fh>;
		close $fh;
		push @cloned, $file if $text =~ /\b\Q$clones{$type}\E\b/;
	}
	is(join("\n", @cloned), '', named("no C clone of a ${type} exists"));

	my @copied;
	foreach my $file (@files)
	{
		next if $file eq $owners{$type};
		open my $fh, '<', $file or die "cannot read $file: $!";
		local $/;
		my $text = <$fh>;
		close $fh;
		push @copied,
		  map { "$file: $_" } opaque_copies($text, $type, $file =~ /\.c$/);
	}
	is(join("\n", @copied),
		'', named("no file but its owner copies a ${type}"));
}

# sources returns the text of each file under a directory whose name matches
# a pattern.
sub sources
{
	my ($dir, $pattern) = @_;
	my @texts;
	find(
		sub {
			$File::Find::prune = 1 if $_ eq 'doltgrust' || $_ eq 'target';
			return unless /$pattern/ && -f $_;
			open my $fh, '<', $_ or die "cannot read $File::Find::name: $!";
			local $/;
			push @texts, scalar <$fh>;
			close $fh;
		},
		$dir);
	return @texts;
}

my $c_text = join "\n", sources($root, qr/\.c$/);
my $rust_text = join "\n", sources($root, qr/\.rs$/);
my %forms =
  map { $_ => 1 } qw(opaque opaque-storage shape capability move-out);
my $port_map = join "\n", sources("$root/verify", qr/^port-map\.tsv$/);
my (%rows, %outstanding, @wrong);
open my $tokens, '<', "$root/verify/tokens.tsv"
  or die "cannot read $root/verify/tokens.tsv: $!";
while (my $line = <$tokens>)
{
	$line =~ s/\r?\n$//;
	next if $line eq '' || $line =~ /^#/;
	my ($crate, $rust_case, $item, $c_form, $keeps, $check) =
	  split /\t/, $line, -1;
	$rows{$rust_case}++;
	push @wrong, "$rust_case: no Rust case"
	  unless -f "$root/$rust_case/src/lib.rs";
	push @wrong, "$rust_case: not $crate\'s"
	  unless $rust_case =~ m{^\Q$crate\E/};
	if ($c_form eq 'outstanding')
	{
		my $case = (split m{/}, $rust_case)[-1];
		push @{ $outstanding{$crate} }, $rust_case;
		push @wrong, "$rust_case: outstanding with a form's parts"
		  unless $item eq '-' && $keeps eq '-' && $check eq '-';
		push @wrong, "$rust_case: outstanding with a C form"
		  if -f "$root/$crate/c/fail/$case.c"
		  || $port_map =~ /^\Q$case\E::case\t[^\t]*\t(?!-\t)/m;
		next;
	}
	push @wrong, "$rust_case: form $c_form" unless $forms{$c_form};
	push @wrong, "$rust_case: no keeps" unless $keeps ne '';
	push @wrong, "$rust_case: no check" unless $check ne '';

	foreach my $named (split /; /, $check)
	{
		my $found =
			$named =~ /^t\/117: (.*)$/
		  ? $named{$1}
		  : $named =~ /^c: (.*)$/ ? do
		{
			(my $function = "test_$1") =~ s/::/_/g;
			$c_text =~ /\b\Q$function\E\b/;
		  }
		  : $named =~ /^rust: .*::(\w+)$/ ? $rust_text =~ /\bfn \Q$1\E\b/
		  :                                 0;
		push @wrong, "$rust_case: no check $named" unless $found;
	}
}
close $tokens;
is(join("\n", @wrong),
	'',
	'every row of tokens.tsv names its case, form, and an existing check');

foreach my $crate_dir (sort grep { -d "$_/c" } glob("$root/*"))
{
	my $crate = substr($crate_dir, length($root) + 1);
	my @cases =
	  map { substr(dirname(dirname($_)), length($root) + 1) }
	  glob("$crate_dir/fail/*/src/lib.rs"),
	  glob("$crate_dir/*/fail/*/src/lib.rs");
	my @unaccounted = grep { ($rows{$_} // 0) != 1 } sort @cases;
	is( join("\n", @unaccounted),
		'',
		"each of the $crate crate's Rust compile-fail cases has one row of tokens.tsv"
	);
}

my $owed = 0;
$owed += scalar @{ $outstanding{$_} } foreach keys %outstanding;
my $counts = join ', ',
  map { "$_ " . scalar @{ $outstanding{$_} } } sort keys %outstanding;
if ($ENV{doltgres_tokens_landing})
{
	is($owed, 0, "tokens.tsv has no row outstanding ($counts)");
}
else
{
	pass("tokens.tsv has $owed rows outstanding"
		  . ($owed ? " ($counts)" : ''));
}

done_testing();
