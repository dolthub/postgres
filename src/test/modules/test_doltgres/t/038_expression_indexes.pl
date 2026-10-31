# Expression keys and partial membership must survive native writes, canonical
# catalog rebinding, branch merges and journal replay alike.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('expression_indexes');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { return $node->safe_psql('postgres', shift) }
sub fails {
 my ($query, $pattern, $name) = @_;
 my ($rc, $out, $err) = $node->psql('postgres', $query);
 isnt($rc, 0, $name);
 like($err, $pattern, "$name error");
}
for my $journal ('off', 'on') {
 sql("ALTER SYSTEM SET dolt_row_journal = '$journal'; SELECT pg_reload_conf()");
 sql(q{CREATE TABLE t(id int PRIMARY KEY, a int, b int, label text, live bool);
 INSERT INTO t VALUES (1, 2, 3, 'Alpha', true), (2, 4, 5, 'Beta', false), (3, 2, 3, 'ALPHA', false), (4, NULL, 2, NULL, true);
 CREATE UNIQUE INDEX expr ON t(lower(label), (a+b) DESC) INCLUDE(b) WHERE live;
 CREATE INDEX sums ON t((a+b), id);
 CREATE TABLE nullable(v int);
 CREATE UNIQUE INDEX nullable_expr ON nullable((v+1)) WHERE v IS NULL;
 INSERT INTO nullable VALUES(NULL),(NULL);
 });
 is(sql(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT id FROM t WHERE live AND lower(label)='alpha' AND a+b=5}), '1', "$journal expression lookup");
 like(sql(q{SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT id FROM t WHERE live AND lower(label)='alpha'}), qr/Index.*expr/, "$journal uses partial expression index");
 fails(q{UPDATE t SET live=true WHERE id=3}, qr/duplicate key/, "$journal predicate transition checks uniqueness");
 sql(q{UPDATE t SET live=false WHERE id=1; UPDATE t SET live=true WHERE id=3});
 is(sql(q{SET enable_seqscan=off; SET enable_bitmapscan=off; SELECT id FROM t WHERE live AND lower(label)='alpha'}), '3', "$journal updates remove and add membership");
 is(sql(q{SET enable_seqscan=off; SET enable_indexscan=off; SELECT id FROM t WHERE a+b IN (5,9) ORDER BY id}), "1\n2\n3", "$journal bitmap expression qualification");
 sql(q{DELETE FROM t WHERE id=3; INSERT INTO t VALUES (5,2,3,'ALpha',true); SELECT dolt_commit('-Am','indexed base'); SELECT dolt_branch('feature')});
 is(sql(q{SET enable_seqscan=off; SELECT b FROM t WHERE live AND lower(label)='alpha'}), '3', "$journal included column after catalog rebinding");
 sql(q{SELECT dolt_checkout('feature'); INSERT INTO t VALUES (6,7,8,'Gamma',true); SELECT dolt_commit('-Am','feature')});
 sql(q{INSERT INTO t VALUES (7,8,9,'Delta',true); SELECT dolt_commit('-Am','main'); SELECT dolt_merge('feature')});
 is(sql(q{SET enable_seqscan=off; SELECT id FROM t WHERE live AND lower(label) IN ('gamma','delta') ORDER BY id}), "6\n7", "$journal merged expression keys");
 is(sql(q{SET enable_seqscan=off; SELECT id FROM t AS OF 'feature' WHERE live AND lower(label)='alpha'}), '5', "$journal revision expression binding");
 sql(q{SELECT dolt_branch('collision')});
 sql(q{SELECT dolt_checkout('collision'); INSERT INTO t VALUES (8,10,10,'Same',true); SELECT dolt_commit('-Am','collision')});
 sql(q{INSERT INTO t VALUES (9,10,10,'SAME',true); SELECT dolt_commit('-Am','ours')});
 is(sql(q{SELECT conflicts FROM dolt_merge('collision')}), '1', "$journal merge detects expression uniqueness collision");
 is(sql(q{SELECT count(*) FROM dolt_constraint_violations_t}), '2', "$journal reports both colliding rows");
 sql(q{SELECT dolt_merge('--abort'); SELECT dolt_branch('-D','collision')});
 sql(q{ALTER TABLE t RENAME COLUMN label TO name});
 is(sql(q{SET enable_seqscan=off; SELECT id FROM t WHERE live AND lower(name)='alpha'}), '5', "$journal renamed expression dependency");
 sql(q{DROP TABLE t, nullable; SELECT dolt_branch('-D','feature'); SELECT dolt_commit('-Am','cleanup')});
}
sql(q{CREATE TABLE patterns(id int PRIMARY KEY, t text, v varchar(20), b char(10));
 INSERT INTO patterns VALUES (1,'alpha','alpha','alpha'),(2,'beta','beta','beta'),(3,'alphabet','alphabet','alphabet');
 CREATE INDEX tp ON patterns(t text_pattern_ops);
 CREATE INDEX vp ON patterns(v varchar_pattern_ops);
 CREATE INDEX bp ON patterns(b bpchar_pattern_ops)});
for my $column ('t','v','b') {
 is(sql("SET enable_seqscan=off; SELECT id FROM patterns WHERE $column LIKE 'alph%' ORDER BY id"), "1\n3", "$column pattern index results");
 like(sql("SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT id FROM patterns WHERE $column LIKE 'alph%'"), qr/Index Cond:.*~>=~.*~<~/s, "$column prefix becomes index range");
}
$node->stop;
done_testing();
