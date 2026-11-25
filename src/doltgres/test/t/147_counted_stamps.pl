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

# A keyless table's counted entry keeps its copies' stamps in tuple id order
# (bugs row 388): one CREATE TABLE AS writes polygons with a run of 200 NULL
# rows, whose small tuples fill earlier pages' free space, so later copies
# take lower tuple ids; after a fold, a fetch by each NULL row's tuple id, an
# index's rebuild, and a count read every copy, where the fold once kept the
# stamps in insertion order and the search of them missed most. A VACUUM FULL
# rebuilds the table, and every copy reads by its new tuple id.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('counted');
$node->init;
$node->append_conf('postgresql.conf', "fold_threshold = 1GB\nfold_timeout = 3600s");
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE FUNCTION unread(t regclass) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE r record; bad int8 := 0;
BEGIN
  FOR r IN SELECT tid FROM nulls LOOP
    BEGIN
      EXECUTE format('SELECT 1 FROM %s WHERE ctid = $1', t) USING r.tid;
    EXCEPTION WHEN data_corrupted THEN
      bad := bad + 1;
    END;
  END LOOP;
  RETURN bad;
END $$;
CREATE TABLE polygons AS
  SELECT CASE WHEN g BETWEEN 1000 AND 1199 THEN NULL ELSE polygon(box(point(g, g), point(g + 1, g + 1))) END AS f1
  FROM generate_series(1, 3378) g;
CREATE INDEX polygons_f1 ON polygons USING gist (f1);
CREATE TABLE nulls AS SELECT ctid AS tid FROM polygons WHERE f1 IS NULL;
});
is( $node->safe_psql(
		'postgres', 'SELECT count(*), count(DISTINCT (tid::text::point)[0]) > 1 FROM nulls'),
	'200|t',
	'the NULL rows spread over more than one page');
$node->safe_psql('postgres', "SELECT doltgres_test_fold('polygons')");
is($node->safe_psql('postgres', "SELECT unread('polygons')"),
	'0', 'after a fold every NULL row reads by its tuple id (row 388)');
my ($ret, $stdout, $stderr) = $node->psql('postgres', 'REINDEX TABLE polygons');
is($ret . $stderr, '0', 'the index rebuilds over the folded copies (row 388)');
is( $node->safe_psql('postgres',
		'SELECT count(*), count(*) FILTER (WHERE f1 IS NULL) FROM polygons'),
	'3378|200',
	'every copy counts after the fold');
$node->safe_psql('postgres',
	'VACUUM FULL polygons; TRUNCATE nulls; INSERT INTO nulls SELECT ctid FROM polygons WHERE f1 IS NULL');
is($node->safe_psql('postgres', "SELECT unread('polygons')"),
	'0', 'after a VACUUM FULL every NULL row reads by its new tuple id (row 388)');
$node->stop;

done_testing();
