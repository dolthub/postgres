# Canonical schema properties are retained as prolly nodes alongside table data.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('schema_tree');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sub schema_hash
{
 my $server = shift // $node;
 return $server->safe_psql('postgres', q{SELECT encode(t.schema,'hex') FROM pg_dolt_ref r JOIN pg_dolt_object c ON c.objhash=r.reftarget CROSS JOIN LATERAL dolt_root_find(c.objchildren[1],'table','public.t') t WHERE r.refname='heads/main'});
}
sql(q{CREATE ROLE reader IN ROLE pg_read_all_data; CREATE TABLE t(id int PRIMARY KEY,v text COLLATE "C") USING dolt_prolly_table; INSERT INTO t VALUES(1,'first')});
my $first = sql(q{SELECT dolt_commit('-Am','first')});
my $schema = schema_hash();
is(sql("SELECT convert_from(objpayload,'UTF8'),cardinality(objchildren) FROM pg_dolt_object WHERE objhash=decode('$schema','hex')"), 'DGSCHEMA2|1', 'schema envelope names one retained prolly root');
is(sql("SELECT count(*)>0 FROM pg_dolt_object s JOIN pg_dolt_chunk c ON c.chunkhash=s.objchildren[1] WHERE s.objhash=decode('$schema','hex')"), 't', 'canonical schema uses the same chunk store as table contents');
is(sql(q{SELECT count(*) FROM pg_dolt_object WHERE objkind='chunk'}), '0', 'no tree is copied into manifest objects');
is(sql("SET ROLE reader; BEGIN READ ONLY; SELECT v FROM t AS OF '$first'; SELECT pg_current_xact_id_if_assigned() IS NULL; COMMIT"), "first\nt", 'cold schema-tree decoding needs no writes or elevated caller');
sql(q{UPDATE t SET v='second'});
my $second = sql(q{SELECT dolt_commit('-am','data only')});
is(schema_hash(), $schema, 'data-only edits reuse identical schema tree');
my $old_oid = sql(q{SELECT 't'::regclass::oid});
sql(q{DROP TABLE t; CREATE TABLE t(id int PRIMARY KEY,v text COLLATE "C") USING dolt_prolly_table; INSERT INTO t VALUES(1,'second')});
sql(q{SELECT dolt_commit('--allow-empty','-Am','same schema different physical OID')});
isnt(sql(q{SELECT 't'::regclass::oid}), $old_oid, 'replacement has a different physical OID');
is(schema_hash(), $schema, 'schema tree identity excludes allocation history');

sql(q{COMMENT ON COLUMN t.v IS 'new description'});
my $commented = sql(q{SELECT dolt_commit('-Am','comment change')});
isnt(schema_hash(), $schema, 'schema-only change changes canonical schema tree');
is(sql("SELECT a.v,b.v FROM t AS OF '$first' a JOIN t AS OF '$commented' b USING(id)"), 'first|second', 'independent schema and data revisions decode together');

my $count = sql(q{SELECT count(*) FROM pg_dolt_object});
sql(q{BEGIN; COMMENT ON COLUMN t.v IS 'aborted description'; SELECT dolt_add('.'); ROLLBACK});
is(sql(q{SELECT count(*) FROM pg_dolt_object}), $count, 'aborted capture rolls back schema chunks and staged publication');
is(sql("SELECT v FROM t AS OF '$commented'"), 'second', 'aborted schema capture leaves history readable');

# The foundation remains able to read pre-tree schema objects. Construct an old
# JSON descriptor from the live catalog's known portable fields, with the same
# content. This tests a mixed-format commit, not an upgrade rewrite.
my $legacy = sql(q{SELECT encode(dolt_manifest_put('schema',convert_to('{"format":"DGTABLE1","name":"public.t","owner":"reader","acl":[],"columns":[{"name":"id","type":"int4","typmod":-1},{"name":"v","type":"text","typmod":-1}]}','UTF8'),ARRAY[]::bytea[]),'hex')});
my $db = sql("SELECT encode(objchildren[1],'hex') FROM pg_dolt_object WHERE objhash=dolt_hash_decode('$commented')");
my $legacy_db = sql("SELECT encode(dolt_root_put(array_agg(kind),array_agg(name),array_agg(CASE WHEN kind='table' THEN decode('$legacy','hex') ELSE schema END),array_agg(content)),'hex') FROM dolt_root_members(decode('$db','hex'))");
my $legacy_commit = sql("SELECT dolt_hash_encode(dolt_manifest_put('commit',objpayload,ARRAY[decode('$legacy_db','hex')])) FROM pg_dolt_object WHERE objhash=dolt_hash_decode('$commented')");
is(sql("SET ROLE reader; SELECT v FROM t AS OF '$legacy_commit'"), 'second', 'legacy JSON schemas remain readable in mixed-format history');
# Invalid typed edges are rejected before publication.
my ($status, $out, $err) = $node->psql('postgres', "SELECT dolt_manifest_put('schema','invalid'::bytea,ARRAY[(SELECT objchildren[1] FROM pg_dolt_object WHERE objhash=decode('$schema','hex'))])");
isnt($status, 0, 'invalid tree envelope rejected');
like($err, qr/invalid schema tree envelope/, 'invalid envelope diagnostic');
sql(q{COMMENT ON COLUMN t.v IS U&'caf\00e9'; SELECT dolt_commit('-Am','unicode schema')});
my $utf8_schema = schema_hash();
my $latin = PostgreSQL::Test::Cluster->new('schema_latin1');
$latin->init(extra => ['--encoding=LATIN1', '--locale=C']);
$latin->start;
$latin->safe_psql('postgres', q{CREATE ROLE reader IN ROLE pg_read_all_data; CREATE TABLE t(id int PRIMARY KEY,v text COLLATE "C") USING dolt_prolly_table; COMMENT ON COLUMN t.v IS U&'caf\00e9'; SELECT dolt_commit('-Am','latin1 schema')});
is(schema_hash($latin), $utf8_schema, 'schema properties have canonical UTF8 bytes across database encodings');
$latin->stop;
sql('DROP TABLE t');
$node->stop('immediate');
$node->start;
is(sql("SET ROLE reader; SELECT v FROM t AS OF '$first'"), 'first', 'retained schema tree survives source drop and restart');
$node->stop;
done_testing();
