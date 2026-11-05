# Extensions belong to the server, as built-in objects do. Their scripts make
# server objects, never a branch's; versioned definitions use their functions,
# operators and types by qualified name; their types can be stored; dropping
# one a branch still uses is refused; and replacing one rebinds every session.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('extensions');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres',shift) }
sub fails {
 my ($query,$error,$name,$db)=@_;
 my ($rc,$out,$err)=$node->psql($db // 'postgres',$query);
 like($err,$error,$name);
}
plan skip_all=>'pgcrypto and hstore are not installed'
  unless sql(q{SELECT count(*) FROM pg_available_extensions WHERE name IN ('pgcrypto', 'hstore')}) eq '2';
sql(q{CREATE TABLE anchor(id int PRIMARY KEY); SELECT dolt_commit('-Am', 'base')});

# An extension's script makes server objects of every kind, which no branch versions.
sql(q{CREATE EXTENSION server_test_ext});
is(sql(q{SELECT count(*) FROM dolt_status}),'0','creating an extension changes no branch');
is(sql(q{SELECT string_agg(c.relname || ':' || coalesce(a.amname, '-'), ',' ORDER BY c.relname) FROM pg_class c LEFT JOIN pg_am a ON a.oid = c.relam WHERE c.relname IN ('xt_config', 'xt_copy', 'xt_matview')}),
  'xt_config:heap,xt_copy:heap,xt_matview:heap','its tables are heap tables, CREATE TABLE AS and materialized views too');
is(sql(q{SELECT k || v FROM xt_copy}),'greetinghello','CREATE TABLE AS in a script fills a server table');
is(sql(q{SELECT doubled, xt_next(1), (ROW(1, 2)::xt_pair).b FROM xt_view}),'10|2|2','its SQL and PL/pgSQL functions, composite type and view work');
is(sql(q{CREATE TYPE branch_pair AS (a int); ALTER TYPE branch_pair ADD ATTRIBUTE b int;
CREATE FUNCTION branch_f() RETURNS int LANGUAGE sql AS 'SELECT 1'; ALTER FUNCTION branch_f() STABLE;
SELECT (ROW(1, 2)::branch_pair).b, (ROW(3, 4)::xt_pair).b, branch_f()}),'2|4|1','type and routine DDL on a branch leave an extension\'s types and functions alone');
sql(q{DROP FUNCTION branch_f(); DROP TYPE branch_pair; SELECT dolt_branch('other')});
is(sql(q{SELECT dolt_checkout('other'); SELECT count(*) FROM pg_proc WHERE proname IN ('xt_double', 'xt_next')}) =~ s/.*\n//sr,'2','every branch lists them');

# An extension installed into a branch's schema is in that schema wherever the branch has it.
sql(q{CREATE DATABASE ext2});
$node->safe_psql('ext2',q{CREATE TABLE anchor(id int PRIMARY KEY); CREATE SCHEMA extschema; SELECT dolt_commit('-Am', 'schema')});
$node->safe_psql('ext2',q{CREATE EXTENSION server_test_ext SCHEMA extschema});
is($node->safe_psql('ext2',q{SELECT extschema.xt_double(2), (SELECT doubled FROM extschema.xt_view)}),'4|10','an extension can be installed into a schema a branch created');
$node->safe_psql('ext2',q{SELECT dolt_branch('noext')});
is($node->safe_psql('ext2',q{SELECT dolt_checkout('noext'); DROP SCHEMA extschema; SELECT count(*) FROM pg_proc WHERE proname = 'xt_double'}) =~ s/.*\n//sr,'0','a branch without that schema does not list its objects');
fails(q{SELECT dolt_checkout('noext'); SELECT extschema.xt_double(2)},qr/schema "extschema" does not exist/,'nor finds them by name','ext2');
is($node->safe_psql('ext2',q{SELECT extschema.xt_double(3)}),'6','the branch that has the schema still does');

# Replacing an extension rebinds a session's catalogs, relations and plans.
my $warm=$node->background_psql('ext2');
$warm->query_safe(q{PREPARE doubled(int) AS SELECT extschema.xt_double($1); EXECUTE doubled(1)});
$node->safe_psql('ext2',q{DROP EXTENSION server_test_ext; CREATE EXTENSION server_test_ext SCHEMA extschema});
is($warm->query_safe(q{EXECUTE doubled(5)}),'10','a prepared plan rebinds after the extension is replaced');
is($warm->query_safe(q{SELECT count(*) FROM pg_proc WHERE proname = 'xt_double'}),'1','and the catalogs list it once');
$warm->quit;

# Versioned definitions use an extension's functions, operators and types by qualified name.
sql(q{CREATE EXTENSION pgcrypto; CREATE EXTENSION hstore});
sql(q{CREATE TABLE secrets(id int PRIMARY KEY, pw text NOT NULL DEFAULT crypt('default', gen_salt('md5')),
  sha bytea GENERATED ALWAYS AS (digest(pw, 'sha256')) STORED, doubled int DEFAULT xt_double(21), CHECK (length(pw) > 10));
CREATE TABLE attrs(id int PRIMARY KEY, h hstore CHECK (h ? 'k'));
INSERT INTO secrets(id) VALUES (1); INSERT INTO attrs VALUES (1, 'k=>1, a=>2')});
is(sql(q{SELECT pw = crypt('default', pw), sha = digest(pw, 'sha256'), doubled FROM secrets}),'t|t|42','defaults and generated columns call extension functions');
fails(q{INSERT INTO secrets(id, pw) VALUES (2, 'short')},qr/violates check constraint "secrets_pw_check"/,'a check constraint calls one');
fails(q{INSERT INTO attrs VALUES (2, 'a=>1')},qr/violates check constraint "attrs_h_check"/,'a check constraint uses an extension operator');
is(sql(q{SELECT h -> 'a' FROM attrs}),'2','an extension type is stored and read back');
sql(q{ALTER TABLE secrets ADD COLUMN salt text DEFAULT gen_salt('md5'); SELECT dolt_commit('-Am', 'extension uses')});
is(sql(q{SELECT length(salt) FROM secrets}),'11','ALTER TABLE adds a column with an extension default');
# A database root lists, by name, the extensions whose objects its definitions use.
sub root_extensions {
 my $ref=shift;
 return sql(qq{SELECT dolt_root_json(r.reftarget)::jsonb->'extensions' FROM pg_dolt_ref r WHERE r.refname = '$ref'});
}
is(root_extensions('working/main'),'["hstore", "pgcrypto", "server_test_ext"]','a root lists the extensions its definitions use');
is(root_extensions('working/other'),'','and a root whose definitions use none lists none');
sql(q{SELECT dolt_branch('feature'); SELECT dolt_checkout('feature'); INSERT INTO secrets(id) VALUES (3); UPDATE attrs SET h = h || 'b=>3'; SELECT dolt_commit('-Am', 'feature')});
is(sql(q{SELECT dolt_merge('feature')}) =~ /merge successful|Fast-forward|fast-forward/ ? 'merged' : 'not merged','merged','branches with extension uses merge');
is(sql(q{SELECT count(*), sum(doubled) FROM secrets}),'2|84','the merged rows took their defaults');
is(sql(q{SELECT to_h -> 'b', from_h -> 'b' IS NULL FROM dolt_diff('HEAD~1', 'HEAD', 'attrs')}),'3|t','a diff shows extension type values');

# Dropping an extension a branch still uses is refused, on any branch.
fails(q{DROP EXTENSION pgcrypto},qr/cannot drop extension pgcrypto because other objects depend on it.*table public\.secrets on branch "(main|feature)"/s,'an extension a branch uses cannot be dropped');
fails(q{DROP EXTENSION pgcrypto CASCADE},qr/cannot drop extension pgcrypto because other objects depend on it/,'not even with CASCADE');
fails(q{DROP EXTENSION hstore},qr/cannot drop extension hstore because other objects depend on it/,'nor one whose type or operator a branch uses');
sql(q{SELECT dolt_branch('keeper')});
sql(q{DROP TABLE secrets; SELECT dolt_commit('-Am', 'no secrets on main')});
fails(q{DROP EXTENSION pgcrypto},qr/table public\.secrets on branch "(keeper|feature)"/,'another branch\'s use keeps it');
for my $branch ('keeper', 'feature') {
 sql(qq{SELECT dolt_checkout('$branch'); DROP TABLE secrets; SELECT dolt_commit('-Am', 'no secrets')});
}
sql(q{DROP EXTENSION pgcrypto});
is(sql(q{SELECT count(*) FROM pg_extension WHERE extname = 'pgcrypto'}),'0','once no branch uses it, it can be dropped');
is(root_extensions('working/main'),'["hstore"]','the root lists only the extensions still used');
fails(q{SELECT count(*) FROM secrets AS OF 'HEAD~1'},qr/this revision of the database requires extension "pgcrypto", which is not installed/,'history that used it needs it to be read');
fails(q{SELECT dolt_checkout('-b', 'old', 'HEAD~1')},qr/requires extension "pgcrypto"/,'nor can it be checked out');
is(sql(q{SELECT count(*) FROM dolt_branches WHERE name = 'old'}),'0','and the checkout leaves no branch');
sql(q{CREATE EXTENSION pgcrypto});
is(sql(q{SELECT count(*) FROM secrets AS OF 'HEAD~1'}),'2','installing it again makes that history readable');
is(sql(q{SELECT count(*) FROM attrs}),'1','other branches\' tables are unaffected');

# PostGIS, where it is installed: geometry and geography values in versioned tables.
SKIP:
{
 skip 'postgis is not installed', 4 unless sql(q{SELECT count(*) FROM pg_available_extensions WHERE name = 'postgis'}) eq '1';
 sql(q{CREATE EXTENSION postgis});
 sql(q{CREATE TABLE places(id int PRIMARY KEY, geom geometry(Point, 4326), area geography);
 INSERT INTO places VALUES (1, 'SRID=4326;POINT(-122.4 37.8)', 'POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))'); SELECT dolt_commit('-Am', 'places')});
 is(sql(q{SELECT ST_AsEWKT(geom), round(ST_Area(area)) FROM places}),'SRID=4326;POINT(-122.4 37.8)|12308778361','geometry and geography are stored and read back');
 fails(q{INSERT INTO places VALUES (2, 'SRID=3857;POINT(0 0)', NULL)},qr/does not match column SRID/,'the column typmod applies');
 is(sql(q{SELECT f_table_name, srid, type FROM geometry_columns}),'places|4326|POINT','geometry_columns lists a versioned table');
 is(sql(q{SELECT dolt_checkout('-b', 'moved'); UPDATE places SET geom = ST_Translate(geom, 1, 1); SELECT dolt_commit('-Am', 'moved'); SELECT ST_AsText(geom) FROM places AS OF 'HEAD~1'}) =~ s/.*\n//sr,'POINT(-122.4 37.8)','history keeps geometry values');
}
$node->stop;
done_testing();
