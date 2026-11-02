use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node=PostgreSQL::Test::Cluster->new('stored_tests');
$node->init(extra=>['--encoding=UTF8','--locale=C']);$node->start;
sub sql { $node->safe_psql('postgres',$_[0]); }
sql(q{CREATE TABLE probe(id int) USING dolt_prolly_table;
INSERT INTO dolt_tests VALUES
('large','types','SELECT 9007199254740992::bigint','expected_single_value','!=','9007199254740993'),
('text','types',$q$SELECT '01'::text$q$,'expected_single_value','!=','1'),
('empty','types',$q$SELECT ''::text$q$,'expected_single_value','!=',NULL),
('null','types','SELECT NULL::text','expected_single_value','!=',''),
('boolean','types','SELECT true','expected_single_value','==','true'),
('types','named','SELECT 2','expected_single_value','==','2'),
('fractional','validation','SELECT 1','expected_rows','==','0.5'),
('multi','validation','SELECT 1; SELECT 2','expected_rows','==','1'),
('write','validation','CREATE TABLE forbidden(id int)','expected_rows','==','0'),
('cte','validation','WITH d AS (INSERT INTO probe VALUES(1) RETURNING *) SELECT * FROM d','expected_rows','==','1'),
('division','errors','SELECT 1/0','expected_rows','==','1'),
('good','errors','SELECT 1','expected_rows','==','1')});
is(sql(q{SELECT test_name,status FROM dolt_test_run('types')}),'types|PASS','exact name takes precedence over a matching group');
is(sql(q{SELECT count(*) FROM dolt_test_run('large','text','empty','null','boolean') WHERE status='PASS'}),'5','typed comparisons preserve exact numeric values, text, NULL and booleans');
is(sql(q{SELECT count(*) FROM dolt_test_run('good','errors')}),'3','multiple selectors preserve repeated matches');
is(sql(q{SELECT test_name,message FROM dolt_test_run('validation') ORDER BY test_name}),"cte|Cannot execute write queries\nfractional|cannot run assertion on non integer value: 0.5\nmulti|Can only run exactly one query\nwrite|Cannot execute write queries",'one read query per stored test');
is(sql(q{SELECT count(*) FROM probe}),'0','modifying CTE did not run');
is(sql(q{SELECT count(*) FROM pg_class WHERE relname='forbidden'}),'0','DDL test did not run');
is(sql(q{SELECT test_name,status FROM dolt_test_run('errors') ORDER BY test_name}),"division|FAIL\ngood|PASS",'query error does not abort subsequent tests');
my($ret,$out,$err)=$node->psql('postgres',q{SELECT * FROM dolt_test_run('good','missing')});isnt($ret,0,'missing selector rejected even after a valid selector');like($err,qr/could not find tests for argument: missing/,'missing selector diagnostic');
$node->stop;done_testing();
