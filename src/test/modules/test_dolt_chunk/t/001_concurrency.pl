# Copyright (c) 2026, PostgreSQL Global Development Group

# Concurrent inserts of the same chunk never wait for each other: each
# transaction that finds no committed copy stores its own. Committed chunks
# survive crash recovery while uncommitted ones do not.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunks');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_dolt_chunk');

sub rows
{
	my ($data) = @_;
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_dolt_chunk WHERE chunkhash = chunk_hash('$data')");
}

# The second inserter neither waits for nor relies on the first. If both
# commit there are two copies; if the first aborts, the second's copy remains.
foreach my $outcome ('COMMIT', 'ROLLBACK')
{
	my $data = "shared-$outcome";
	my $first = $node->background_psql('postgres');
	my $second = $node->background_psql('postgres');

	$first->query_safe('BEGIN');
	is($first->query_safe("SELECT test_chunk_put(chunk_hash('$data'), '$data')"),
		't', "first inserter inserts ($outcome)");
	$second->query_safe('BEGIN');
	is($second->query_safe("SELECT test_chunk_put(chunk_hash('$data'), '$data')"),
		't', "second inserter stores its own copy without waiting ($outcome)");
	$first->query_safe($outcome);
	$second->query_safe('COMMIT');
	$second->quit;
	$first->quit;
	is(rows($data), $outcome eq 'COMMIT' ? '2' : '1', "copies after $outcome");
	is($node->safe_psql('postgres', "SELECT test_chunk_get(chunk_hash('$data')) = '$data'"),
		't', "chunk readable after $outcome");
	is($node->safe_psql('postgres', "SELECT test_chunk_put(chunk_hash('$data'), '$data')"),
		'f', "later put finds a committed copy after $outcome");
}

# Overlapping concurrent loads store every chunk with no errors or waits.
my @sessions = map { $node->background_psql('postgres') } 1 .. 4;
for my $i (0 .. $#sessions)
{
	my $first = $i * 10000;
	$sessions[$i]->query_until(qr/started/,
		"\\echo started\nSELECT test_chunk_bench_put(7, $first, 20000, 512);\n\\echo finished\n");
}
$_->quit for @sessions;
is( $node->safe_psql(
		'postgres',
		"SELECT count(DISTINCT chunkhash) FROM pg_dolt_chunk WHERE length(chunkdata) = 512"),
	'50000',
	'overlapping loads store every chunk');
is( $node->safe_psql(
		'postgres',
		'SELECT count(*) FROM (SELECT chunkhash, chunkpart, chunkcopy FROM pg_dolt_chunk GROUP BY 1, 2, 3 HAVING count(*) > 1) d'),
	'0',
	'a transaction stores at most one copy of a chunk part');

# Committed chunks survive a crash; an in-progress insert does not.
$node->safe_psql('postgres', "SELECT test_chunk_put(chunk_hash('durable'), 'durable')");
my $pending = $node->background_psql('postgres');
$pending->query_safe('BEGIN');
$pending->query_safe("SELECT test_chunk_put(chunk_hash('pending'), 'pending')");
$node->stop('immediate');
$pending->quit;
$node->start;
is( $node->safe_psql(
		'postgres', "SELECT test_chunk_get(chunk_hash('durable')) = 'durable'"),
	't',
	'committed chunk recovered');
is( $node->safe_psql('postgres', "SELECT test_chunk_exists(chunk_hash('pending'))"),
	'f', 'uncommitted chunk absent after recovery');
is($node->safe_psql('postgres', "SELECT test_chunk_put(chunk_hash('pending'), 'pending')"),
	't', 'uncommitted chunk can be inserted again');

$node->stop;
done_testing();
