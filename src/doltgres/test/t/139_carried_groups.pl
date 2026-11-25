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

# A key change that carries more of a map key's versions than one prepare's
# group takes (bugs row 372): in the transaction that inserted them, a table's
# primary key dropped and added again carries its free space map's versions
# into the new storage group after group, each group's versions in a record of
# their own, where the carry once finished every version against the first
# group and stopped the server; the rows read back on the primary after a
# crash and after a checkpoint and a crash, and on its streaming standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = 400;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'delta_memory = 2MB');
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

my $start =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$primary->safe_psql(
	'postgres', qq{
CREATE TABLE rekeyed (id int4 PRIMARY KEY, v text);
BEGIN;
INSERT INTO rekeyed SELECT g, (SELECT string_agg(md5(g::text || '.' || k::text), '') FROM generate_series(1, 250) k) FROM generate_series(1, $rows) g;
ALTER TABLE rekeyed DROP CONSTRAINT rekeyed_pkey;
ALTER TABLE rekeyed ADD PRIMARY KEY (id);
COMMIT});

# The carry took more than one group: some map key's versions went into two
# or more records.
cmp_ok(
	$primary->safe_psql(
		'postgres', qq{
SELECT max(n) FROM (SELECT count(*) AS n FROM pg_get_wal_records_info('$start', pg_current_wal_flush_lsn())
WHERE record_type = 'MapCarried' GROUP BY substring(description FROM '^(.*); [0-9]+ versions\$')) AS keys}
	),
	'>',
	1,
	'a map key\'s carried versions take more than one group\'s record');

my $wanted = "$rows|" . ($rows * 8000);
my $read = 'SELECT count(*), sum(length(v)) FROM rekeyed';

# Bugs row 372's cases: the standby replays the carry, and the primary reads
# its rows after a crash, then after a checkpoint and a crash.
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', $read),
	$wanted, 'the standby reads the rekeyed rows (row 372)');
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $read),
	$wanted, 'the primary reads the rekeyed rows after a crash (row 372)');
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->stop('immediate');
$primary->start;
is( $primary->safe_psql('postgres', $read),
	$wanted,
	'the primary reads the rekeyed rows after a checkpoint and a crash (row 372)'
);

$standby->stop;
$primary->stop;
done_testing();
