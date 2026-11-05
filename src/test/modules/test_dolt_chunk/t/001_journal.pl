# Copyright (c) 2026, PostgreSQL Global Development Group

# The chunk journal: concurrent puts never wait for each other, and committed
# chunks survive a crash, a torn tail and a damaged metapage. A standby
# receives logged chunks through the WAL, and only those.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunks');
$node->init(allows_streaming => 1);

# A small recent index, so that loads write runs and merge them.
$node->append_conf('postgresql.conf', "dolt_chunk_journal_tail = 4096\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_dolt_chunk');
sub sql { return $node->safe_psql('postgres', shift) }

# A second put of a chunk neither waits for the first nor depends on its outcome.
foreach my $outcome ('COMMIT', 'ROLLBACK')
{
	my $data = "shared-$outcome";
	my $first = $node->background_psql('postgres');
	my $second = $node->background_psql('postgres');

	$first->query_safe('BEGIN');
	is($first->query_safe("SELECT test_chunk_put(chunk_hash('$data'), '$data')"), 't', "first put stores ($outcome)");
	$second->query_safe('BEGIN');
	$second->query_safe("SELECT test_chunk_put(chunk_hash('$data'), '$data')");
	$first->query_safe($outcome);
	$second->query_safe('COMMIT');
	$second->quit;
	$first->quit;
	is(sql("SELECT test_chunk_get(chunk_hash('$data')) = '$data'"), 't', "the chunk is readable after $outcome");
	is(sql("SELECT count(*) FROM dolt_chunk_list() WHERE chunk = chunk_hash('$data')"), '1', "and listed once ($outcome)");
}

# Overlapping concurrent loads store every chunk, through runs and merges.
my @sessions = map { $node->background_psql('postgres') } 1 .. 4;
for my $i (0 .. $#sessions)
{
	my $first = $i * 10000;
	$sessions[$i]->query_until(qr/started/,
		"\\echo started\nSELECT test_chunk_bench_put(7, $first, 20000, 512);\n\\echo finished\n");
}
$_->quit for @sessions;
my $loaded = "SELECT count(*) FROM dolt_chunk_list() WHERE bytes = 512";
is(sql($loaded), '50000', 'overlapping loads store every chunk');
is(sql('SELECT test_chunk_bench_get(7, 50000, 50000, 3) >= 0'), 't', 'and every one is found');

# Committed puts survive a crash, logged or not.
sql("SELECT test_chunk_put(chunk_hash('durable'), 'durable')");
sql("SELECT test_chunk_put(chunk_hash('durable unlogged'), 'durable unlogged', false)");
$node->stop('immediate');
$node->start;
is(sql("SELECT test_chunk_get(chunk_hash('durable')) = 'durable'"), 't', 'a logged chunk survives a crash');
is(sql("SELECT test_chunk_get(chunk_hash('durable unlogged')) = 'durable unlogged'"), 't', 'and so does an unlogged one');
is(sql($loaded), '50000', 'and so do the runs');

# A torn tail is cut off at startup, and appends continue after it.
my $journal = $node->data_dir . '/pg_dolt/chunk_journal';
$node->stop('immediate');
open(my $fh, '>>', $journal) or die "could not open $journal: $!";
binmode $fh;
print $fh "\x01\x02\x03\x04" x 37;
close $fh;
$node->start;
is(sql($loaded), '50000', 'a torn tail loses nothing that was written whole');
sql("SELECT test_chunk_put(chunk_hash('after tear'), 'after tear')");
$node->restart;
is(sql("SELECT test_chunk_get(chunk_hash('after tear')) = 'after tear'"), 't', 'a put after a torn tail is readable after a restart');

# A damaged metapage falls back to the other, and with both damaged the stream is indexed anew.
my $index = $node->data_dir . '/pg_dolt/chunk_index';
for my $slots ([0], [0, 1])
{
	$node->stop;
	open(my $ih, '+<', $index) or die "could not open $index: $!";
	binmode $ih;
	for my $slot (@$slots)
	{
		seek($ih, $slot * 8192 + 8, 0);
		print $ih "\xff" x 16;
	}
	close $ih;
	$node->start;
	is(sql($loaded), '50000', 'every chunk is found with metapage ' . join(' and ', @$slots) . ' damaged');
	is(sql("SELECT test_chunk_get(chunk_hash('durable')) = 'durable'"), 't', 'including the recent ones');
}

# A standby receives logged chunks through the WAL, not unlogged ones.
$node->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'backup', has_streaming => 1);
$standby->start;
sql("SELECT test_chunk_put(chunk_hash('to standby'), 'to standby')");
sql("SELECT test_chunk_put(chunk_hash('primary only'), 'primary only', false)");
sql("SELECT test_chunk_put(chunk_hash('logged later'), 'logged later', false)");
sql("SELECT test_chunk_put(chunk_hash('logged later'), 'logged later')");
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', "SELECT test_chunk_get(chunk_hash('to standby')) = 'to standby'"), 't',
	'a logged chunk reaches the standby');
is($standby->safe_psql('postgres', "SELECT test_chunk_exists(chunk_hash('primary only'))"), 'f',
	'an unlogged one does not');
is($standby->safe_psql('postgres', "SELECT test_chunk_get(chunk_hash('logged later')) = 'logged later'"), 't',
	'a logged put of a chunk an unlogged put stored reaches the standby');
is($standby->safe_psql('postgres', $loaded), '50000', 'the standby has every chunk the backup held');

$standby->stop;
$node->stop;
done_testing();
