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


# The take-back of an aborted transaction's versions (2g): its abort, a
# subtransaction's, and a terminated backend's exit prune the keys it wrote
# to, and leave its free space map's takes for the fold or VACUUM, so the next
# insert takes the ctids heap's takes on the reference. No read of the delta
# reaches SQL yet, so each case reads the ctids the next insert takes, through
# the test module's insert.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('removal');
$node->init;
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test; CALL doltgres_test_twins();
CREATE TABLE source_three (a int4);
INSERT INTO source_three VALUES (1), (2), (3);
CREATE TABLE taken (a int4);
});

# INSERTED is the call that inserts the source's three rows into the table one
# at a time and returns their ctids.
my $INSERTED =
  "SELECT doltgres_test_insert('taken', 'source_three', false, 0)";

is($node->safe_psql('postgres', $INSERTED),
	"(0,1)\n(0,2)\n(0,3)", 'three rows take the first three ctids');

is($node->safe_psql('postgres', "BEGIN; $INSERTED; ROLLBACK;"),
	"(0,4)\n(0,5)\n(0,6)", 'a transaction that rolls back takes the next three');
is($node->safe_psql('postgres', $INSERTED),
	"(0,7)\n(0,8)\n(0,9)",
	"after an aborted insert the next takes the ctids past it, as heap's does");

is( $node->safe_psql(
		'postgres', qq{
BEGIN;
$INSERTED;
SAVEPOINT inner_rows;
$INSERTED;
ROLLBACK TO SAVEPOINT inner_rows;
$INSERTED;
COMMIT;
}),
	"(0,10)\n(0,11)\n(0,12)\n(0,13)\n(0,14)\n(0,15)\n(0,16)\n(0,17)\n(0,18)",
	"inside a rolled-back savepoint's parent the next insert takes the ctids past it"
);
is($node->safe_psql('postgres', $INSERTED),
	"(0,19)\n(0,20)\n(0,21)",
	"a rolled-back savepoint's rows are taken back and its parent's kept");

my $session = $node->background_psql('postgres');
$session->query_safe('BEGIN');
is($session->query_safe($INSERTED),
	"(0,22)\n(0,23)\n(0,24)", 'a session inserts inside its transaction');
my $pid = $session->query_safe('SELECT pg_backend_pid()');
$node->safe_psql('postgres', "SELECT pg_terminate_backend($pid)");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid")
  or die "the terminated backend never left";
eval { $session->quit };
is($node->safe_psql('postgres', $INSERTED),
	"(0,25)\n(0,26)\n(0,27)",
	"a backend terminated inside its transaction takes its rows back at its exit's abort"
);

$node->stop;
done_testing();
