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

# The compression corpus (design item 15): the pinned zstd and lz4 compress a
# fixed corpus to the same bytes on every platform. Each case of
# test/compression/expected.tsv, from the fork's zstd as the storage design
# uses it, TOAST's lz4, and pg_dump's zstd and lz4, gives its recorded length
# and SHA-256.

use strict;
use warnings FATAL => 'all';
use Digest::SHA qw(sha256_hex);
use File::Basename;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $corpus = dirname(__FILE__) . '/../compression';
my @inputs = ('text.txt', 'leaves.bin', 'repeated.txt', 'random.bin');

# raw returns a file's bytes as they are.
sub raw
{
	my ($path) = @_;
	open my $fh, '<:raw', $path or die "could not open $path: $!";
	local $/;
	my $bytes = <$fh>;
	close $fh;
	return $bytes;
}

# summary returns an output's length and SHA-256, as expected.tsv records them.
sub summary
{
	my ($bytes) = @_;
	return length($bytes) . ' ' . sha256_hex($bytes);
}

my $node = PostgreSQL::Test::Cluster->new('compression');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
$node->safe_psql('postgres',
	'CREATE TABLE corpus (id int4 PRIMARY KEY, data bytea)');
for my $i (0 .. $#inputs)
{
	my $hex = unpack('H*', raw("$corpus/corpus/$inputs[$i]"));
	$node->safe_psql('postgres',
		"INSERT INTO corpus VALUES ($i, decode('$hex', 'hex'))");
}

my %dictionaries = (none => 0, dictionary => 1, trained => 2);
my %want;
my %got;
for my $line (split /\n/, raw("$corpus/expected.tsv"))
{
	next if $line =~ /^#/ || $line =~ /^\s*$/;
	my ($case, $codec, $input, $setting, $length, $sha256) = split /\t/,
	  $line;
	push @{ $want{$codec} }, "$case $length $sha256";
	my $result;
	if ($codec eq 'zstd')
	{
		$result = $node->safe_psql('postgres',
			"SELECT length(x) || ' ' || encode(sha256(x), 'hex') FROM doltgres_test_zstd($input, $dictionaries{$setting}) x"
		);
	}
	elsif ($codec eq 'toast-lz4')
	{
		$result = $node->safe_psql('postgres',
			"SELECT coalesce(length(x) || ' ' || encode(sha256(x), 'hex'), 'null none') FROM corpus, doltgres_test_toast_lz4(data) x WHERE id = $input"
		);
	}
	else
	{
		my $dump = PostgreSQL::Test::Utils::tempdir() . '/dump';
		$node->command_ok(
			[
				'pg_dump', '--format' => 'directory',
				'--compress' => $setting, '--table' => 'corpus',
				'--file' => $dump, $node->connstr('postgres')
			],
			"pg_dump --compress $setting dumps the corpus");
		opendir my $dh, $dump or die "could not open $dump: $!";
		my @data = grep { /^\d+\.dat\.(lz4|zst)$/ } readdir $dh;
		closedir $dh;
		$result = summary(raw("$dump/$data[0]"));
	}
	push @{ $got{$codec} }, "$case $result";
}
diag(join("\n", map { @{ $got{$_} } } sort keys %got));

is_deeply($got{zstd}, $want{zstd}, "the fork's zstd output is the recorded one");
is_deeply($got{'toast-lz4'}, $want{'toast-lz4'},
	"TOAST's lz4 output is the recorded one");
is_deeply($got{pg_dump}, $want{pg_dump},
	"pg_dump's zstd and lz4 output is the recorded one");

done_testing();
