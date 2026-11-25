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

# A collection roots the chunks that only spilled versions name (bugs row
# 398): an INSERT ... SELECT copies a folded source whose index's long keys a
# build cut straight into packs, and its row, value map, and index entry
# versions spill out of a 1MB delta, naming the source's chunks. A
# collection's capture then roots every chunk those spilled versions name,
# where it once rooted only the keys the memtable still held; and after the
# source is dropped, a full collection runs before the copy's folds, which
# read every value and every key through the index.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 400;
my $wide =
  "(SELECT string_agg(md5(g::text || '.' || k::text), '') FROM generate_series(1, 70) k)";
my $node = PostgreSQL::Test::Cluster->new('spilled');
$node->init;
$node->append_conf('postgresql.conf',
	"delta_memory = 1MB\nfold_threshold = 1GB\nfold_timeout = 3600s");
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE TABLE source (id int4, v text);
CREATE TABLE copied (id int4, v text);
CREATE INDEX copied_k ON copied (repeat(v, 8));
});
$node->safe_psql('postgres',
	"INSERT INTO source SELECT g, $wide FROM generate_series(1, $rows) g",
	timeout => 1200);
my $toast = "(SELECT reltoastrelid::regclass FROM pg_class WHERE oid = '%s'::regclass)";
$node->safe_psql('postgres',
	sprintf("SELECT doltgres_test_fold($toast), doltgres_test_fold('source')", 'source'));
$node->safe_psql('postgres', "CREATE INDEX source_k ON source (repeat(v, 8))");
my $xid = $node->safe_psql('postgres',
	"BEGIN; INSERT INTO copied SELECT * FROM source; SELECT pg_current_xact_id(); COMMIT;",
	timeout => 1200);

# spilled returns the count of a tree's spill files the copy's transaction
# wrote for a relation's storage.
sub spilled
{
	my ($tree, $relation) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_ls_dir('pg_versioned/' || (SELECT oid FROM pg_database WHERE datname = 'postgres') || '/delta/spill', true, false) f "
		  . "WHERE f LIKE '$tree-%-' || pg_relation_filenode($relation) || '-$xid-%'");
}
cmp_ok(spilled('rows', q{'copied'::regclass}), '>', 0, "the copy's row versions spilled");
cmp_ok(spilled('values', sprintf($toast, 'copied')), '>', 0, "the copy's value map versions spilled");
cmp_ok(spilled('entries', q{'copied_k'::regclass}), '>', 0, "the copy's index entry versions spilled");
is($node->safe_psql('postgres', "SELECT doltgres_test_chunks($xid)"),
	'0', 'the copy carries the packed chunks and writes none of its own');
is($node->safe_psql('postgres', "SELECT doltgres_test_unrooted('copied')"),
	'0', "a collection's capture roots every chunk the copy's spilled row versions name (row 398)");
is($node->safe_psql('postgres', 'SELECT doltgres_test_unrooted(' . sprintf($toast, 'copied') . ')'),
	'0', "a collection's capture roots every chunk the copy's spilled value map versions name (row 398)");
is($node->safe_psql('postgres', "SELECT doltgres_test_unrooted('copied_k')"),
	'0', "a collection's capture roots every chunk the copy's spilled index entry versions name (row 398)");
$node->safe_psql('postgres', 'DROP TABLE source');
$node->safe_psql('postgres', 'CHECKPOINT') for 1 .. 2;
like($node->safe_psql('postgres', 'SELECT doltgres_test_collect()'),
	qr/^\d+ \d+ \d+$/, 'a full collection runs before the copy is folded');
$node->safe_psql('postgres',
	sprintf("SELECT doltgres_test_fold($toast), doltgres_test_fold('copied'), doltgres_test_index_fold('copied_k')",
		'copied'));
my $checked = "WHERE v = " . ($wide =~ s/g::text/id::text/r);
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM copied $checked");
is($stdout, $rows,
	"every value the copy's spilled versions named reads back after a collection (row 398): $stderr");
my $keyed = "SET enable_seqscan = off; SET enable_bitmapscan = off;";
my $through = "SELECT count(*) FROM copied WHERE repeat(v, 8) >= ''";
like($node->safe_psql('postgres', "$keyed EXPLAIN (COSTS OFF) $through"),
	qr/Index Scan using copied_k/, 'the key read scans the index');
($ret, $stdout, $stderr) = $node->psql('postgres', "$keyed $through");
is($stdout, $rows,
	"every key the copy's spilled entry versions named reads back after a collection (row 398): $stderr");
$node->stop;

done_testing();
