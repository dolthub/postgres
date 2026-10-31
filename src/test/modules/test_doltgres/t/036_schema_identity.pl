# A namespace has one OID, the same on every branch, and public is always
# 2200. Server objects (extensions' members, heap tables) belong to the
# server, as built-in objects do: every branch's catalogs list them in their
# namespace, whatever path reads them. A branch may drop or rename any
# namespace; that stays local to it and never touches server objects.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('schema_identity');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres',shift) }
plan skip_all=>'pgcrypto and hstore are not installed'
  unless sql(q{SELECT count(*) FROM pg_available_extensions WHERE name IN ('pgcrypto', 'hstore')}) eq '2';
sub fails {
 my ($query,$error,$name)=@_;
 my ($rc,$out,$err)=$node->psql('postgres',$query);
 like($err,$error,$name);
}
# The same query run with each join method in turn: answers must not depend on the plan.
sub every_join {
 my ($query,$expected,$name)=@_;
 for my $method ('nestloop','hashjoin','mergejoin') {
  my $off=join(' ',map {"SET enable_$_=off;"} grep {$_ ne $method} ('nestloop','hashjoin','mergejoin'));
  is(sql("$off $query"),$expected,"$name ($method)");
 }
}

sql(q{CREATE EXTENSION pgcrypto; CREATE EXTENSION hstore;
CREATE SCHEMA app;
CREATE TABLE public.t(id int PRIMARY KEY, h hstore);
CREATE TABLE app.u(id int PRIMARY KEY, v text);
CREATE TABLE public.heap_t(id int) USING heap;
CREATE FUNCTION public.f() RETURNS int LANGUAGE sql AS $$ SELECT 1 $$;
INSERT INTO public.t VALUES (1, 'a=>1'); INSERT INTO app.u VALUES (1, 'main');
SELECT dolt_commit('-Am', 'base')});

# One OID per namespace.
is(sql(q{SELECT 'public'::regnamespace::oid}),'2200','public is 2200 on a branch');
is(sql(q{SELECT 'public'::regnamespace::oid FROM (SELECT 1) s, "postgres/main".public.t LIMIT 1}),'2200','and in a branch-qualified query');
my $app=sql(q{SELECT oid FROM pg_namespace WHERE nspname = 'app'});
sql(q{SELECT dolt_branch('other')});
is(sql(q{SELECT oid FROM "postgres/other".pg_catalog.pg_namespace WHERE nspname = 'app'}),$app,'a namespace has the same OID on every branch');
$node->restart;
is(sql(q{SELECT oid FROM pg_namespace WHERE nspname = 'app'}),$app,'and after a restart');
is(sql(q{SELECT oid FROM pg_namespace WHERE nspname = 'app'}),sql(q{SELECT 'app'::regnamespace::oid}),'regnamespace agrees with pg_namespace');

# Every path finds each kind of object in its namespace.
my %objects=(
 'versioned table'=>[q{SELECT c.relname, n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname IN ('t', 'u') ORDER BY 1},"t|public\nu|app"],
 'heap table'=>[q{SELECT c.relname, n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relname = 'heap_t'},'heap_t|public'],
 'versioned routine'=>[q{SELECT p.proname, n.nspname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.proname = 'f'},'f|public'],
 'extension function'=>[q{SELECT p.proname, n.nspname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.proname = 'crypt'},'crypt|public'],
 'extension type'=>[q{SELECT t.typname, n.nspname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace WHERE t.typname = 'hstore'},'hstore|public'],
 'extension operator'=>[q{SELECT count(*), min(n.nspname) FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace WHERE o.oprname = '->' AND o.oprleft = 'hstore'::regtype},'2|public'],
);
for my $kind (sort keys %objects) {
 every_join($objects{$kind}[0],$objects{$kind}[1],"$kind joins to its namespace");
}
is(sql(q{SELECT routine_schema FROM information_schema.routines WHERE routine_name = 'crypt' GROUP BY 1}),'public','information_schema lists extension functions');
is(sql(q{SELECT string_agg(table_schema || '.' || table_name, ',' ORDER BY table_name) FROM information_schema.tables WHERE table_name IN ('t', 'u', 'heap_t')}),'public.heap_t,public.t,app.u','information_schema lists heap and versioned tables');
is(sql(q{SELECT 'heap_t'::regclass::text, 'app.u'::regclass::text, 'public.crypt(text,text)'::regprocedure::text, 'hstore'::regtype::text}),'heap_t|app.u|crypt(text,text)|hstore','reg* casts name objects in their namespace');
is(sql(q{SELECT (h -> 'a') FROM t}),'1','an extension operator applies to a versioned table');
like(sql(q{\dt}),qr/heap_t.*\n.*\bt\b/s,'psql lists heap and versioned tables');
like(sql(q{\df public.crypt}),qr/crypt/,'psql lists extension functions in public');

# A server object made after a session built its catalogs is in them at once.
is(sql(q{CREATE TABLE public.late_heap(id int) USING heap; INSERT INTO late_heap VALUES (1); SELECT count(*) FROM late_heap, t}),'1','a session sees the heap table it just made');
# Each such change rebuilds the session's catalogs; the ones it replaces are freed.
my $memory=q{SELECT sum(total_bytes) FROM pg_backend_memory_contexts};
my $churn=$node->background_psql('postgres');
$churn->query_safe(q{SELECT count(*) FROM t});
my $before=$churn->query_safe($memory);
$churn->query_safe(qq{CREATE TEMP TABLE churn_$_(x int) USING heap; SELECT count(*) FROM t}) for 1..20;
cmp_ok($churn->query_safe($memory)-$before,'<',20*1024*1024,'replaced catalogs do not accumulate in a session');
$before=$churn->query_safe($memory);
$churn->query_safe(qq{CREATE TABLE versioned_churn_$_(x int PRIMARY KEY)}) for 1..40;
cmp_ok($churn->query_safe($memory)-$before,'<',20*1024*1024,'nor do the catalogs of the roots versioned DDL replaces');
$churn->quit;

# One namespace for uniqueness, across server and branch objects.
fails(q{CREATE FUNCTION public.crypt(text, text) RETURNS text LANGUAGE sql AS $$ SELECT 'x' $$},qr/already exists/,'a versioned routine cannot duplicate an extension function');
fails(q{CREATE TABLE heap_t(id int)},qr/already exists/,'a versioned table cannot take a heap table\'s name');

# Namespaces differ between branches, and may be empty.
is(sql(q{SELECT dolt_checkout('other'); CREATE SCHEMA only_other; SELECT table_name, status FROM dolt_status}) =~ s/.*\n//sr,'only_other|new schema','an empty namespace is a change on its branch');
sql(q{SELECT dolt_checkout('other'); SELECT dolt_commit('-Am', 'empty schema')});
is(sql(q{SELECT count(*) FROM "postgres/other".pg_catalog.pg_namespace WHERE nspname = 'only_other'}),'1','an empty namespace exists on its branch');
is(sql(q{SELECT count(*) FROM pg_namespace WHERE nspname = 'only_other'}),'0','and not on another');
sql(q{CREATE DATABASE fresh});
$node->safe_psql('fresh',q{CREATE SCHEMA myschema});
is($node->safe_psql('fresh',q{SELECT table_name, staged, status FROM dolt_status}),'myschema|f|new schema','an empty namespace is a change before a repository\'s first table');
is($node->safe_psql('fresh',q{SELECT dolt_add('.')}) . $node->safe_psql('fresh',q{SELECT dolt_commit('-m', 'new schema') IS NOT NULL; SELECT message FROM dolt_log LIMIT 1}),"0t\nnew schema",'and is staged and committed');
my $literals=join(', ',map {(3221225472+$_).'::oid'} 0..63);
is($node->safe_psql('fresh',qq{CREATE TABLE t(id int PRIMARY KEY); SELECT $literals}) =~ tr/|//,63,'OID literals equal to released handles are only numbers');
my $values=join(', ',map {"($_)"} split(/, /,$literals));
is($node->safe_psql('fresh',qq{CREATE TABLE u(id int PRIMARY KEY); CREATE FUNCTION same(o oid) RETURNS oid LANGUAGE sql AS 'SELECT o';
SELECT count(*) FROM (VALUES $values) v(o) WHERE same(o) = o AND same(3221225472::oid) = 3221225472}),'64','and so are such values passed to SQL functions');

# A rename stays on its branch.
sql(q{SELECT dolt_checkout('other'); ALTER SCHEMA app RENAME TO app2; SELECT dolt_commit('-Am', 'rename')});
is(sql(q{SELECT string_agg(nspname, ',' ORDER BY nspname) FROM "postgres/other".pg_catalog.pg_namespace WHERE nspname LIKE 'app%'}),'app2','a branch sees its rename');
is(sql(q{SELECT string_agg(nspname, ',' ORDER BY nspname) FROM pg_namespace WHERE nspname LIKE 'app%'}),'app','another branch keeps the old name');
is(sql(q{SELECT m.v || '/' || o.v FROM "postgres/main".app.u m JOIN "postgres/other".app2.u o USING (id)}),'main/main','a join across branches finds each namespace\'s table');
is(sql(q{SELECT count(*) FROM "postgres/main".public.t m JOIN "postgres/other".public.t o USING (id)}),'1','a join of one name on two branches');

# Dropping public on a branch hides its server objects there, and nowhere else.
sql(q{SELECT dolt_branch('nopublic'); SELECT dolt_checkout('nopublic'); DROP SCHEMA public CASCADE; SELECT dolt_commit('-Am', 'no public')});
my $session=$node->background_psql('postgres',on_error_stop=>0);
$session->query_safe(q{SELECT dolt_checkout('nopublic')});
is($session->query_safe(q{SELECT count(*) FROM pg_proc WHERE proname = 'crypt'}),'0','a branch without public does not list its server objects');
is($session->query_safe(q{SELECT count(*) FROM pg_namespace WHERE nspname = 'public'}),'0','nor public');
$session->query(q{SELECT crypt('a', 'b')});
like($session->{stderr},qr/function crypt\(unknown, unknown\) does not exist/,'nor finds them by name');
$session->{stderr}='';
is($session->query_safe(q{SELECT count(*) FROM pg_class WHERE relname = 'heap_t'}),'0','a heap table in public is hidden there too');
$session->query_safe(q{CREATE SCHEMA public});
is($session->query_safe(q{SELECT 'public'::regnamespace::oid}),'2200','public created again is 2200');
is($session->query_safe(q{SELECT count(*) FROM pg_proc WHERE proname = 'crypt'}),'1','and holds the server objects again');
$session->query_safe(q{SELECT dolt_checkout('main')});
is($session->query_safe(q{SELECT count(*) FROM pg_proc WHERE proname = 'crypt'}),'1','main keeps its server objects');
is($session->query_safe(q{SELECT count(*) FROM heap_t}),'0','and its heap table');

# A branch keeps a schema whose server objects its other definitions use.
$session->query_safe(q{SELECT dolt_branch('uses'); SELECT dolt_checkout('uses'); CREATE TABLE app.w(id int PRIMARY KEY, h hstore)});
$session->query(q{DROP SCHEMA public CASCADE});
like($session->{stderr},qr/cannot drop schema public because other objects depend on it.*table app\.w uses server objects in schema public/s,'a branch cannot drop a schema whose server objects its definitions use');
$session->{stderr}='';
is($session->query_safe(q{SELECT count(*) FROM pg_namespace WHERE nspname = 'public'}),'1','and keeps it');

# Server objects pin their schema's name: renaming it on a branch would strand them.
$session->query(q{ALTER SCHEMA public RENAME TO mine});
like($session->{stderr},qr/cannot rename schema public because server objects belong to it/,'a schema holding server objects cannot be renamed on a branch');
$session->{stderr}='';
$session->query_safe(q{SELECT dolt_checkout('main')});
$session->quit;
is(sql(q{SELECT crypt('a', gen_salt('md5')) IS NOT NULL}),'t','server objects are untouched on main');

# Collations are server objects: one a branch's definitions use cannot be dropped.
sql(q{CREATE DATABASE coll});
$node->safe_psql('coll',q{CREATE TABLE anchor(id int PRIMARY KEY); CREATE SCHEMA ct;
CREATE COLLATION ct.c_column (LC_COLLATE = "POSIX", LC_CTYPE = "POSIX");
CREATE COLLATION ct.c_index (LC_COLLATE = "POSIX", LC_CTYPE = "POSIX");
CREATE COLLATION ct.c_default (LC_COLLATE = "POSIX", LC_CTYPE = "POSIX");
CREATE TABLE ct.t(f1 text COLLATE ct.c_column, f2 text DEFAULT ('a' COLLATE ct.c_default));
CREATE INDEX ON ct.t (f2 COLLATE ct.c_index);
CREATE TABLE public.u(f text COLLATE ct.c_column)});
for my $case (['c_column','table ct.t'],['c_index','index ct.t_f2_idx'],['c_default','table ct.t']) {
 my ($rc,$out,$err)=$node->psql('coll',"DROP COLLATION ct.$case->[0] CASCADE");
 like($err,qr/cannot drop collation ct\.$case->[0] because other objects depend on it.*\Q$case->[1]\E on branch "main"/s,"a collation a branch's $case->[1] uses cannot be dropped ($case->[0])");
}
is($node->safe_psql('coll',q{SELECT count(*) FROM pg_class WHERE relname = 't'}),'1','and every session still reads the branch');
my ($coll_rc,$coll_out,$coll_err)=$node->psql('coll',q{DROP SCHEMA ct CASCADE});
like($coll_err,qr/table public\.u uses server objects in schema ct/,'nor can the schema of a collation another schema\'s table uses');
$node->safe_psql('coll',q{DROP TABLE public.u; DROP SCHEMA ct CASCADE});
is($node->safe_psql('coll',q{SELECT count(*) FROM pg_namespace WHERE nspname = 'ct'}),'0','once nothing outside uses it, the schema drops with its tables');

# Without server objects in it, public can be renamed on a branch like any schema.
sql(q{CREATE DATABASE plain});
$node->safe_psql('plain',q{CREATE TABLE t(id int PRIMARY KEY); INSERT INTO t VALUES (1); SELECT dolt_commit('-Am', 'base'); SELECT dolt_branch('renamed')});
is($node->safe_psql('plain',q{SELECT dolt_checkout('renamed'); ALTER SCHEMA public RENAME TO mine; SELECT count(*) FROM mine.t}) =~ s/.*\n//sr,'1','public renames on a branch when it holds no server objects');
is($node->safe_psql('plain',q{SELECT dolt_checkout('renamed'); SELECT 'mine'::regnamespace::oid <> 2200, count(*) FROM pg_namespace WHERE nspname = 'public'}) =~ s/.*\n//sr,'t|0','the renamed schema is not public');
is($node->safe_psql('plain',q{SELECT 'public'::regnamespace::oid, count(*) FROM public.t}),'2200|1','and another branch keeps public');
$node->stop;
done_testing();
