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
# meson's way into cargo (design item 18): it runs cargo from the workspace, so
# rustup applies the pinned toolchain, and checks that toolchain and the system
# libraries Rust needs. Its simc mode builds the C port's test crate, simc
# (the port text's section 13), as the static library the C tests' modules
# link, in which only the names simc's headers declare stay global.
#
#   cargo.pl simc <workspace> <target dir> <profile> <output> <depfile> <cc command...>
#   cargo.pl host <workspace>

use strict;
use warnings FATAL => 'all';

use Cwd;
use File::Compare;
use File::Copy;
use File::Path qw(make_path remove_tree);
use File::Spec;

my ($mode, $workspace, @args) = @ARGV;
die "usage: cargo.pl simc|host <workspace> ...\n"
  unless defined $workspace;
my @paths = map { File::Spec->rel2abs($_) } grep { defined } @args[ 0 .. 3 ];
chdir $workspace or die "cargo.pl: cannot enter $workspace: $!\n";

# home is the build machine's home directory, which no built library may name.
my $home = $^O eq 'MSWin32' ? $ENV{USERPROFILE} : $ENV{HOME};

# remaps make rustc name every source by a path the same on every machine: the
# workspace's by its path from the workspace, and the dependencies' by their
# path under cargo's registry, so no machine path is compiled in.
my @remaps = (
	'--remap-path-prefix=' . getcwd() . '=',
	'--remap-path-prefix='
	  . ($ENV{CARGO_HOME} // "$home/.cargo")
	  . '/registry/src=cargo-registry/');
check_toolchain();

# rustc names the standard library's sources by the toolchain's own copy where
# one is installed, so the remap names them as a released standard library
# names them.
push @remaps, std_remap();

if ($mode eq 'host')
{
	print host(), "\n";
}
elsif ($mode eq 'simc')
{
	build(\@args, \@paths);
}
else
{
	die "cargo.pl: unknown mode $mode\n";
}

# pinned returns the release rust-toolchain.toml names.
sub pinned
{
	open my $fh, '<', 'rust-toolchain.toml'
	  or die "cargo.pl: cannot read rust-toolchain.toml: $!\n";
	while (<$fh>)
	{
		return $1 if /^channel\s*=\s*"([^"]+)"/;
	}
	die "cargo.pl: rust-toolchain.toml names no channel\n";
}

# check_toolchain fails unless rustc is the pinned release.
sub check_toolchain
{
	my $want = pinned();
	my $version = `rustc --version`;
	die "cargo.pl: rustc not found; install Rust $want with rustup\n"
	  if $? != 0;
	my ($have) = $version =~ /^rustc (\S+)/;
	die "cargo.pl: rustc is $have, but the fork builds with Rust $want\n"
	  unless defined $have && $have eq $want;
}

# std_remap returns the remap of the toolchain's copy of the standard
# library's source to the name a released standard library's panics give it.
sub std_remap
{
	chomp(my $sysroot = `rustc --print sysroot`);
	my ($commit) = `rustc -vV` =~ /^commit-hash: (\S+)$/m;
	die "cargo.pl: rustc names no sysroot or commit\n"
	  unless $sysroot ne '' && defined $commit;
	return "--remap-path-prefix=$sysroot/lib/rustlib/src/rust=/rustc/$commit";
}

# check_no_home fails when a built library names a Rust source by a path in
# the build machine's home directory or the build folder, which the remaps
# keep out; the paths a dependency's assembler or C compiler records in its
# own objects' debug information aren't rustc's, and no panic reports them.
sub check_no_home
{
	my ($library, $target) = @_;
	open my $fh, '<:raw', $library
	  or die "cargo.pl: cannot read $library: $!\n";
	local $/;
	my $bytes = <$fh>;
	close $fh;
	for my $dir ($home, $target)
	{
		my $named = join '[/\\\\]',
		  map { quotemeta } grep { $_ ne '' } split m{[/\\]}, $dir;
		my $source =
		  $^O eq 'MSWin32' ? qr/$named[^\0\n]*?\.rs/i : qr/$named[^\0\n]*?\.rs/;
		die "cargo.pl: $library names a Rust source in $dir\n"
		  if $bytes =~ $source;
	}
}

# host returns the host target of the pinned rustc.
sub host
{
	for (`rustc -vV`)
	{
		return $1 if /^host: (\S+)/;
	}
	die "cargo.pl: rustc reports no host\n";
}

# listed_libs returns the libraries native-libs.tsv says rustc reports for the
# host; meson reads the file's other list itself.
sub listed_libs
{
	my $host = host();
	open my $fh, '<', 'native-libs.tsv'
	  or die "cargo.pl: cannot read native-libs.tsv: $!\n";
	while (<$fh>)
	{
		next if /^#/ || /^\s*$/;
		chomp;
		my ($target, $reported) = split /\t/;
		return $reported if $target eq $host;
	}
	die "cargo.pl: native-libs.tsv lists no libraries for $host\n";
}

# build builds simc's library with cargo, checks the system libraries it
# needs, and hands meson the library the C tests' modules link and its
# dependency file.
sub build
{
	my ($given, $paths) = @_;
	die "usage: cargo.pl simc <workspace> <target dir> <profile> <output> <depfile> <cc command...>\n"
	  unless @$given >= 5;
	my ($target, $output, $depfile) = @$paths[ 0, 2, 3 ];
	my ($profile, $named) = @$given[ 1, 2 ];
	my @cc = @$given[ 4 .. $#$given ];

	$ENV{CARGO_TARGET_DIR} = $target;
	delete $ENV{LIBCLANG_PATH} if ($ENV{LIBCLANG_PATH} // '') eq '';

	# A build script's C is otherwise compiled for the machine's own macOS
	# version, which the server's link warns of. Cargo doesn't build such C
	# again for another version, so a target folder built for another, or
	# before its version was kept, is emptied.
	if ($^O eq 'darwin')
	{
		my $version = $ENV{MACOSX_DEPLOYMENT_TARGET} = deployment_target(@cc);
		my $kept = "$target/macos-version";
		my $built = '';
		if (open my $fh, '<', $kept)
		{
			chomp($built = <$fh> // '');
			close $fh;
		}
		if ($built ne $version)
		{
			remove_tree($target);
			make_path($target);
			open my $fh, '>', $kept or die "cargo.pl: cannot write $kept: $!\n";
			print $fh "$version\n";
			close $fh;
		}
	}
	my $out = "$target/$profile";
	my $instrument = '';

	# The remaps, with the build folder's, which holds the sources build
	# scripts generate, join the flags the environment sets when it sets any,
	# since cargo then reads no configured flags, and the configuration's flags
	# otherwise. They go in as CARGO_ENCODED_RUSTFLAGS, which cargo prefers to
	# RUSTFLAGS and which separates flags by 0x1f, so a path with a space stays
	# one flag.
	my @flags = (@remaps, "--remap-path-prefix=$target=target/");
	if (defined $ENV{CARGO_ENCODED_RUSTFLAGS} || defined $ENV{RUSTFLAGS})
	{
		my @set =
		  defined $ENV{CARGO_ENCODED_RUSTFLAGS}
		  ? split(/\x1f/, $ENV{CARGO_ENCODED_RUSTFLAGS})
		  : split(' ', $ENV{RUSTFLAGS});
		$ENV{CARGO_ENCODED_RUSTFLAGS} = join "\x1f", @set, @flags;
		delete $ENV{RUSTFLAGS};
	}
	else
	{
		$instrument .=
		  qq{ --config "build.rustflags=[}
		  . join(', ', map { "'$_'" } @flags) . qq{]"};
	}
	my $noted = "$out/native-static-libs.txt";

	open my $cargo, '-|',
	  'cargo rustc --locked --quiet --message-format=json-render-diagnostics --profile '
	  . $profile
	  . $instrument
	  . ' -p simc'
	  . ' --lib -- --print=native-static-libs 2>&1'
	  or die "cargo.pl: cannot run cargo: $!\n";
	my $libs;
	while (<$cargo>)
	{
		next if /^\{/;
		if (/native-static-libs: (.*)$/)
		{
			$libs = $1;
			next;
		}
		next if /^note: link against the following native artifacts/;
		print STDERR $_;
	}
	close $cargo or die "cargo.pl: cargo failed\n";

	if (defined $libs)
	{
		open my $fh, '>', $noted or die "cargo.pl: cannot write $noted: $!\n";
		print $fh "$libs\n";
		close $fh;
	}
	open my $fh, '<', $noted
	  or die "cargo.pl: $noted is missing; remove $out to rebuild\n";
	chomp($libs = <$fh>);
	close $fh;
	my $listed = listed_libs();

	# simc's sim links the zstd Postgres links through pg's outside_postgres
	# feature, which the test modules link themselves.
	(my $needed = $libs) =~ s/(?:^|\s)(?:zstd\.lib|-lzstd)(?=\s|$)//g;
	$needed =~ s/^\s+//;
	die "cargo.pl: Rust needs \"$libs\", but native-libs.tsv lists \"$listed\" for "
	  . host() . "\n"
	  unless $needed eq $listed;

	my $built = $^O eq 'MSWin32' ? "$out/simc.lib" : "$out/libsimc.a";
	my $linked = "$out/linked";
	if (!-e $output || !-e $linked || compare($built, $linked) != 0)
	{
		# A module built by MSVC exports only the names it marks, so MSVC links
		# the library as it is.
		if ($^O eq 'MSWin32')
		{
			copy($built, $output) or die "cargo.pl: cannot copy $built: $!\n";
		}
		else
		{
			localize($built, $output, "$out/merge", [simc_exports()], @cc);
		}
		copy($built, $linked) or die "cargo.pl: cannot copy $built: $!\n";
	}
	check_no_home($built, $target);

	(my $info = $built) =~ s/\.(a|lib)$/.d/;
	open my $in, '<', $info or die "cargo.pl: cannot read $info: $!\n";
	local $/;
	my $deps = <$in>;
	close $in;
	$deps =~ s/^[^\n]*?\Q$built\E:/$named:/;
	open my $dep, '>', $depfile or die "cargo.pl: cannot write $depfile: $!\n";
	print $dep $deps;
	close $dep;
}

# deployment_target returns the macOS version the C compiler builds for, which
# cargo then builds every object for.
sub deployment_target
{
	my $defined = join ' ', map { quotemeta } @_, qw(-dM -E -x c /dev/null);
	`$defined` =~ /__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ (\d+)/
	  or die "cargo.pl: the C compiler names no macOS version it builds for\n";
	return int($1 / 10000) . '.' . int($1 / 100) % 100;
}

# run runs a command and fails if it does.
sub run
{
	system(@_) == 0 or die "cargo.pl: @_[0 .. 1] failed\n";
}

# simc_exports returns the names of the functions simc's headers declare, the
# ones simc defines, which stay global in its object; the backend's state the
# runner's C gives simc is in simc/backend.h, which it doesn't read.
sub simc_exports
{
	my @names;
	for my $header ('../include/doltgres/simc/simc.h', '../include/doltgres/std/test_fs.h')
	{
		open my $fh, '<', $header or die "cargo.pl: cannot read $header: $!\n";
		local $/;
		my $text = <$fh>;
		close $fh;
		push @names, $text =~ /^extern\b[^;(]*?\b(\w+)\s*\(/mg;
	}
	return @names;
}

# localize merges the static library into one relocatable object in which every
# symbol is local except the names kept.
sub localize
{
	my ($library, $object, $work, $kept, @cc) = @_;
	my $tools = `rustc --print sysroot`;
	chomp $tools;
	$tools .= '/lib/rustlib/' . host() . '/bin';
	remove_tree($work);
	make_path($work);

	my %seen;
	for (`"$tools/llvm-ar" t "$library"`)
	{
		chomp;
		die "cargo.pl: $library holds two members named $_\n" if $seen{$_}++;
	}

	my @keep = @$kept;
	my $keep = "$work/keep.txt";
	open my $fh, '>', $keep or die "cargo.pl: cannot write $keep: $!\n";
	print $fh map { ($^O eq 'darwin' ? '_' : '') . "$_\n" } @keep;
	close $fh;

	if ($^O eq 'darwin')
	{
		# Apple's tools can't read the LLVM bitcode Rust embeds in its objects,
		# so it is removed, with the toolchain's own llvm-objcopy, before the merge.
		# A member whose atoms the linker can't strip one by one (the assembly
		# helpers for outline atomics, which nothing calls on Apple's processors)
		# is left out, since it would make the whole merge unstrippable; were one
		# needed, a module's link would fail on its missing symbol.
		run("$tools/llvm-ar", 'x', "--output=$work", $library);
		opendir my $dh, $work or die "cargo.pl: cannot read $work: $!\n";
		my @members = grep { `otool -hv $_` =~ /SUBSECTIONS_VIA_SYMBOLS/ }
		  map { "$work/$_" } sort grep { /\.o$/ } readdir $dh;
		closedir $dh;
		run("$tools/llvm-objcopy", '--remove-section=__LLVM,__bitcode', $_)
		  for @members;
		run(@cc, '-r', '-nostdlib', '-Wl,-fatal_warnings', '-o', "$work/merged.o",
			@members);
		run('nmedit', '-s', $keep, "$work/merged.o", '-o', $object);
	}
	else
	{
		run(@cc, '-r', '-nostdlib', '-o', "$work/merged.o", '-Wl,--whole-archive',
			$library, '-Wl,--no-whole-archive');
		run("$tools/llvm-objcopy",
			@keep
			? ("--keep-global-symbols=$keep")
			: ('--wildcard', '--localize-symbol=*'),
			"$work/merged.o", $object);
	}
}
