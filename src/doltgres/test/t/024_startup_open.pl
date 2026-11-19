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

# The startup's open of repositories before the redo decision (item 6's
# Opening): with only initdb's repositories, a start does exactly what
# Postgres' does, and logs nothing of the fork's, after a clean stop and after a
# crash; over a repository whose newest manifest is damaged, a start reads the
# WAL kept for its fallback and comes up.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('startup_open');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE TABLE written (i int); INSERT INTO written VALUES (1)');
$node->stop('fast');
$node->start;
$node->safe_psql('postgres', 'INSERT INTO written VALUES (2)');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT count(*) FROM written'),
	'2', 'a start after a crash recovers as Postgres does');

my $log = slurp_file($node->logfile);
unlike(
	$log,
	qr/manifest|pack table|a crash left behind|dropped database|list file/,
	"a start with only initdb's repositories logs nothing of the fork's");
my $versioned = $node->data_dir . '/pg_versioned';
opendir my $dir, $versioned or die;
my $made = join ' ', sort { $a <=> $b } grep { !/^\./ } readdir $dir;
closedir $dir;
is($made,
	$node->safe_psql('postgres', "SELECT string_agg(oid::text, ' ' ORDER BY oid) FROM pg_database"),
	"a start makes no repository but its databases'");

$node->stop;

# flip_first_byte flips the bits of a file's first byte in place.
sub flip_first_byte
{
	my ($path) = @_;
	open my $fh, '+<:raw', $path or die "could not open $path: $!";
	read($fh, my $byte, 1) == 1 or die "could not read $path: $!";
	seek($fh, 0, 0) or die "could not seek in $path: $!";
	print $fh chr(ord($byte) ^ 0xff);
	close $fh or die "could not write $path: $!";
	return;
}

# A start over a repository whose newest manifest is damaged warns of it, reads
# the WAL kept for its fallback, from its previous manifest's redo point to the
# start's, and comes up (bugs row 58): before replay begins, and on a promoted
# cluster, where the range starts on the timeline before the promotion.
SKIP:
{
	skip 'Injection points not supported by this build', 6
	  if $ENV{enable_injection_points} ne 'yes';

	my $promoted = PostgreSQL::Test::Cluster->new('startup_open_promoted');
	$promoted->init(allows_streaming => 1);
	$promoted->append_conf('postgresql.conf', "wal_keep_size = '1GB'");
	for my $case (
		[
			$node,
			88888,
			'a start over a damaged newest manifest warns of it',
			'a start over a damaged newest manifest reads the WAL kept for its fallback',
			'a start over a damaged newest manifest comes up'
		],
		[
			$promoted,
			66666,
			'a start after a promotion over a damaged newest manifest warns of it',
			'a start after a promotion reads the WAL kept from before the promotion',
			'a start after a promotion over a damaged newest manifest comes up'
		])
	{
		my ($cluster, $database, $warns, $reads, $up) = @$case;
		my $data = $cluster->data_dir;
		mkdir "$data/pg_versioned" unless -d "$data/pg_versioned";
		$cluster->start;
		$cluster->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
		$cluster->safe_psql('postgres',
			"SELECT doltgres_test_repository($database)");
		$cluster->stop;
		if ($cluster == $promoted)
		{
			$cluster->set_standby_mode;
			$cluster->start;
			$cluster->promote;
			$cluster->safe_psql('postgres', 'CHECKPOINT');
			$cluster->stop;
		}
		flip_first_byte("$data/pg_versioned/$database/manifest");
		my $offset = -s $cluster->logfile;
		$cluster->start;
		ok( $cluster->log_contains(
				qr/WARNING:  database $database: its newest manifest is not whole/,
				$offset),
			$warns);
		ok( !$cluster->log_contains(
				qr/could not read the WAL kept for a manifest's fallback/,
				$offset),
			$reads);
		is($cluster->safe_psql('postgres', 'SELECT 1'), '1', $up);
		$cluster->stop;
	}
}

done_testing();
