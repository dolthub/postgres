use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('schema_workspace');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nautovacuum=off\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
# Schemas created before the first version-control command are changes, not
# part of the lazily initialized repository's initial commit.
sql(q{CREATE SCHEMA empty_first; SELECT dolt_add('-a'); SELECT dolt_commit('-m','empty schema')});
is(sql(q{SELECT count(*) FROM pg_namespace WHERE nspname='empty_first'}), '1',
 'an empty schema can be staged and committed before any tables exist');
sql(q{SELECT dolt_checkout('-b','before_schema','HEAD~1')});
is(sql(q{SELECT count(*) FROM pg_namespace WHERE nspname='empty_first'}), '0',
 'initial history does not silently include the new schema');
sql(q{SELECT dolt_checkout('main')});
is(sql(q{SELECT count(*) FROM pg_namespace WHERE nspname='empty_first'}), '1',
 'checkout restores the committed empty schema');
sql(q{CREATE SCHEMA empty_second; SELECT dolt_commit('-Am','second empty schema')});
sql(q{DROP SCHEMA empty_second; SELECT dolt_add('-A'); SELECT dolt_commit('-m','drop empty schema')});
is(sql(q{SELECT count(*) FROM pg_namespace WHERE nspname='empty_second'}), '0',
 'stage-all includes empty schema deletion');
sql(q{SELECT dolt_checkout('-b','with_second','HEAD~1')});
is(sql(q{SELECT count(*) FROM pg_namespace WHERE nspname='empty_second'}), '1',
 'committed empty schema survives without tables');
sql(q{SELECT dolt_checkout('main')});
sql(q{CREATE TYPE source AS (v int); CREATE TYPE target AS (v int);
 CREATE FUNCTION cv(source) RETURNS target LANGUAGE sql IMMUTABLE AS 'SELECT ROW(($1).v)::target';
 SELECT dolt_commit('-Am','base');
 CREATE AGGREGATE total(int) (SFUNC=int4pl, STYPE=int4, INITCOND='0');
 CREATE OPERATOR === (LEFTARG=int4, RIGHTARG=int4, FUNCTION=int4eq);
 CREATE CAST (source AS target) WITH FUNCTION cv(source)});
is(sql(q{SELECT count(*) FROM dolt_status WHERE NOT staged}), '3', 'status lists new aggregate operator and cast');
is(sql(q{SELECT dolt_add('public.total(integer)'); SELECT count(*) FROM dolt_status WHERE staged}), "0\n1", 'stage aggregate by signature');
is(sql(q{SELECT dolt_add('public.===(pg_catalog.int4,pg_catalog.int4)'); SELECT count(*) FROM dolt_status WHERE staged}), "0\n2", 'stage operator by portable identity');
is(sql(q{SELECT dolt_add('(public.source AS public.target)'); SELECT count(*) FROM dolt_status WHERE staged}), "0\n3", 'stage cast by portable identity');
sql(q{SELECT dolt_commit('-m','schema objects')});
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'schema object commit clears status');
is(sql(q{SELECT count(*) FROM dolt_diff_summary('HEAD~1','HEAD')}), '3', 'diff summary lists schema objects');
sql(q{DROP AGGREGATE total(int); DROP OPERATOR === (int,int); DROP CAST (source AS target)});
is(sql(q{SELECT count(*) FROM dolt_status WHERE status='deleted'}), '3', 'status reports deleted schema objects');
sql(q{SELECT dolt_checkout('public.total(integer)','public.===(pg_catalog.int4,pg_catalog.int4)','(public.source AS public.target)')});
is(sql(q{SELECT total(v), 1===1, (ROW(2)::source::target).v FROM (VALUES(1),(2)) t(v)}), '3|t|2', 'named checkout restores all schema object kinds');
$node->stop;
done_testing();
