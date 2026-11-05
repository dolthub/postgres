# Database roots: a database state's members in one chunk, found by binary
# search, spilling across chunks cut where key hashes say once they outgrow
# a page.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('database_root');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->start;
sub sql { return $node->safe_psql('postgres', shift) }

# A root of the members a query names: kind, name, and a schema and content
# address made from the name.
sub root
{
	my ($members) = @_;
	return sql(qq{SELECT encode(dolt_root_put(array_agg(kind ORDER BY ord), array_agg(name ORDER BY ord),
		array_agg(substr(sha256(convert_to('s' || name, 'UTF8')), 1, 20) ORDER BY ord),
		array_agg(CASE WHEN kind IN ('table', 'index') THEN substr(sha256(convert_to('c' || name, 'UTF8')), 1, 20) END ORDER BY ord)), 'hex')
		FROM ($members) m(kind, name, ord)});
}
sub hexarg { return "decode('$_[0]', 'hex')" }

my $empty = sql(q{SELECT encode(dolt_root_put('{}', '{}', '{}', '{}'), 'hex')});
is(sql("SELECT count(*) FROM dolt_root_members(" . hexarg($empty) . ")"), '0', 'an empty root has no members');

my $small = root(q{VALUES ('table', 'public.t', 1), ('index', 'public.t_pkey', 2), ('namespace', 'public', 3), ('routine', 'public.f(pg_catalog.int4)', 4)});
is(sql("SELECT string_agg(kind || ':' || name || ':' || (content IS NOT NULL), ',') FROM dolt_root_members(" . hexarg($small) . ")"),
	'index:public.t_pkey:true,namespace:public:false,routine:public.f(pg_catalog.int4):false,table:public.t:true',
	'members read back in key order, kind first, with content only where given');
is(root(q{VALUES ('routine', 'public.f(pg_catalog.int4)', 1), ('namespace', 'public', 2), ('index', 'public.t_pkey', 3), ('table', 'public.t', 4)}),
	$small, 'the same members make the same root, in any order');
is(sql("SELECT schema = substr(sha256(convert_to('spublic.t', 'UTF8')), 1, 20) FROM dolt_root_find(" . hexarg($small) . ", 'table', 'public.t')"),
	't', 'a member is found by kind and name');
is(sql("SELECT schema IS NULL FROM dolt_root_find(" . hexarg($small) . ", 'index', 'public.t')"), 't', 'and a missing one is not');
is(sql("SELECT level || ':' || slots FROM dolt_root_nodes(" . hexarg($small) . ")"), '0:4', 'a small root is one leaf');
my ($status, $out, $err) = $node->psql('postgres',
	q{SELECT dolt_root_put('{table,table}', '{public.t,public.t}', ARRAY[decode(repeat('00', 20), 'hex'), decode(repeat('00', 20), 'hex')], '{NULL,NULL}')});
like($err, qr/lists table "public.t" twice/, 'a member may not appear twice');

# Thousands of members spill across nodes, each within a page.
my $tables = q{SELECT kind, format('public.table_%s', g), g FROM generate_series(1, 3000) g, (VALUES ('table'), ('index')) k(kind)};
my $big = root($tables);
my ($nodes, $levels, $largest) = split /\|/, sql("SELECT count(*), max(level), max(bytes) FROM dolt_root_nodes(" . hexarg($big) . ")");
cmp_ok($nodes, '>', 10, "6,000 members spill across nodes ($nodes)");
cmp_ok($levels, '>=', 1, 'under an index');
cmp_ok($largest, '<=', 8100, "each node fits a page ($largest bytes)");
is(sql("SELECT count(*), count(DISTINCT name) FROM dolt_root_members(" . hexarg($big) . ")"), '6000|3000', 'every member reads back');
is(sql("SELECT count(*) FROM generate_series(1, 3000, 37) g, dolt_root_find(" . hexarg($big) . ", 'index', format('public.table_%s', g)) f WHERE f.content = substr(sha256(convert_to('c' || format('public.table_%s', g), 'UTF8')), 1, 20)"),
	'82', 'and is found through the index nodes');

# One more member rewrites one leaf and the nodes above it.
my $grown = root(qq{$tables UNION ALL SELECT 'table', 'public.table_1500a', 0});
is(sql("SELECT count(*) FROM dolt_root_nodes(" . hexarg($grown) . ") n WHERE NOT EXISTS (SELECT 1 FROM dolt_root_nodes(" . hexarg($big) . ") o WHERE o.node = n.node)"),
	$levels + 1, 'one insert changes one node per level');
is(sql("SELECT count(*) FROM dolt_chunk_list() WHERE chunk IN (SELECT node FROM dolt_root_nodes(" . hexarg($grown) . "))"),
	sql("SELECT count(*) FROM dolt_root_nodes(" . hexarg($grown) . ")"), 'and only new chunks are written');

# The functions read any root's members, so only superusers may call them.
sql(q{CREATE ROLE reader});
($status, $out, $err) = $node->psql('postgres', "SET ROLE reader; SELECT count(*) FROM dolt_root_members(" . hexarg($small) . ")");
like($err, qr/permission denied/, 'reading a root needs privilege');
($status, $out, $err) = $node->psql('postgres', "SET ROLE reader; SELECT dolt_root_json(" . hexarg($small) . ")");
like($err, qr/permission denied/, 'and so does its rendering');

$node->stop;
done_testing();
