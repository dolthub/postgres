# User-defined types made in a versioned database are versioned schema
# members: enums, domains and composite types, branched and merged like the
# rest of the schema.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('versioned_types');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}
sub fails {my($q,$re,$label)=@_;my($code,$out,$err)=$node->psql('postgres',$q);isnt($code,0,$label);like($err,$re,"$label diagnostic");}
my $index='SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_sort=off; ';
sql(q{CREATE TABLE seed(id int PRIMARY KEY)});

# ---- enums ----
sql(q{CREATE TYPE mood AS ENUM ('sad','ok','happy')});
sql(q{CREATE TABLE e(id int PRIMARY KEY, m mood DEFAULT 'ok', ms mood[]); INSERT INTO e VALUES (1,'happy','{sad,ok}'),(2,'sad',NULL); INSERT INTO e(id) VALUES (3)});
is(sql(q{SELECT string_agg(id||':'||m, ',' ORDER BY m) FROM e}),'2:sad,3:ok,1:happy','enum columns sort in label order');
is(sql(q{SELECT ms[2] FROM e WHERE id=1}),'ok','enum arrays');
sql(q{ALTER TYPE mood ADD VALUE 'meh' BEFORE 'ok'; ALTER TYPE mood ADD VALUE 'glad' AFTER 'happy'});
is(sql(q{SELECT enum_range(NULL::mood)}),'{sad,meh,ok,happy,glad}','ADD VALUE');
is(sql(q{SELECT m FROM e WHERE id=1}),'happy','stored labels read back after ADD VALUE');
sql(q{ALTER TYPE mood RENAME VALUE 'happy' TO 'joyful'});
is(sql(q{SELECT m FROM e WHERE id=1}),'joyful','RENAME VALUE rewrites stored labels');
sql(q{ALTER TYPE mood RENAME VALUE 'joyful' TO 'happy'});
is(sql(q{SELECT 'meh'::mood < 'ok'::mood AND 'meh'::mood > 'sad'::mood}),'t','added label sorts in place');
fails(q{ALTER TYPE mood ADD VALUE 'ok'},qr/enum label "ok" already exists/,'native enum validation');
sql(q{CREATE TABLE ei(id int PRIMARY KEY, m mood); CREATE INDEX ei_m ON ei(m); INSERT INTO ei VALUES (1,'happy'),(2,'sad'),(3,'ok')});
is(sql($index.q{SELECT string_agg(m::text, ',' ORDER BY m) FROM ei}),'sad,ok,happy','enum index keys follow label order');
sql(q{ALTER TYPE mood ADD VALUE 'fine' BEFORE 'ok'; INSERT INTO ei VALUES (4,'fine')});
is(sql($index.q{SELECT string_agg(id::text, ',' ORDER BY m) FROM ei WHERE m < 'ok'}),'2,4','an added label rebuilds the positions index keys hold');
is(sql($index.q{SELECT id FROM ei WHERE m = 'ok'}),'3','index equality after an added label');
sql(q{ALTER TYPE mood RENAME VALUE 'happy' TO 'joyful'});
is(sql($index.q{SELECT id FROM ei WHERE m = 'joyful'}),'1','a renamed label keeps its index keys');
sql(q{ALTER TYPE mood RENAME VALUE 'joyful' TO 'happy'});

# ---- domains ----
sql(q{CREATE DOMAIN posint AS int NOT NULL DEFAULT 1 CHECK (VALUE > 0); CREATE DOMAIN upbeat AS mood CHECK (VALUE <> 'sad')});
sql(q{CREATE TABLE d(id int PRIMARY KEY, p posint, u upbeat); INSERT INTO d(id, u) VALUES (1,'ok')});
is(sql(q{SELECT p||','||u FROM d}),'1,ok','domain default');
fails(q{INSERT INTO d VALUES (2,-5,'ok')},qr/value for domain posint violates check constraint "posint_check"/,'domain CHECK');
fails(q{INSERT INTO d VALUES (2,5,'sad')},qr/value for domain upbeat violates check constraint "upbeat_check"/,'domain over an enum');
fails(q{ALTER DOMAIN posint ADD CONSTRAINT small CHECK (VALUE < 1)},qr/column "p" of table "d" contains values that violate the new constraint/,'new domain constraint validates existing rows');
sql(q{ALTER DOMAIN posint ADD CONSTRAINT small CHECK (VALUE < 100); ALTER DOMAIN posint SET DEFAULT 7});
fails(q{INSERT INTO d VALUES (3,500,'ok')},qr/violates check constraint "small"/,'added domain constraint');
sql(q{INSERT INTO d(id, u) VALUES (4,'ok')});
is(sql(q{SELECT p FROM d WHERE id=4}),'7','changed domain default');
is(sql(q{SELECT string_agg(pg_get_constraintdef(oid), ' / ' ORDER BY conname) FROM pg_constraint WHERE contypid='posint'::regtype}),'CHECK ((VALUE > 0)) / NOT NULL / CHECK ((VALUE < 100))','domain constraints deparse as native');
sql(q{CREATE TYPE fieldpair AS (r float8, i float8); CREATE DOMAIN positive_pair AS fieldpair CHECK ((VALUE).r > 0)});
fails(q{ALTER TYPE fieldpair ALTER ATTRIBUTE r TYPE varchar},qr/operator does not exist: character varying > double precision/,'a type change a domain check cannot use is refused');
is(sql(q{ALTER TYPE fieldpair ALTER ATTRIBUTE r TYPE bigint; SELECT (ROW(1, 2)::positive_pair).r}),'1','one it can use is made, and the branch stays readable');

# ---- composite types ----
sql(q{CREATE TYPE pair AS (a int, b mood); CREATE TABLE c(id int PRIMARY KEY, p pair, ps pair[]); INSERT INTO c VALUES (1, ROW(1,'glad'), ARRAY[ROW(2,'ok')::pair])});
is(sql(q{SELECT (p).b||','||ps[1].a FROM c}),'glad,2','composite columns and arrays');
sql(q{ALTER TYPE pair RENAME ATTRIBUTE a TO x});
is(sql(q{SELECT (p).x FROM c}),'1','renamed attribute');
sql(q{CREATE TYPE spare AS ENUM ('a'); ALTER TYPE spare RENAME TO extra});
is(sql(q{SELECT enum_range(NULL::extra)}),'{a}','an unused type renames');
sql(q{ALTER TYPE mood RENAME TO feeling; ALTER TYPE feeling RENAME TO mood});
is(sql(q{SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid='e'::regclass AND attname='m'}),'mood','renaming a used type rewrites its users');
sql(q{CREATE TYPE wrap AS (inner_p pair, n int); CREATE TABLE w(id int PRIMARY KEY, v wrap); INSERT INTO w VALUES (1, ROW(ROW(5,'ok')::pair, 9))});
sql(q{ALTER TYPE pair ADD ATTRIBUTE z int});
is(sql(q{SELECT p::text||ps::text||(SELECT v::text FROM w) FROM c}),'(1,glad,){"(2,ok,)"}("(5,ok,)",9)','ADD ATTRIBUTE extends stored values, arrays and nested values');
sql(q{INSERT INTO c VALUES (2, ROW(3,'sad',4), NULL); ALTER TYPE pair DROP ATTRIBUTE b});
is(sql(q{SELECT string_agg(p::text, ',' ORDER BY id) FROM c}).sql(q{SELECT v::text FROM w}),'(1,),(3,4)("(5,)",9)','DROP ATTRIBUTE removes stored fields');
fails(q{ALTER TYPE pair ALTER ATTRIBUTE x TYPE bigint},qr/cannot alter type "pair" because column "c\.(p|ps)" uses it/,'ALTER ATTRIBUTE TYPE of a used type is rejected, as native');
sql(q{DROP TABLE w; DROP TYPE wrap; CREATE TYPE loose AS (a int); ALTER TYPE loose ALTER ATTRIBUTE a TYPE text, ADD ATTRIBUTE b int});
is(sql(q{SELECT string_agg(attname||':'||format_type(atttypid, NULL), ',' ORDER BY attnum) FROM pg_attribute WHERE attrelid=(SELECT typrelid FROM pg_type WHERE typname='loose') AND attnum>0}),'a:text,b:integer','ALTER ATTRIBUTE TYPE of an unused type');
sql(q{COMMENT ON TYPE loose IS 'loose ends'; COMMENT ON DOMAIN upbeat IS 'cheerful'});
is(sql(q{SELECT obj_description('loose'::regtype, 'pg_type')||','||obj_description('upbeat'::regtype, 'pg_type')}),'loose ends,cheerful','COMMENT ON TYPE and DOMAIN');

# ---- range types ----
sql(q{CREATE TYPE span AS RANGE (subtype = float8, subtype_diff = float8mi); CREATE TYPE moodrange AS RANGE (subtype = mood, multirange_type_name = moodspans);
CREATE TABLE spans(id int PRIMARY KEY, s span, ms span_multirange, m moodrange); INSERT INTO spans VALUES (1, '[1,2)', '{[1,2),[5,6)}', '[sad,ok]')});
is(sql(q{SELECT s::text||ms::text||m::text FROM spans WHERE s @> 1.5::float8 AND m @> 'meh'::mood}),'[1,2){[1,2),[5,6)}[sad,ok]','range and multirange columns of versioned range types');
is(sql(q{SELECT span(1, 3, '[]')::text||span_multirange()::text||span_multirange(span(1,2), span(3,4))::text||(span(1,2)::span_multirange)::text||moodspans(moodrange('sad','ok'))::text}),
   '[1,3]{}{[1,2),[3,4)}{[1,2)}{[sad,ok)}','constructors and the multirange cast');
is(sql(q{SELECT string_agg(typname||':'||typtype::text, ',' ORDER BY typname) FROM pg_type WHERE typname IN ('span','_span','span_multirange','_span_multirange')}),'_span:b,_span_multirange:b,span:r,span_multirange:m','a range type and what it derives');
sql(q{ALTER TYPE span RENAME TO interval_span});
is(sql(q{SELECT pg_typeof(span(1,2))::text||','||(SELECT s FROM spans)}),'interval_span,[1,2)','a renamed range type keeps its constructors\' names, as native');
fails(q{DROP TYPE interval_span},qr/column s of table spans depends on type interval_span/,'DROP TYPE is blocked by range columns');
sql(q{DROP TYPE interval_span CASCADE});
is(sql(q{SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute WHERE attrelid='spans'::regclass AND attnum>0}).':'.sql(q{SELECT count(*) FROM pg_proc WHERE proname IN ('span','span_multirange')}),
   'id,m:0','DROP TYPE CASCADE drops a range type\'s columns and constructors');
sql(q{CREATE FUNCTION fdiff(a float8, b float8) RETURNS float8 LANGUAGE sql IMMUTABLE AS 'SELECT a - b'});
fails(q{CREATE TYPE odd AS RANGE (subtype = float8, subtype_diff = fdiff)},qr/versioned range types with a user-defined subtype_diff function/,'a user-defined subtype_diff is not yet supported');

# ---- routines and dependencies ----
sql(q{CREATE FUNCTION cheer(f mood) RETURNS mood LANGUAGE sql AS $$SELECT 'glad'::mood$$});
is(sql(q{SELECT cheer('sad')}),'glad','routines take and return versioned types');
fails(q{DROP TYPE pair},qr/column p of table c depends on type pair/,'DROP TYPE is blocked by columns');
fails(q{CREATE TABLE h(id int, f mood) USING heap},qr/cannot make an unversioned object depend on a versioned one/,'a heap table cannot use a versioned type');

# ---- branches and merge ----
sql(q{SELECT dolt_commit('-Am','types'); SELECT dolt_branch('other')});
$node->safe_psql('postgres',q{SELECT dolt_checkout('other'); CREATE TYPE color AS ENUM ('red','blue'); CREATE TYPE numspan AS RANGE (subtype = numeric); ALTER TYPE mood ADD VALUE 'calm'; CREATE TABLE paint(id int PRIMARY KEY, c color, r numspan); INSERT INTO paint VALUES (1,'blue','[1,2]'); SELECT dolt_commit('-Am','other types')});
is(sql(q{SELECT count(*) FROM pg_type WHERE typname='color'}),'0','a type made on another branch is not visible');
is(sql(q{SELECT string_agg(e.enumlabel, ',' ORDER BY e.enumsortorder) FROM "postgres/other".pg_catalog.pg_enum e JOIN "postgres/other".pg_catalog.pg_type t ON t.oid=e.enumtypid WHERE t.typname='mood'}),'sad,meh,fine,ok,happy,glad,calm','another branch\'s labels are readable');
is(sql(q{SELECT enum_range(NULL::mood)}),'{sad,meh,fine,ok,happy,glad}','this branch keeps its own labels');
is(sql(q{SELECT fast_forward FROM dolt_merge('other')}),'1','a type change merges');
is(sql(q{SELECT enum_range(NULL::mood)::text||','||(SELECT c::text||r::text FROM paint)}),'{sad,meh,fine,ok,happy,glad,calm},blue[1,2]','merged types and their rows');
$node->safe_psql('postgres',q{SELECT dolt_branch('b1'); SELECT dolt_branch('b2'); SELECT dolt_checkout('b1'); ALTER TYPE mood ADD VALUE 'x1'; SELECT dolt_commit('-Am','b1'); SELECT dolt_checkout('b2'); ALTER TYPE mood ADD VALUE 'x2'; SELECT dolt_commit('-Am','b2')});
sql(q{SELECT dolt_merge('b1')});
is(sql(q{SELECT conflicts FROM dolt_merge('b2')}).':'.sql(q{SELECT table_name||': '||description FROM dolt_schema_conflicts}),'1:public.mood: both branches changed type public.mood','a type changed on both branches conflicts');

# ---- drops ----
sql(q{DROP TABLE c, d; DROP TYPE pair; DROP DOMAIN upbeat; DROP TYPE loose});
is(sql(q{SELECT count(*) FROM pg_type WHERE typname IN ('pair','_pair','upbeat','loose')}),'0','DROP TYPE and DROP DOMAIN');
is(sql(q{SELECT count(*) FROM pg_type AS OF 'HEAD' WHERE typname='pair'}),'1','history keeps dropped types');
$node->stop;
done_testing();
