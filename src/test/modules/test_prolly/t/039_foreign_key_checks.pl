# Foreign key checks against prolly referenced tables look the key up in the
# referenced table's unique index and lock the row, without a query: they must
# behave as the query did, across key shapes, types and concurrent changes.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fk_checks');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\n");
$node->start;
sub sql { $node->safe_psql('postgres', $_[0]); }
sub fails
{
	my ($query, $pattern, $label) = @_;
	my ($ret, $out, $err) = $node->psql('postgres', $query);
	like($err, $pattern, $label);
}

# A composite key whose constraint lists columns in another order than the index.
sql(q{CREATE TABLE parent(a int, b text, c bigint, v int, PRIMARY KEY (a, b, c));
 INSERT INTO parent SELECT i, 'b' || i, i * 10, i FROM generate_series(1, 100) i;
 CREATE TABLE child(id int PRIMARY KEY, c bigint, a int, b text, FOREIGN KEY (c, a, b) REFERENCES parent(c, a, b))});
sql(q{INSERT INTO child VALUES (1, 50, 5, 'b5'), (2, 1000, 100, 'b100')});
is(sql(q{SELECT count(*) FROM child}), '2', 'rows with existing parents are accepted');
fails(q{INSERT INTO child VALUES (3, 51, 5, 'b5')}, qr/violates foreign key constraint .*\n.*Key \(c, a, b\)=\(51, 5, b5\) is not present in table "parent"/,
	'a missing parent is reported with its key');
sql(q{INSERT INTO child VALUES (4, NULL, 5, 'b5')});
is(sql(q{SELECT count(*) FROM child WHERE id = 4}), '1', 'a key with a null passes MATCH SIMPLE');
sql(q{UPDATE child SET c = 60, a = 6, b = 'b6' WHERE id = 1});
fails(q{UPDATE child SET a = 7 WHERE id = 1}, qr/violates foreign key constraint/, 'an update to a missing key is rejected');

# Different but compatible types on the two sides.
sql(q{CREATE TABLE p2(id int PRIMARY KEY); INSERT INTO p2 SELECT generate_series(1, 10);
 CREATE TABLE c2(id int PRIMARY KEY, pid bigint REFERENCES p2(id))});
sql(q{INSERT INTO c2 VALUES (1, 3)});
fails(q{INSERT INTO c2 VALUES (2, 11)}, qr/violates foreign key constraint/, 'a cross-type key is checked');

# The equality operator may take a different representation from the child.
# PostgreSQL's normal RI query inserts the required implicit casts.
sql(q{CREATE TABLE pfloat(id double precision PRIMARY KEY);
 INSERT INTO pfloat VALUES (1), (2);
 CREATE TABLE cint(id int REFERENCES pfloat)});
is(sql(q{INSERT INTO cint VALUES (1); SELECT id FROM cint}), '1',
 'an integer child is coerced for a double precision parent');
fails(q{INSERT INTO cint VALUES (3)}, qr/violates foreign key constraint/,
 'coercion does not admit a missing parent');
fails(q{DELETE FROM pfloat WHERE id=1}, qr/violates foreign key constraint/,
 'a cross-type reference prevents parent deletion');
fails(q{UPDATE pfloat SET id=3 WHERE id=1}, qr/violates foreign key constraint/,
 'a cross-type reference prevents parent key changes');
sql(q{CREATE TABLE pnumeric(id numeric PRIMARY KEY); INSERT INTO pnumeric VALUES (1);
 CREATE TABLE cnumeric(id bigint REFERENCES pnumeric)});
is(sql(q{INSERT INTO cnumeric VALUES (1); SELECT id FROM cnumeric}), '1',
 'a by-value integer is coerced to a by-reference numeric operator input');

# Rows the transaction itself inserted are found; rows it deleted are not.
is(sql(q{BEGIN; INSERT INTO p2 VALUES (20); INSERT INTO c2 VALUES (3, 20); COMMIT; SELECT pid FROM c2 WHERE id = 3}), '20',
	'a parent inserted earlier in the transaction is found');
fails(q{BEGIN; DELETE FROM p2 WHERE id = 9; INSERT INTO c2 VALUES (4, 9); COMMIT}, qr/violates foreign key constraint/,
	'a parent deleted earlier in the transaction is not');

# Concurrent changes to the parent row.
my $a = $node->background_psql('postgres', on_error_stop => 0);
my $b = $node->background_psql('postgres', on_error_stop => 0);
$a->query_safe('BEGIN; UPDATE p2 SET id = id WHERE id = 5');
is($b->query_safe('INSERT INTO c2 VALUES (10, 5); SELECT count(*) FROM c2 WHERE id = 10'), '1',
	'a non-key update of the parent does not block the check');
$a->query_safe('COMMIT');
$a->query_safe('BEGIN; DELETE FROM p2 WHERE id = 6');
$b->query_until(qr/started/, "\\echo started\nINSERT INTO c2 VALUES (11, 6);\n");
ok($node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock'}),
	'a check waits for a concurrent delete of its parent');
$a->query_safe('COMMIT');
$b->query('SELECT 1');
like($b->{stderr}, qr/violates foreign key constraint/, 'and fails once the delete commits');
$b->{stderr} = '';
$a->query_safe('BEGIN; DELETE FROM p2 WHERE id = 7');
$b->query_until(qr/started/, "\\echo started\nINSERT INTO c2 VALUES (12, 7);\n");
ok($node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock'}),
	'a check waits again');
$a->query_safe('ROLLBACK');
$b->query('SELECT 1');
is($b->{stderr}, '', 'and passes once the delete rolls back');
is(sql(q{SELECT count(*) FROM c2 WHERE id = 12}), '1', 'with its row inserted');
$b->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM c2');
$a->query_safe('DELETE FROM c2 WHERE pid = 8; DELETE FROM p2 WHERE id = 8');
$b->query('INSERT INTO c2 VALUES (13, 8)');
like($b->{stderr}, qr/violates foreign key constraint|could not serialize access/, 'repeatable read does not see a parent deleted since its snapshot');
$b->{stderr} = '';
$b->query('ROLLBACK');

# A parent locked FOR KEY SHARE by the check blocks its deletion until the checker ends.
$b->query_safe('BEGIN; INSERT INTO c2 VALUES (14, 2)');
$a->query_until(qr/started/, "\\echo started\nDELETE FROM p2 WHERE id = 2;\n");
ok($node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock'}),
	'deleting a parent waits for the transaction whose check locked it');
$b->query_safe('COMMIT');
$a->query('SELECT 1');
like($a->{stderr}, qr/violates foreign key constraint/, 'and then fails on the new reference');
$a->quit;
$b->quit;
$node->stop;
done_testing();
