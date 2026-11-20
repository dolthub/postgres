# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# The checkpoint's runs of the delta (design item 8): a checkpoint writes every
# storage's delta into runs its manifest names, a start loads them back before
# redo, and a recovery's cleanup drops an unlogged storage's, as Postgres
# empties an unlogged relation after a crash. No read of the delta reaches SQL
# yet, so each case reads what the runs gave a storage's tuple id counter: the
# ctid the next insert takes, through the test module's insert.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('runs');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 2');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');

# inserted returns the ctids the fork gives a source table's rows inserted
# into a target one at a time, on the node given or the primary.
sub inserted
{
	my ($target, $source, $on) = @_;
	return ($on // $node)
	  ->safe_psql('postgres',
		"SELECT doltgres_test_insert('$target', '$source', false, 0)");
}

$node->safe_psql(
	'postgres', q{
CREATE TABLE source_three (a int4);
INSERT INTO source_three VALUES (1), (2), (3);
CREATE TABLE logged (a int4);
CREATE UNLOGGED TABLE unlogged (a int4);
});
is(inserted('logged', 'source_three'),
	"(0,1)\n(0,2)\n(0,3)", 'three rows take the first three ctids');
is(inserted('unlogged', 'source_three'),
	"(0,1)\n(0,2)\n(0,3)", "an unlogged table's three rows take them too");

$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
is( inserted('logged', 'source_three'),
	"(0,4)\n(0,5)\n(0,6)",
	"after a crash the next insert takes a ctid past every row the checkpoint's runs loaded"
);
is( inserted('unlogged', 'source_three'),
	"(0,1)\n(0,2)\n(0,3)",
	"after a crash an unlogged table's rows are gone, so its next insert takes the first ctid again"
);

$node->stop;
$node->start;
is( inserted('unlogged', 'source_three'),
	"(0,4)\n(0,5)\n(0,6)",
	"after a clean stop an unlogged table keeps its rows through the shutdown checkpoint's runs"
);
is(inserted('logged', 'source_three'),
	"(0,7)\n(0,8)\n(0,9)",
	"after a clean stop a table's rows come back from the runs too");

# A prepared transaction's rows survive a crash through the checkpoint's runs,
# and it commits after.
is( $node->safe_psql(
		'postgres', q{
BEGIN;
SELECT doltgres_test_insert('logged', 'source_three', false, 0);
PREPARE TRANSACTION 'runs';
}),
	"(0,10)\n(0,11)\n(0,12)",
	'a prepared transaction takes the next three ctids');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT gid FROM pg_prepared_xacts'),
	'runs', 'the prepared transaction is still prepared after a crash');
$node->safe_psql('postgres', "COMMIT PREPARED 'runs'");
is( inserted('logged', 'source_three'),
	"(0,13)\n(0,14)\n(0,15)",
	"after a crash and its commit, the next insert takes a ctid past the prepared transaction's rows"
);

# A standby restarted after a restartpoint loads that restartpoint's runs, then
# redoes the records after it.
$node->backup('runs_backup');
my $standby = PostgreSQL::Test::Cluster->new('runs_standby');
$standby->init_from_backup($node, 'runs_backup', has_streaming => 1);
$standby->start;
is(inserted('logged', 'source_three'),
	"(0,16)\n(0,17)\n(0,18)",
	'the primary inserts while the standby follows');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
is(inserted('logged', 'source_three'),
	"(0,19)\n(0,20)\n(0,21)", 'the primary inserts after the restartpoint');
$node->wait_for_replay_catchup($standby);
$standby->stop('immediate');
$standby->start;
$node->wait_for_replay_catchup($standby);
$standby->promote;
is( inserted('logged', 'source_three', $standby),
	"(0,22)\n(0,23)\n(0,24)",
	"a standby restarted after a restartpoint takes a ctid past every row its runs and its redo brought"
);
is( inserted('unlogged', 'source_three', $standby),
	"(0,1)\n(0,2)\n(0,3)",
	"a promoted standby's unlogged table is empty, so its next insert takes the first ctid"
);

$standby->stop;

# A keyed table's rows inserted and then replayed in crash recovery, which
# makes their storage's state with no catalog and so no base, come back after
# a later restart from the end-of-recovery checkpoint's runs, and the next
# insert, whose make takes the catalog's key, continues past them.
$node->safe_psql(
	'postgres', q{
CREATE TABLE source_later (a int4);
INSERT INTO source_later VALUES (4), (5), (6);
CREATE TABLE keyed (a int4 PRIMARY KEY);
});
is(inserted('keyed', 'source_three'),
	"(0,1)\n(0,2)\n(0,3)", 'a keyed table takes the first three ctids');
$node->stop('immediate');
$node->start;
$node->stop('immediate');
$node->start;
is( inserted('keyed', 'source_later'),
	"(0,4)\n(0,5)\n(0,6)",
	"a keyed table's rows replayed in crash recovery come back from the end-of-recovery checkpoint's runs"
);

# damaged flips a file's first byte and returns its bytes as they were;
# restored writes them back.
sub damaged
{
	my ($path) = @_;
	open(my $in, '<:raw', $path) or die "could not read \"$path\": $!";
	local $/;
	my $bytes = <$in>;
	close($in);
	restored($path,
		chr(ord(substr($bytes, 0, 1)) ^ 0xff) . substr($bytes, 1));
	return $bytes;
}

sub restored
{
	my ($path, $bytes) = @_;
	open(my $out, '>:raw', $path) or die "could not write \"$path\": $!";
	print $out $bytes;
	close($out);
}

# A database whose newest manifest is damaged is warned of once at the start,
# its runs left as they are, while the other database's runs load; its
# manifest made whole under the running server, its storages are refused until
# the server restarts, and the next start loads its runs.
$node->safe_psql('postgres', 'CREATE DATABASE other');
my $other = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'other'");
$node->safe_psql(
	'other', q{
CREATE EXTENSION doltgres_test; CALL doltgres_test_twins();
CREATE TABLE source_three (a int4);
INSERT INTO source_three VALUES (1), (2), (3);
CREATE TABLE logged (a int4);
SELECT doltgres_test_insert('logged', 'source_three', false, 0);
});
$node->stop;
my $manifest = $node->data_dir . "/pg_versioned/$other/manifest";
my $whole = damaged($manifest);
my $offset = -s $node->logfile;
$node->start;
my $log = slurp_file($node->logfile, $offset);
my $warned = () =
  $log =~ /database $other: its checkpoint's runs aren't loaded/g;
is($warned, 1, 'the start warns once of the database it skips');
like($log, qr/WARNING:  /, 'as a warning');
is(inserted('logged', 'source_three'),
	"(0,22)\n(0,23)\n(0,24)",
	"the other database's runs load past the skipped one");
restored($manifest, $whole);
my ($code, $stdout, $stderr) = $node->psql('other',
	"SELECT doltgres_test_insert('logged', 'source_three', false, 0)");
like(
	$stderr,
	qr/ERROR:  database $other is refused until the server restarts/,
	"the skipped database's storages are refused once its manifest is whole");
$node->restart;
is( $node->safe_psql(
		'other',
		"SELECT doltgres_test_insert('logged', 'source_three', false, 0)"),
	"(0,4)\n(0,5)\n(0,6)",
	"once its manifest is whole, the next start loads the skipped database's runs"
);

# remade makes a database of a name, with the OID given or a new one, with a
# table of three rows the fork inserted, and returns their ctids.
sub remade
{
	my ($name, $oid) = @_;
	$node->safe_psql('postgres',
		"CREATE DATABASE $name" . (defined $oid ? " OID $oid" : ''));
	return $node->safe_psql(
		$name, q{
CREATE EXTENSION doltgres_test; CALL doltgres_test_twins();
CREATE TABLE source_three (a int4);
INSERT INTO source_three VALUES (1), (2), (3);
CREATE TABLE logged (a int4);
SELECT doltgres_test_insert('logged', 'source_three', false, 0);
});
}

# A dropped database's states are freed at its drop and at the drop's redo, so
# neither the drop's checkpoint, nor a checkpoint or restartpoint after it,
# opens its repository, and a database made again with its OID starts empty
# (bugs row 66).
$node->backup('drop_backup');
my $follower = PostgreSQL::Test::Cluster->new('runs_follower');
$follower->init_from_backup($node, 'drop_backup', has_streaming => 1);
$follower->start;
is(remade('remade'), "(0,1)
(0,2)
(0,3)", 'a new database takes the first three ctids');
my $remade = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'remade'");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($follower);
$follower->safe_psql('postgres', 'CHECKPOINT');
($code, $stdout, $stderr) =
  $node->psql('postgres', 'DROP DATABASE remade');
is($stderr, '', "the drop's checkpoint opens nothing of the dropped database");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($follower);
($code, $stdout, $stderr) = $follower->psql('postgres', 'CHECKPOINT');
is($stderr, '',
	"a restartpoint after the drop's redo opens nothing of the dropped database");
is(remade('remade', $remade), "(0,1)
(0,2)
(0,3)",
	'a database made again with the OID takes the first three ctids');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($follower);
$follower->safe_psql('postgres', 'CHECKPOINT');
$follower->stop('immediate');
$follower->start;
$node->wait_for_replay_catchup($follower);
$follower->promote;
is( $follower->safe_psql(
		'remade',
		"SELECT doltgres_test_insert('logged', 'source_three', false, 0)"),
	"(0,4)
(0,5)
(0,6)",
	"the database made again loads its own runs, past its first three rows"
);
$follower->stop;

# A database skipped for its damaged manifest, on the primary and on a standby
# whose own copy is damaged, has its mark freed by its drop and by the drop's
# redo, so a database made again with its OID is used at once on both.
is(remade('marked'), "(0,1)
(0,2)
(0,3)", 'a database to skip takes the first three ctids');
my $marked = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'marked'");
$node->safe_psql('postgres', 'CHECKPOINT');
$node->backup('mark_backup');
my $marking = PostgreSQL::Test::Cluster->new('runs_marking');
$marking->init_from_backup($node, 'mark_backup', has_streaming => 1);
$marking->start;
$node->wait_for_replay_catchup($marking);
$marking->stop;
damaged($marking->data_dir . "/pg_versioned/$marked/manifest");
$marking->start;
$node->stop;
my $marked_manifest = $node->data_dir . "/pg_versioned/$marked/manifest";
my $marked_whole = damaged($marked_manifest);
$node->start;
restored($marked_manifest, $marked_whole);
($code, $stdout, $stderr) = $node->psql('marked',
	"SELECT doltgres_test_insert('logged', 'source_three', false, 0)");
like(
	$stderr,
	qr/ERROR:  database $marked is refused until the server restarts/,
	'the skipped database is refused');
$node->safe_psql('postgres', 'DROP DATABASE marked');
is(remade('marked', $marked), "(0,1)
(0,2)
(0,3)",
	"a database made again with a skipped database's OID is used at once");
$node->wait_for_replay_catchup($marking);
$marking->promote;
is( $marking->safe_psql(
		'marked',
		"SELECT doltgres_test_insert('logged', 'source_three', false, 0)"),
	"(0,4)
(0,5)
(0,6)",
	"a standby's redo of the drop frees its own mark, so the database made again is used after its promotion"
);
$marking->stop;

$node->stop;
done_testing();
