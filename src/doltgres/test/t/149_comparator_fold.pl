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

# An index's fold keeps the one entry of a row whose indexed value changed to
# a different value its operator class calls equal (bugs row 409): numeric's
# 1.0 and 1.00, and citext's 'A' and 'a', each in both directions, so the
# removal of the old entry and the addition of the new one fall in either byte
# order. An UPDATE gives the row a new tuple id; a row inserted after the old
# one's deletion, a fold, and a VACUUM takes the old one's tuple id, so the
# index's delta holds the removal and the addition at one tuple id under keys
# the comparator calls one. After the fold an index scan finds the row once,
# with its new value.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('comparator');
$node->init;
$node->append_conf('postgresql.conf', "fold_threshold = 1GB\nfold_timeout = 3600s\nautovacuum = off");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CREATE EXTENSION citext');

# found checks that an index scan for a value finds only the row given, after a fold of the table and its index.
sub found
{
	my ($name, $value, $row, $label) = @_;

	$node->safe_psql('postgres',
		"SELECT doltgres_test_fold('$name'); SELECT doltgres_test_index_fold('${name}_v')");
	is( $node->safe_psql(
			'postgres', qq{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT k || ':' || v::text FROM $name WHERE v = '$value';
}),
		$row,
		$label);
}

# changed makes a table of twenty rows whose first row's indexed value is
# old, folds it and its index, updates that row's value to new, and checks
# the index after their fold.
sub changed
{
	my ($name, $type, $old, $new, $others) = @_;

	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $name (k int4, v $type);
CREATE INDEX ${name}_v ON $name (v);
INSERT INTO $name VALUES (1, '$old');
INSERT INTO $name SELECT g, $others FROM generate_series(2, 20) g;
SELECT doltgres_test_fold('$name');
SELECT doltgres_test_index_fold('${name}_v');
UPDATE $name SET v = '$new' WHERE k = 1;
});
	found($name, $old, "1:$new", "$name: an index scan after an UPDATE and a fold finds the row once as '$new'");
}

# reused makes a table of one row whose indexed value is old, folds it and its
# index, deletes the row, folds and vacuums the table until its tuple id is
# free, inserts a row whose value is new into that tuple id, and checks the
# index's delta and then the index after their fold.
sub reused
{
	my ($name, $type, $old, $new, $delta) = @_;

	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $name (k int4, v $type);
CREATE INDEX ${name}_v ON $name (v);
INSERT INTO $name VALUES (1, '$old');
SELECT doltgres_test_fold('$name');
SELECT doltgres_test_index_fold('${name}_v');
DELETE FROM $name WHERE k = 1;
SELECT doltgres_test_fold('$name');
});
	$node->safe_psql('postgres', "VACUUM $name");
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$name')");
	$node->safe_psql('postgres', "VACUUM $name");
	$node->safe_psql('postgres', "INSERT INTO $name VALUES (2, '$new')");
	is($node->safe_psql('postgres', "SELECT ctid FROM $name WHERE k = 2"),
		'(0,1)', "$name: the new row takes the deleted row's tuple id");
	$node->safe_psql('postgres', "SELECT doltgres_test_fold('$name')");
	is($node->safe_psql('postgres', "SELECT doltgres_test_index_entries('${name}_v')"),
		$delta, "$name: the index's delta removes and adds an entry at that tuple id");
	found($name, $old, "2:$new", "$name: an index scan after the fold finds the new row once as '$new'");
}

changed('numeric_longer', 'numeric', '1.0', '1.00', 'g * 10');
changed('numeric_shorter', 'numeric', '1.00', '1.0', 'g * 10');
changed('citext_lower', 'citext', 'A', 'a', q{'z' || g});
changed('citext_upper', 'citext', 'a', 'A', q{'z' || g});
reused('reused_lower', 'citext', 'A', 'a', "remove (0,1)\nadd (0,1)");
reused('reused_upper', 'citext', 'a', 'A', "add (0,1)\nremove (0,1)");
reused('reused_longer', 'numeric', '1.0', '1.00', "remove (0,1)\nadd (0,1)");

done_testing();
