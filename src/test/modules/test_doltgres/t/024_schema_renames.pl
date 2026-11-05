# Renames of versioned objects. Definitions name the objects they use, so a
# rename rewrites exactly the definitions that the database's dependency
# record lists as referring to the renamed object.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('schema_renames');
$node->init(extra=>['--encoding=UTF8','--locale=C']);
$node->append_conf('postgresql.conf',"default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql {return $node->safe_psql('postgres',$_[0]);}

sql(q{
CREATE TABLE t(id int PRIMARY KEY, v int CONSTRAINT vpos CHECK (v > 0));
CREATE INDEX t_v ON t(v);
INSERT INTO t VALUES (1,10);
CREATE TABLE c(id int PRIMARY KEY, t_id int REFERENCES t(id));
INSERT INTO c VALUES (5,1);
CREATE FUNCTION doubled() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.v := NEW.v * 2; RETURN NEW; END$$;
CREATE TRIGGER t_doubled BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION doubled();
CREATE FUNCTION value_of(r t) RETURNS int LANGUAGE sql AS $$SELECT r.v$$;
CREATE FUNCTION f(x int) RETURNS int LANGUAGE sql IMMUTABLE AS $$SELECT x$$;
CREATE TABLE u(id int PRIMARY KEY, w int DEFAULT f(1), CHECK (f(w) > 0));
CREATE TYPE mood AS ENUM ('sad','ok');
CREATE DOMAIN upbeat AS mood CHECK (VALUE <> 'sad');
CREATE TYPE pair AS (a int, m mood);
CREATE TABLE m(id int PRIMARY KEY, mo mood DEFAULT 'ok', ms mood[], d upbeat, p pair);
INSERT INTO m VALUES (1,'sad','{ok,sad}','ok',ROW(1,'ok')), (2,'ok',NULL,NULL,NULL);
CREATE INDEX m_mo ON m(mo);
CREATE FUNCTION cheer(x mood) RETURNS mood LANGUAGE sql AS $$SELECT x$$;
SELECT dolt_commit('-Am','base');
});
like(sql(q{SELECT dolt_root_json(reftarget)::jsonb->'dependents'->>'routine:public.f' FROM pg_dolt_ref WHERE refname='working/main'}),
	qr/"table", "public.u"/,'the dependency record lists a routine\'s users');

# ---- routines ----
sql(q{ALTER FUNCTION f(int) RENAME TO g});
is(sql(q{SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef WHERE adrelid='u'::regclass}),'g(1)','renaming a routine rewrites the defaults that call it');
is(sql(q{SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid='u'::regclass AND contype='c'}),'CHECK ((g(w) > 0))','and the checks');
sql(q{CREATE SCHEMA other; ALTER FUNCTION g(int) SET SCHEMA other; INSERT INTO u(id) VALUES (1)});
is(sql(q{SELECT pg_get_expr(adbin, adrelid)||','||(SELECT w FROM u) FROM pg_attrdef WHERE adrelid='u'::regclass}),'other.g(1),1','SET SCHEMA of a routine');

# ---- tables, columns, indexes, constraints and triggers ----
sql(q{ALTER TABLE t RENAME TO t2; INSERT INTO t2 VALUES (2,3)});
is(sql(q{SELECT string_agg(id||':'||v, ',' ORDER BY id) FROM t2}),'1:10,2:6','a renamed table keeps its rows and trigger');
is(sql(q{SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid='c'::regclass AND contype='f'}),'FOREIGN KEY (t_id) REFERENCES t2(id)','a foreign key follows the table it references');
is(sql(q{SELECT value_of(t2) FROM t2 WHERE id=1}),'10','a routine taking the row type follows it');
my ($code,$out,$err)=$node->psql('postgres',q{INSERT INTO c VALUES (6,99)});
like($err,qr/is not present in table "t2"/,'the foreign key still checks the renamed table');
# As on heap, a PL/pgSQL body names columns by name.
sql(q{ALTER TABLE t2 RENAME COLUMN v TO val; CREATE OR REPLACE FUNCTION doubled() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.val := NEW.val * 2; RETURN NEW; END$$; ALTER INDEX t_v RENAME TO t2_val; ALTER TABLE t2 RENAME CONSTRAINT vpos TO val_positive; ALTER TRIGGER t_doubled ON t2 RENAME TO t2_doubled});
is(sql(q{SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes WHERE tablename='t2'}),'t2_val,t_pkey','index rename');
is(sql(q{SET enable_seqscan=off; SELECT id FROM t2 WHERE val=6}),'2','column rename with its index');
($code,$out,$err)=$node->psql('postgres',q{INSERT INTO t2 VALUES (3,-1)});
like($err,qr/violates check constraint "val_positive"/,'constraint rename');
is(sql(q{SELECT tgname FROM pg_trigger WHERE NOT tgisinternal}),'t2_doubled','trigger rename');
sql(q{ALTER TABLE t2 SET SCHEMA other; INSERT INTO other.t2 VALUES (4,4)});
is(sql(q{SELECT val||','||(SELECT tgrelid::regclass FROM pg_trigger WHERE NOT tgisinternal) FROM other.t2 WHERE id=4}),'8,other.t2','SET SCHEMA of a table with its trigger');

# ---- types ----
sql(q{ALTER TYPE mood RENAME TO feeling; ALTER DOMAIN upbeat RENAME TO cheerful; ALTER TYPE pair RENAME ATTRIBUTE m TO f});
is(sql(q{SELECT string_agg(format_type(atttypid, atttypmod), ',' ORDER BY attnum) FROM pg_attribute WHERE attrelid='m'::regclass AND attnum > 0}),'integer,feeling,feeling[],cheerful,pair','renaming types rewrites the columns that use them');
is(sql(q{SELECT pg_get_function_arguments('cheer'::regproc)||','||format_type((SELECT typbasetype FROM pg_type WHERE typname='cheerful'), NULL)}),'x feeling,feeling','and the routines and domains');
sql(q{INSERT INTO m(id) VALUES (3)});
is(sql(q{SELECT mo FROM m WHERE id=3}),'ok','and the defaults that cast to them');
is(sql(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT string_agg(id::text, ',' ORDER BY id) FROM m WHERE mo = 'ok'}),'2,3','and the indexes on columns of them');

# ---- enum labels ----
sql(q{ALTER TYPE feeling RENAME VALUE 'ok' TO 'fine'});
is(sql(q{SELECT string_agg(id||':'||mo||':'||coalesce(ms::text,'-')||':'||coalesce(d::text,'-')||':'||coalesce((p).f::text,'-'), ' ' ORDER BY id) FROM m}),
	'1:sad:{fine,sad}:fine:fine 2:fine:-:-:- 3:fine:-:-:-','RENAME VALUE rewrites stored labels in columns, arrays, domains and composites');
is(sql(q{SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef WHERE adrelid='m'::regclass}),"'fine'::feeling",'and the defaults that name it');
is(sql(q{SELECT string_agg(mo::text, ',' ORDER BY id) FROM m AS OF 'HEAD'}),'sad,ok','history keeps the old labels');
is(sql(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT string_agg(id::text, ',' ORDER BY id) FROM m WHERE mo = 'fine'}),'2,3','a renamed label keeps its index keys');

# ---- schemas ----
sql(q{
CREATE SCHEMA app;
CREATE TYPE app.level AS ENUM ('low','high');
CREATE TABLE app.items(id int PRIMARY KEY, lv app.level DEFAULT 'low');
CREATE INDEX items_lv ON app.items(lv);
INSERT INTO app.items VALUES (1,'high');
CREATE FUNCTION app.twice(x int) RETURNS int LANGUAGE sql IMMUTABLE AS $$SELECT x * 2$$;
CREATE TABLE app.refs(id int PRIMARY KEY, item int REFERENCES app.items(id));
CREATE FUNCTION app.mark() RETURNS trigger LANGUAGE plpgsql AS $$BEGIN NEW.id := NEW.id + 100; RETURN NEW; END$$;
CREATE TRIGGER items_mark BEFORE INSERT ON app.items FOR EACH ROW EXECUTE FUNCTION app.mark();
CREATE TABLE outside(id int PRIMARY KEY, w int DEFAULT app.twice(2), lv app.level);
});
sql(q{COMMENT ON SCHEMA app IS 'the application'; ALTER SCHEMA app RENAME TO application; INSERT INTO application.items(id) VALUES (2); INSERT INTO outside(id) VALUES (1)});
is(sql(q{SELECT obj_description(oid, 'pg_namespace') FROM pg_namespace WHERE nspname='application'}),'the application','a renamed schema keeps its comment');
is(sql(q{SELECT string_agg(id||':'||lv, ',' ORDER BY id) FROM application.items}),'1:high,102:low','a renamed schema keeps its tables, rows, defaults and triggers');
is(sql(q{SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conrelid='application.refs'::regclass AND contype='f'}),'FOREIGN KEY (item) REFERENCES application.items(id)','foreign keys within the schema follow it');
is(sql(q{SELECT pg_get_expr(adbin, adrelid)||','||format_type((SELECT atttypid FROM pg_attribute WHERE attrelid='outside'::regclass AND attname='lv'), NULL) FROM pg_attrdef WHERE adrelid='outside'::regclass}),'application.twice(2),application.level','objects outside the schema that refer into it follow it');
is(sql(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT id FROM application.items WHERE lv = 'high'}),'1','indexes in a renamed schema');
is(sql(q{SELECT count(*) FROM pg_namespace WHERE nspname='app'}),'0','the branch no longer has the old schema');

# ---- branches ----
sql(q{SELECT dolt_commit('-Am','renames')});
is(sql(q{SELECT count(*) FROM t AS OF 'HEAD~1'}),'1','history keeps the old table name');
is(sql(q{SELECT count(*) FROM other.t2 AS OF 'HEAD'}),'3','the renamed table is committed with its rows');
is(sql(q{SELECT count(*) FROM application.items AS OF 'HEAD'}),'2','a renamed schema is committed');
$node->stop;
done_testing();
