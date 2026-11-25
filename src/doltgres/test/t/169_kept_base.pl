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

# An index read and a bitmap read of a folded table keep the base their first
# fetch decoded and decode no base record for each row they fetch; a fold
# that installs a base while a fetch is held between its delta read and its
# check moves the kept base, and the read gives each row once (bugs row 478).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $rows = 200;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"fold_threshold = 1GB\nfold_timeout = 3600s");
$node->start;
$node->safe_psql('postgres',
	"CREATE EXTENSION doltgres_test; CREATE EXTENSION injection_points; CREATE TABLE kept (k int4 PRIMARY KEY, v int4, w text); CREATE INDEX ON kept (v); INSERT INTO kept SELECT g, g, 'old' FROM generate_series(1, 5000) g"
);
$node->safe_psql('postgres', "SELECT doltgres_test_fold('kept')");

foreach my $plan ([ 'index', 'enable_bitmapscan' ],
	[ 'bitmap', 'enable_indexscan' ])
{
	my ($name, $off) = @$plan;
	my ($before, $sum, $after) = split /\n/,
	  $node->safe_psql('postgres',
		"SET enable_seqscan = off; SET $off = off; SET enable_indexonlyscan = off; SELECT doltgres_test_bases_decoded(); SELECT sum(k) FROM kept WHERE v <= $rows; SELECT doltgres_test_bases_decoded()"
	  );
	note(
		"the $name read of $rows rows decoded ",
		$after - $before,
		' base records');
	is( $sum,
		$rows * ($rows + 1) / 2,
		"the $name read reads every row it selects");
	cmp_ok($after - $before, '<', $rows,
		"the $name read decodes fewer base records than it fetches rows (row 478)"
	);
}

# A row the read fetches later changes, unfolded, before the read; the read's
# first fetch holds between its delta read and its check while a fold applies
# the change and installs a base: the held read gives the changed row once,
# from the base it moves to, as a read after it does.
my $read =
  "SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SELECT string_agg(k || ':' || w, ',' ORDER BY k) FROM kept WHERE v <= 3";
$node->safe_psql('postgres', "UPDATE kept SET w = 'new' WHERE k = 3");
my $point = $node->background_psql('postgres');
$point->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-fetch-key-read', 'wait')"
);
$point->query_until(qr/held/, "\\echo held\n$read;\n");
$node->wait_for_event('client backend', 'doltgres-fetch-key-read');
is($node->safe_psql('postgres', "SELECT doltgres_test_fold('kept')"),
	't', 'a fold under the held fetch installs a base');
$node->safe_psql('postgres',
	"SELECT injection_points_detach('doltgres-fetch-key-read'); SELECT injection_points_wakeup('doltgres-fetch-key-read')"
);
my $held = $point->query_safe('SELECT 1');
$point->quit;
$held =~ s/\n1$//;
is($held, '1:old,2:old,3:new',
	'the held read gives the changed row once after the fold moved its base');
is($node->safe_psql('postgres', $read),
	$held, 'a read after the fold gives what the held read gave');

$node->stop;

done_testing();
