
# Copyright (c) 2026, PostgreSQL Global Development Group

# An updater records its abort in pg_xact before it leaves the ProcArray. In
# that window a concurrent no-key update of the row, which ignores aborted
# updaters, must not keep the aborting one in the new multixact alongside
# itself: with key-share lockers holding the multixact, foreign key checks
# against a parent whose updates roll back used to fail with "new multixact
# has more than one updating member".
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', 'max_connections = 100');
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE TABLE parent (id bigint PRIMARY KEY, name text);
CREATE TABLE child (id bigserial PRIMARY KEY,
	parent bigint NOT NULL REFERENCES parent, title text);
INSERT INTO parent SELECT g, 'p' FROM generate_series(1, 10) g;
});

my $dir = PostgreSQL::Test::Utils::tempdir;
append_to_file("$dir/insert.sql",
	"\\set p random(1, 10)\nINSERT INTO child (parent, title) VALUES (:p, 't');\n");
append_to_file("$dir/rollback.sql",
	"\\set p random(1, 10)\nBEGIN;\nUPDATE parent SET name = md5(random()::text) WHERE id = :p;\nROLLBACK;\n"
);

# Inserts take key-share locks on parent rows; updates of them abort.
my $log_offset = -s $node->logfile;
my ($stdout, $stderr);
IPC::Run::run(
	[
		'pgbench', '-n', '-c', '48', '-j', '8', '-T', '15',
		'-f', "$dir/insert.sql\@8", '-f', "$dir/rollback.sql\@2",
		'-h', $node->host, '-p', $node->port, 'postgres'
	],
	'>', \$stdout, '2>', \$stderr);
like($stdout, qr/number of transactions actually processed: [1-9]/, 'the workload ran');

my $log = slurp_file($node->logfile, $log_offset);
unlike($log, qr/more than one updating member/,
	'an aborting updater is never kept beside a new one');
unlike($stderr, qr/more than one updating member/, 'and no client saw the error');

$node->stop;
done_testing();
