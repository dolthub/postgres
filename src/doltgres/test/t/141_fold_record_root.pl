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

# A fold in a backend that never asked about doltgres-fold-record-root,
# with the point attached, completes and logs the other root the point asks
# for (bugs row 368), where asking inside the install's checkpoint window
# once loaded the point there and allocated inside its critical section; the
# streaming standby's fold of the comparator's tree warns at the other root.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->safe_psql(
	'postgres', q{
CREATE EXTENSION doltgres_test;
CREATE EXTENSION injection_points;
CREATE FUNCTION ranged_cmp(a int4, b int4) RETURNS int4 LANGUAGE sql IMMUTABLE AS 'SELECT btint4cmp(a, b)';
CREATE OPERATOR CLASS ranged_ops FOR TYPE int4 USING btree AS
	OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 ranged_cmp(int4, int4);
CREATE TYPE ranged AS RANGE (subtype = int4, subtype_opclass = ranged_ops);
CREATE TABLE ranged_keys (k ranged PRIMARY KEY, v text);
});
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
$primary->safe_psql('postgres',
	"INSERT INTO ranged_keys SELECT ranged(n, n + 1), 'r' FROM generate_series(1, 10) n"
);
$primary->wait_for_catchup($standby);

# Bugs row 368's case: the point attached in one backend, the fold in a new
# one.
my $offset = -s $standby->logfile;
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-fold-record-root', 'notice')");
is( $primary->safe_psql(
		'postgres', "SELECT doltgres_test_fold('ranged_keys')"),
	't',
	'the fold completes with doltgres-fold-record-root attached (row 368)');
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fold-record-root')");
ok( $standby->wait_for_log(
		qr/WARNING:  standby's fold of relation "ranged_keys" does not match its primary's/,
		$offset),
	'the fold logged the root the point asks for (row 368)');

$standby->stop;
$primary->stop;
done_testing();
