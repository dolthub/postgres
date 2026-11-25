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

# A committed transaction's out-of-line values leave the working state's
# chunk delta once no storage of the repository is unfolded past its commit
# (bugs row 301): committed inserts and updates of large EXTERNAL values,
# then a fold of the table and its TOAST storage, leave the delta at its size
# before the inserts, and a cursor opened before the fold reads every value
# after it; and at delta_memory 1MB, rows inserted, half deleted,
# and folded in one transaction, an update of half of them, a save, and a fold
# leave the delta room for the update of the other half, which finishes (bugs
# row 407).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# used returns the bytes the delta holds after a free pass, which frees what removals retired before it.
sub used
{
	my ($node) = @_;

	$node->safe_psql('postgres', 'SELECT doltgres_test_delta(23, 0, 0)');
	return $node->safe_psql('postgres', 'SELECT doltgres_test_delta(0, 0, 0)');
}

# folded folds a table and its TOAST storage, whose value map names the table's out-of-line values.
sub folded
{
	my ($node, $table) = @_;

	$node->safe_psql('postgres',
		"SELECT doltgres_test_fold('$table'); SELECT doltgres_test_fold(reltoastrelid) FROM pg_class WHERE oid = '$table'::regclass"
	);
}

my $node = PostgreSQL::Test::Cluster->new('ended');
$node->init;
$node->append_conf('postgresql.conf',
	"delta_memory = 32MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE large (k int4, v text);
ALTER TABLE large ALTER COLUMN v SET STORAGE EXTERNAL;
SELECT doltgres_test_fold('large');
});
my $before = used($node);
$node->safe_psql('postgres', 'INSERT INTO large SELECT g, repeat(md5(g::text), 600) FROM generate_series(1, 40) g');
$node->safe_psql('postgres', "UPDATE large SET v = v || 'y'");
my $written = used($node);
folded($node, 'large');
my $folded = used($node);
note("the delta held $before bytes before the inserts, $written after the update, and $folded after the fold");
cmp_ok($written, '>', $before + 1024 * 1024, 'the inserts and the update take more than a megabyte of the delta');
cmp_ok($folded, '<=', $before, 'the fold leaves the delta at its size before the inserts');
is($node->safe_psql('postgres', "SELECT count(*), sum(length(v)) FROM large"),
	'40|768040', 'every value reads back after the fold');

# A cursor opened before the fold reads, after the fold took its values' chunks out of the delta, the copies the fold placed in its packs.
$node->safe_psql(
	'postgres', q{
CREATE TABLE cursored (k int4, v text);
ALTER TABLE cursored ALTER COLUMN v SET STORAGE EXTERNAL;
});
folded($node, 'cursored');
$before = used($node);
$node->safe_psql('postgres',
	"INSERT INTO cursored SELECT g, repeat(md5('cursored' || g), 600) FROM generate_series(1, 20) g");
my $expected = $node->safe_psql('postgres', 'SELECT string_agg(md5(v), \',\' ORDER BY k) FROM cursored');
my $cursor = $node->background_psql('postgres');
$cursor->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$cursor->query_safe('DECLARE c NO SCROLL CURSOR FOR SELECT k, md5(v) FROM cursored');
my $first = $cursor->query_safe('FETCH 1 FROM c');
folded($node, 'cursored');
my $rest = $cursor->query_safe('FETCH ALL FROM c');
$cursor->query_safe('COMMIT');
$cursor->quit;
my %read = map { split /\|/ } split(/\n/, "$first\n$rest");
is(join(',', map { $read{$_} } sort { $a <=> $b } keys %read),
	$expected, 'the cursor reads every value after the fold');
cmp_ok(used($node), '<=', $before,
	'a free pass after the cursor ends, with no fold since, leaves the delta at its size before the inserts');
$node->stop;

my $small = PostgreSQL::Test::Cluster->new('small');
$small->init;
$small->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
$small->start;
$small->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE restarted (k int4, v text);
ALTER TABLE restarted ALTER COLUMN v SET STORAGE EXTERNAL;
SELECT doltgres_test_fold('restarted');
});
$before = used($small);
$small->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO restarted SELECT g, repeat(md5(g::text), 100) FROM generate_series(1, 400) g;
DELETE FROM restarted WHERE k % 2 = 0;
SELECT doltgres_test_fold('restarted');
COMMIT;
});
$small->safe_psql('postgres', "UPDATE restarted SET v = v || 'y' WHERE k <= 200");
$small->safe_psql('postgres', 'SELECT doltgres_test_save()');
folded($small, 'restarted');
$folded = used($small);
note("the small delta held $before bytes before the inserts and $folded after the fold");
cmp_ok($folded, '<=', $before, 'the fold leaves the small delta at its size before the inserts');
my ($code, $stdout, $stderr) = $small->psql('postgres',
	"SET statement_timeout = '60s'; UPDATE restarted SET v = v || 'y' WHERE k > 200");
is($code, 0, 'the update of the other half finishes') or diag($stderr);
is($small->safe_psql('postgres', "SELECT count(*), sum(length(v)) FROM restarted"),
	'200|640200', 'every value reads back after the updates');
$small->stop;

done_testing();
