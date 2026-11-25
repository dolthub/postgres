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

# Reads of a table whose rows spilled into more listed files than the
# backend's reads once held: each listed file's read is kept while it is
# listed, under work_mem, so a sequential read builds each file's name once
# and a key fetch walks each chunk once, not once a row, and past the bound
# for the files' indexes a read seeks two index pages a row at most (bugs rows
# 487 and 485).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $commits = 12;
my $each = 2000;
my $rows = $commits * $each;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"delta_memory = 16MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off"
);
$node->start;

# Each commit's writer spills its own versions before it commits, into files
# of its own transaction, so the table lists a file or more for every commit;
# the delta never reaches its budget, so no writer asks the fold launcher for
# a fold that would take the files away.
$node->safe_psql('postgres',
	'CREATE EXTENSION doltgres_test; CREATE TABLE spilled (k int4 PRIMARY KEY, pad text)'
);
$node->safe_psql('postgres',
	"BEGIN; INSERT INTO spilled SELECT g, repeat('x', 100) FROM generate_series("
	  . ($_ * $each + 1) . ', '
	  . ($_ * $each + $each)
	  . ') g; SELECT count(*) FROM generate_series(1, 100) WHERE doltgres_test_delta(61, 0, 0) = 1; COMMIT'
) for 0 .. $commits - 1;

# The counts a read adds under a work_mem: the spill files' names it builds,
# one for each read of a file it makes, the chunks it walks, the files'
# indexes it reads whole, their index pages it reads alone, seeking, the files
# its reads open, and their filters' pages it reads from the files.
sub counted
{
	my ($work_mem, $query) = @_;
	my ($built, $walked, $indexes, $pages,
		$opened, $filters, $count, $built_after,
		$walked_after, $indexes_after, $pages_after, $opened_after,
		$filters_after)
	  = split /\n/,
	  $node->safe_psql(
		'postgres',
		"SET work_mem = '$work_mem'; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SELECT doltgres_test_spill_reads(1); SELECT doltgres_test_spill_reads(2); SELECT doltgres_test_spill_reads(3); SELECT doltgres_test_spill_reads(4); SELECT doltgres_test_spill_reads(5); SELECT doltgres_test_spill_reads(6); $query; SELECT doltgres_test_spill_reads(1); SELECT doltgres_test_spill_reads(2); SELECT doltgres_test_spill_reads(3); SELECT doltgres_test_spill_reads(4); SELECT doltgres_test_spill_reads(5); SELECT doltgres_test_spill_reads(6)"
	  );
	return (
		$count,
		$built_after - $built,
		$walked_after - $walked,
		$indexes_after - $indexes,
		$pages_after - $pages,
		$opened_after - $opened,
		$filters_after - $filters);
}

my ($count, $built, $walked) =
  counted('4MB', 'SET enable_indexscan = off; SELECT count(*) FROM spilled');
note(
	"the sequential read of $count rows built $built names and walked $walked chunks"
);
is($count, $rows, 'the sequential read reads every row');
cmp_ok($built, '>=', 9,
	'the table lists more files than the backend once kept');
cmp_ok($built, '<', $rows / 100,
	'the sequential read builds each listed file\'s name once, not once a row (row 487)'
);

my ($pk_count, $pk_built, $pk_walked) = counted('4MB',
	'SET enable_seqscan = off; SELECT count(*) FROM spilled WHERE k <= 2000');
note(
	"the primary key's read of $pk_count rows built $pk_built names and walked $pk_walked chunks"
);
is($pk_count, 2000, 'the primary key\'s read reads every row it selects');
cmp_ok($pk_walked, '<', 200,
	'the primary key\'s read walks each chunk once, not once a row (row 487)'
);
cmp_ok($pk_built, '<', 100,
	'the primary key\'s read builds each listed file\'s name once, not once a row, its filter checks included (row 487)'
);

# Past the bound, at work_mem's least, the reads keep only their indexes and
# cursors: a step past the cursor's key reads the next key alone, so the reads
# still walk each file's strides about once, not once a row (bugs row 487).
for my $low (
	[
		'sequential read',
		$rows, 'SET enable_indexscan = off; SELECT count(*) FROM spilled'
	],
	[
		'primary key\'s read',
		2000,
		'SET enable_seqscan = off; SELECT count(*) FROM spilled WHERE k <= 2000'
	])
{
	my ($what, $want, $query) = @$low;
	my ($low_count, $low_built, $low_walked, $low_indexes) =
	  counted('64kB', $query);
	note(
		"at work_mem 64kB the $what of $low_count rows opened $low_built files, read $low_indexes indexes, and walked $low_walked chunks"
	);
	is($low_count, $want, "past the bound the $what reads every row");
	cmp_ok($low_walked, '<', 200,
		"past the bound the $what walks each file's strides once, not once a row (row 487)"
	);
	cmp_ok($low_built, '<', 100,
		"past the bound the $what opens each file once, not once a row (row 487)"
	);
	cmp_ok($low_indexes, '<', 100,
		"past the bound the $what reads each file's index once, not once a row (row 487)"
	);
}

# Past the bound for the files' indexes alone: keys of a thousand bytes give
# each file's index about a kilobyte a stride, so the files' indexes pass
# work_mem's least; the reads that can't keep theirs keep only themselves,
# their open files, and their cursors, and seek down the index's pages, the
# root and a leaf, two pages a key fetch at most (bugs row 487).
my $wide_each = 400;
my $wide_rows = $commits * $wide_each;
$node->safe_psql('postgres',
	'CREATE TABLE wide (k text COLLATE "C" PRIMARY KEY, pad text)');
$node->safe_psql('postgres',
	"BEGIN; INSERT INTO wide SELECT lpad(g::text, 1000, '0'), repeat('x', 100) FROM generate_series("
	  . ($_ * $wide_each + 1) . ', '
	  . ($_ * $wide_each + $wide_each)
	  . ') g; SELECT count(*) FROM generate_series(1, 100) WHERE doltgres_test_delta(61, 0, 0) = 1; COMMIT'
) for 0 .. $commits - 1;
for my $wide (
	[
		'sequential read',
		$wide_rows, 'SET enable_indexscan = off; SELECT count(*) FROM wide'
	],
	[
		'primary key\'s read',
		$wide_rows,
		"SET enable_seqscan = off; SELECT count(*) FROM wide WHERE k >= ''"
	])
{
	my ($what, $want, $query) = @$wide;
	my ($wide_count, $wide_built, $wide_walked, $wide_indexes, $wide_pages) =
	  counted('64kB', $query);
	note(
		"past the index bound the $what of $wide_count rows opened $wide_built files, read $wide_indexes indexes whole and $wide_pages index pages alone, and walked $wide_walked chunks"
	);
	is($wide_count, $want, "past the index bound the $what reads every row");
	cmp_ok($wide_pages, '>', 0,
		"past the index bound the reads of the $what that keep no index seek down its pages"
	);
	cmp_ok($wide_indexes, '<', 100,
		"past the index bound the $what reads each file's index whole once, not once a row (row 487)"
	);
	cmp_ok($wide_pages, '<=', 2 * $want,
		"past the index bound the $what reads two index pages a row at most (row 487)"
	);
}

# A primary key's read of the newest commit's keys starts past every key of
# the older commits' files: each such file's first step finds no key past the
# scan's place and leaves its cursor at its last key, so its later steps
# answer nothing at once instead of walking its last stride a row (bugs row
# 487).
my ($newest_count, $newest_built, $newest_walked,
	$newest_indexes, $newest_pages)
  = counted(
	'64kB',
	"SET enable_seqscan = off; SELECT count(*) FROM wide WHERE k > lpad('"
	  . ($wide_rows - $wide_each)
	  . "', 1000, '0')");
note(
	"past the index bound the newest commit's primary key read of $newest_count rows opened $newest_built files, read $newest_indexes indexes whole and $newest_pages index pages alone, and walked $newest_walked chunks"
);
is($newest_count, $wide_each,
	'past the index bound the newest commit\'s primary key read reads its rows'
);
cmp_ok($newest_walked, '<', 100,
	'past the index bound a file whose keys all lie before the scan walks its last stride once, not once a row (row 487)'
);
cmp_ok($newest_pages, '<=', 2 * $wide_each,
	'past the index bound the newest commit\'s primary key read reads two index pages a row at most (row 487)'
);

# Past the bound for the reads themselves: a table of more listed files than
# work_mem's least holds reads for. The newest reads go whole and a row that
# touches one opens its file again, reading the trailer and seeking its stride
# down the index's pages, never the index whole (bugs row 487).
my $many_commits = 160;
my $many_each = 25;
my $many_rows = $many_commits * $many_each;
$node->safe_psql('postgres',
	'CREATE TABLE many (k int4 PRIMARY KEY, pad text)');
$node->safe_psql('postgres',
	"BEGIN; INSERT INTO many SELECT g, repeat('x', 100) FROM generate_series("
	  . ($_ * $many_each + 1) . ', '
	  . ($_ * $many_each + $many_each)
	  . ') g; SELECT count(*) FROM generate_series(1, 100) WHERE doltgres_test_delta(61, 0, 0) = 1; COMMIT'
) for 0 .. $many_commits - 1;
my (undef, $many_files) =
  counted('4MB', 'SET enable_indexscan = off; SELECT count(*) FROM many');
cmp_ok($many_files, '>=', 130,
	'the table lists more files than work_mem\'s least holds reads for');

for my $many (
	[
		'sequential read',
		$many_rows, 'SET enable_indexscan = off; SELECT count(*) FROM many'
	],
	[
		'primary key\'s read',
		$many_rows,
		'SET enable_seqscan = off; SELECT count(*) FROM many WHERE k > 0'
	])
{
	my ($what, $want, $query) = @$many;
	my ($many_count, $many_built, $many_walked, $many_indexes, $many_pages) =
	  counted('64kB', $query);
	note(
		"past the reads' bound the $what of $many_count rows over $many_files files opened $many_built files, read $many_indexes indexes whole and $many_pages index pages alone, and walked $many_walked chunks"
	);
	is($many_count, $want, "past the reads' bound the $what reads every row");
	cmp_ok($many_indexes, '<=', $many_files,
		"past the reads' bound the $what reads each file's index whole once at most, not once a row (row 487)"
	);
	cmp_ok($many_pages, '<=', 2 * $many_built,
		"past the reads' bound the $what reads two index pages an open at most (row 487)"
	);
}

# A filter check through a read the trim dropped opens nothing: the read's
# file opens only at its first step or key fetch, so past the reads' bound
# every file a read opens it reads from, and the filters' pages come from the
# node cache, read from a file once each (bugs row 506).
my (undef, undef, $fetch_walked, undef, undef, $fetch_opened, $fetch_filters)
  = counted('64kB',
	'SET enable_seqscan = off; SELECT count(*) FROM many WHERE k > 0');
note(
	"past the reads' bound the primary key's read of $many_rows rows opened $fetch_opened files, walked $fetch_walked chunks, and read $fetch_filters filter pages from files"
);
cmp_ok($fetch_opened, '<=', $fetch_walked + $many_files + $many_rows,
	'past the reads\' bound a filter check opens no file: the files opened are at most the strides walked, one a file, and one a row (row 506)'
);
cmp_ok($fetch_filters, '<=', $many_files,
	'past the reads\' bound the filters\' pages are read from their files once each, not once a row (row 506)'
);

$node->stop;

done_testing();
