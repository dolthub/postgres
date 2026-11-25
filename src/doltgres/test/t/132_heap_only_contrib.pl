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

# The heap-only contrib modules refuse the fork's relations (design item 5;
# bugs row 330): each function of pageinspect, pg_surgery,
# pgstattuple, and amcheck that reads a heap's or an index's pages refuses a
# relation the fork stores with its own access method or relation kind
# check's SQLSTATE and message, as the reference raises them for a table of
# heap2 (heap's handler under another name), an index of another method, or
# a view, while an index of a method the fork doesn't store reads as
# Postgres' own; and pg_prewarm, whose reading of the fork's trees is bugs row
# 331's, returns without failing the server.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('contrib');
$node->init;
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE EXTENSION pg_visibility;
CREATE EXTENSION pg_surgery;
CREATE EXTENSION pgstattuple;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_prewarm;
CREATE TABLE forked (id int4 PRIMARY KEY, v int4);
INSERT INTO forked SELECT g, g FROM generate_series(1, 100) g;
CREATE INDEX forked_v ON forked (v);
CREATE INDEX forked_hash ON forked USING hash (v)});

# refused returns a statement's error as its SQLSTATE and message.
sub refused
{
	my ($sql) = @_;
	my ($ret, $out, $err) =
	  $node->psql('postgres', "\\set VERBOSITY verbose\n$sql");
	my ($error) = $err =~ /ERROR:  ([0-9A-Z]{5}: [^\n]*)/;
	return $error // "no error: $out";
}

# Each case: the call on a fork relation, and the reference's error for the
# same check, with the fork relation's name.
my @cases = (
	[
		q{SELECT tuple_data_split('forked'::regclass, '\x00'::bytea, 0, 1, NULL)},
		'0A000: only heap AM is supported'
	],
	[
		q{SELECT get_raw_page('forked', 0)},
		'42809: cannot get raw page from relation "forked"'
	],
	[
		q{SELECT bt_page_items('forked_v', 1)},
		'42809: "forked_v" is not a btree index'
	],
	[
		q{SELECT bt_metap('forked_v')},
		'42809: "forked_v" is not a btree index'
	],
	[
		q{SELECT heap_force_kill('forked', ARRAY['(0,1)']::tid[])},
		'0A000: only heap AM is supported'
	],
	[ q{SELECT pgstattuple('forked')}, '0A000: only heap AM is supported' ],
	[
		q{SELECT pgstattuple('forked_v')},
		'0A000: index "forked_v" (unknown index) is not supported'
	],
	[
		q{SELECT pgstattuple_approx('forked')},
		'0A000: only heap AM is supported'
	],
	[
		q{SELECT pgstatindex('forked_v')},
		'42809: relation "forked_v" is not a btree index'
	],
	[ q{SELECT verify_heapam('forked')}, '0A000: only heap AM is supported' ],
	[
		q{SELECT bt_index_check('forked_v')},
		'0A000: expected "btree" index as targets for verification'
	],);
for my $case (@cases)
{
	my ($sql, $wanted) = @$case;
	is(refused($sql), $wanted, "$sql refuses a fork relation");
}

# An index of a method whose entries the fork doesn't store yet, hash here,
# keeps Postgres' own pages, which the modules read as they read heap's: the
# hash, GiST, BRIN, and GIN functions' checks refuse such an index once the
# fork stores its method's entries, each method in its own piece after the
# switch (PLAN's Order 2d).
like(
	$node->safe_psql('postgres', q{SELECT pgstathashindex('forked_hash')}),
	qr/^\([0-9]+,/,
	"an index of a method the fork doesn't store reads as Postgres' own");

# get_raw_page refuses a fork table's main fork alone: its visibility map
# fork, which the fork never writes, has no blocks, as the reference's error
# for a fork with none says. Its free space map fork's page, Postgres' own
# (bugs row 87), is bugs row 333's case.
is( refused(q{SELECT get_raw_page('forked', 'vm', 0)}),
	'22023: block number 0 is out of range for relation "forked"',
	'a fork table\'s visibility map fork has no blocks');

# pg_prewarm doesn't yet read a fork relation's trees (bugs row 331), but its
# call returns, whatever it answers, and the server stays up.
my ($ret) = $node->psql('postgres', q{SELECT pg_prewarm('forked')});
ok(defined $ret, 'pg_prewarm of a fork table returns');
is($node->safe_psql('postgres', 'SELECT count(*) FROM forked'),
	'100', 'the server reads the table after pg_prewarm');

# Bugs row 348: pg_visibility works on a fork table, which keeps no
# visibility map since every block is all-visible and all-frozen (design
# item 5): each block within the table reads all-visible and all-frozen, in
# the map and on its page, and a block past it neither; the summary counts
# every block; pg_check_visible and pg_check_frozen find no tuple against
# that, a running insert's and a deleted row's included; and truncating the
# map changes none of it.
$node->safe_psql('postgres',
	q{CREATE TABLE visible (k int4, v text); INSERT INTO visible SELECT g, repeat('v', 200) FROM generate_series(1, 2000) g; DELETE FROM visible WHERE k <= 10}
);
my $running = $node->background_psql('postgres');
$running->query_safe(
	q{BEGIN; INSERT INTO visible SELECT g, 'running' FROM generate_series(2001, 2100) g}
);
my $mapped = q{SELECT count(*) || ' ' || count(*) FILTER (WHERE all_visible AND all_frozen) FROM pg_visibility_map('visible')};
my $blocks = $node->safe_psql('postgres', q{SELECT count(*) FROM pg_visibility_map('visible')});
ok($blocks > 1, 'a fork table of 2,000 rows maps more than one block');
is($node->safe_psql('postgres', $mapped),
	"$blocks $blocks", 'every block of a fork table is all-visible and all-frozen in its map');
is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FILTER (WHERE all_visible AND all_frozen AND pd_all_visible) FROM pg_visibility('visible')}
	),
	$blocks,
	'and on its page');
is( $node->safe_psql(
		'postgres',
		q{SELECT pg_visibility_map('visible', 0) || ' ' || pg_visibility('visible', 0) || ' ' || pg_visibility_map('visible', 1000000) || ' ' || pg_visibility('visible', 1000000)}
	),
	'(t,t) (t,t,t) (f,f) (f,f,f)',
	'a block of the table reads all-visible and all-frozen, and a block past it neither');
is( $node->safe_psql('postgres', q{SELECT pg_visibility_map_summary('visible')}),
	"($blocks,$blocks)", 'the summary counts every block both');
is( $node->safe_psql(
		'postgres',
		q{SELECT (SELECT count(*) FROM pg_check_visible('visible')) || ' ' || (SELECT count(*) FROM pg_check_frozen('visible'))}
	),
	'0 0',
	'no tuple of a fork table contradicts its blocks, with a running insert and a deleted row');
$running->query_safe('ROLLBACK');
$running->quit;
$node->safe_psql('postgres', q{SELECT pg_truncate_visibility_map('visible')});
is($node->safe_psql('postgres', $mapped),
	"$blocks $blocks", 'truncating the map leaves every block all-visible and all-frozen');

$node->stop;
done_testing();
