# Concurrent writers of prolly tables, as TPC-C's new-order and payment
# transactions write them, under repeatable read: pgbench retries the
# serialization failures and deadlocks the workload provokes, and anything
# else fails the run. What the clients committed must add up afterwards: no
# order number taken twice or skipped, no stock or money created or lost,
# and every payment counted once.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('concurrent');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method='dolt_prolly_table'\nmax_connections=40\nlog_error_verbosity=terse\n");
$node->start;
sub sql { return $node->safe_psql('postgres', shift) }

sql(q{CREATE TABLE district(d_id int PRIMARY KEY, next_o_id int NOT NULL, ytd bigint NOT NULL);
CREATE TABLE orders(d_id int, o_id int, item int NOT NULL, PRIMARY KEY (d_id, o_id));
CREATE INDEX orders_item ON orders(item);
CREATE TABLE stock(i_id int PRIMARY KEY, qty int NOT NULL);
CREATE TABLE account(id int PRIMARY KEY, balance bigint NOT NULL);
CREATE TABLE history(id bigserial PRIMARY KEY, d_id int NOT NULL, amount int NOT NULL);
INSERT INTO district SELECT g, 1, 0 FROM generate_series(1, 5) g;
INSERT INTO stock SELECT g, 100000 FROM generate_series(1, 200) g;
INSERT INTO account SELECT g, 1000 FROM generate_series(1, 50) g;
ALTER DATABASE postgres SET default_transaction_isolation = 'repeatable read'});

# The row journal (dolt_row_journal) loses updates here: it is off by default.
{
	my $log_start = -s $node->logfile;
	$node->pgbench(
		'--no-vacuum --client=8 --jobs=4 --time=20 --max-tries=1000 --failures-detailed',
		0,
		[qr{processed: [1-9]}, qr{number of failed transactions: 0 \(0\.000%\)}],
		[qr{^$}],
		'new-order and payment clients commit everything they attempt',
		{
			'new_order' => q{
\set d random(1, 5)
\set i random(1, 200)
BEGIN;
SELECT next_o_id AS o FROM district WHERE d_id = :d FOR UPDATE \gset
UPDATE district SET next_o_id = next_o_id + 1 WHERE d_id = :d;
INSERT INTO orders VALUES (:d, :o, :i);
UPDATE stock SET qty = qty - 1 WHERE i_id = :i;
END;
},
			'payment' => q{
\set d random(1, 5)
\set a random(1, 50)
\set b random(1, 50)
\set amount random(1, 20)
BEGIN;
UPDATE district SET ytd = ytd + :amount WHERE d_id = :d;
UPDATE account SET balance = balance - :amount WHERE id = :a;
UPDATE account SET balance = balance + :amount WHERE id = :b;
INSERT INTO history(d_id, amount) VALUES (:d, :amount);
END;
},
		});
	is(sql(q{SELECT count(*) FROM district d WHERE d.next_o_id - 1 <> (SELECT count(*) FROM orders o WHERE o.d_id = d.d_id)
	 OR d.next_o_id - 1 <> (SELECT coalesce(max(o_id), 0) FROM orders o WHERE o.d_id = d.d_id)}),
		'0', "every district's next order number follows its orders, none skipped");
	is(sql(q{SELECT 200 * 100000 - sum(qty) = (SELECT count(*) FROM orders) FROM stock}), 't',
		"stock taken equals orders placed");
	is(sql(q{SELECT sum(balance) FROM account}), '50000', "payments move money without creating or losing it");
	is(sql(q{SELECT (SELECT sum(ytd) FROM district) = (SELECT coalesce(sum(amount), 0) FROM history)}), 't',
		"each district's year-to-date total counts every payment once");
	is(sql(q{SELECT count(*) FROM orders o JOIN stock s ON s.i_id = o.item}), sql(q{SELECT count(*) FROM orders}),
		"every order's item is found through the index");
	my $log = slurp_file($node->logfile, $log_start);
	my @internal = grep { /ERROR/ && !/could not serialize|deadlock detected/ } split /\n/, $log;
	is(scalar(@internal), 0, "no error but serialization failures and deadlocks")
		or diag(join("\n", @internal[0 .. ($#internal < 4 ? $#internal : 4)]));
}

# A committing writer writes its own chunks before it waits for another
# publisher's lock on the table's root row: publishers of one table queue
# only for the merge, not for each other's chunk writes.
{
	my $before = sql('SELECT max(rootfile) FROM pg_dolt_root');
	sql(q{CREATE TABLE publish(id int PRIMARY KEY, v int NOT NULL);
INSERT INTO publish SELECT g, 0 FROM generate_series(1, 2000) g;
CREATE EXTENSION pageinspect});

	# The roots of the table and its index: the storage the table created.
	my $holder = $node->background_psql('postgres');
	$holder->query_safe('BEGIN');
	is($holder->query_safe("SELECT count(*) FROM (SELECT 1 FROM pg_dolt_root WHERE rootfile > $before FOR UPDATE) r"),
		'2', 'another session locks the root rows of the table and its index');
	my $writer = $node->background_psql('postgres');
	$writer->query_safe('BEGIN');
	$writer->query_safe('UPDATE publish SET v = v + 1');
	my $xid = $writer->query_safe('SELECT pg_current_xact_id()::xid');
	$writer->query_until(qr/committing/, "\\echo committing\nCOMMIT;\n");
	$node->poll_query_until('postgres',
		"SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE backend_xid = '$xid' AND wait_event_type = 'Lock')")
	  or die "writer never waited for the root row";
	ok( sql(qq{SELECT count(*) FROM generate_series(0, pg_relation_size('pg_dolt_chunk') / 8192 - 1) b,
heap_page_items(get_raw_page('pg_dolt_chunk', b::int)) i WHERE i.t_xmin = '$xid'}) > 0,
		'a writer waiting for the root row has already written its chunks');
	$holder->query_safe('COMMIT');
	$holder->quit;
	$writer->quit;
	is(sql('SELECT sum(v) FROM publish'), '2000', 'the waiting writer publishes after the lock is released');
}

$node->stop;
done_testing();
