use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('schema_objects');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nautovacuum=off\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub fails {
 my ($query, $pattern, $name) = @_;
 my ($status, $out, $err) = $node->psql('postgres', $query);
 isnt($status, 0, $name); like($err, $pattern, "$name diagnostic");
}

sql(q{CREATE TABLE t(v int); INSERT INTO t VALUES(1),(2),(3);
 SELECT dolt_commit('-Am','base'); SELECT dolt_branch('feature')});
is(sql(q{SELECT status FROM dolt_checkout('feature');
 CREATE AGGREGATE public.total(integer) (SFUNC=int4pl, STYPE=integer, INITCOND='0');
 SELECT public.total(v) FROM t}), "0\n6", 'native aggregate definition and execution');
is(sql(q{SELECT status FROM dolt_checkout('feature'); SELECT total(v) OVER (ORDER BY v) FROM t}), "0\n1\n3\n6", 'aggregate works as a window function');

sql(q{SELECT dolt_checkout('feature');
 CREATE FUNCTION public.same_int(integer,integer) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1=$2';
 CREATE OPERATOR public.=== (LEFTARG=integer, RIGHTARG=integer, FUNCTION=public.same_int, COMMUTATOR=OPERATOR(public.===));
 CREATE TYPE source AS (v int); CREATE TYPE target AS (v int);
 CREATE FUNCTION convert_value(source) RETURNS target LANGUAGE sql IMMUTABLE AS 'SELECT ROW(($1).v+1)::target';
 CREATE CAST (source AS target) WITH FUNCTION convert_value(source);
 SELECT dolt_commit('-Am','schema objects')});
is(sql(q{SELECT status FROM dolt_checkout('feature'); SELECT 2 OPERATOR(public.===) 2, (ROW(9)::source::target).v}), "0\nt|10", 'operator and cast execute in the branch');
is(sql(q{SELECT count(*) FROM pg_proc WHERE proname='total'}), '0', 'aggregate is absent on main');
is(sql(q{SELECT count(*) FROM pg_operator WHERE oprname='==='}), '0', 'operator is absent on main');
is(sql(q{SELECT count(*) FROM pg_cast c JOIN pg_type t ON t.oid=c.castsource WHERE t.typname='source'}), '0', 'cast is absent on main');

is(sql(q{SELECT has_conflicts FROM dolt_merge('feature'); SELECT total(v) FROM t}), "0\n6", 'merge publishes aggregate');
is(sql(q{SELECT 1 OPERATOR(public.===) 2, (ROW(4)::source::target).v}), 'f|5', 'merge publishes operator, cast and function dependencies');
fails(q{DROP FUNCTION same_int(integer,integer)}, qr/depend/, 'operator protects its function');
sql(q{BEGIN; DROP AGGREGATE total(integer); ROLLBACK});
is(sql(q{SELECT total(v) FROM t}), '6', 'aggregate DROP rolls back');
sql(q{DROP CAST (source AS target); DROP OPERATOR public.=== (integer,integer); DROP AGGREGATE total(integer)});
is(sql(q{SELECT count(*) FROM pg_proc WHERE proname='total'}), '0', 'aggregate DROP is published');
is(sql(q{SELECT status FROM dolt_checkout('feature'); SELECT total(v), 1 OPERATOR(public.===) 1 FROM t}), "0\n6|t", 'drops leave another branch intact');
$node->restart;
is(sql(q{SELECT status FROM dolt_checkout('feature'); SELECT (ROW(7)::source::target).v}), "0\n8", 'canonical casts survive restart');
$node->stop;
done_testing();
