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

# The frees of a TOAST relation's values (bugs row 146): VACUUM's free of a
# tuple id whose row version is gone for good takes the values it named down in
# the TOAST storage's value map, a value going when no row version names it,
# through both of the free's paths (a base row a fold replaced or deleted, and a
# version that never reached a base), never while a snapshot may still read the
# row, never for a writer whose abort already took its values back, and once
# through a crash's redo.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('frees');
$node->init(allows_streaming => 1);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION doltgres_test');

# held_values returns how many values a table's TOAST relation's value map
# holds on a server, the primary unless named, counted again by a reader first.
sub held_values
{
	my ($table, $on) = @_;
	$on //= $node;
	my $toast = $on->safe_psql('postgres',
		"SELECT reltoastrelid::regclass FROM pg_class WHERE relname = '$table'");
	$on->safe_psql('postgres', "SELECT pg_relation_size('$toast')");
	my $totals = $on->safe_psql('postgres', "SELECT doltgres_test_totals('$toast')");
	return (split / /, $totals)[1];
}

# made makes a keyed table whose v values are kept out of line, with rows 1 to
# a count, each value 3,000 bytes.
sub made
{
	my ($table, $rows) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $table (k int4 PRIMARY KEY, v text);
ALTER TABLE $table ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO $table SELECT g, repeat(chr(64 + g), 3000) FROM generate_series(1, $rows) g;
});
	return;
}

# A deleted row's value goes at VACUUM's free of its tuple id, folded into the
# base first or not, and not while a snapshot taken before the delete is held,
# which still reads the value.
made('deleted', 3);
is(held_values('deleted'), 3, 'three rows hold three values');
$node->safe_psql('postgres',
	q{DELETE FROM deleted WHERE k = 1; SELECT doltgres_test_fold('deleted'); VACUUM deleted});
is(held_values('deleted'), 2, 'VACUUM frees a deleted row\'s value that never reached a base');
my $old = $node->background_psql('postgres');
$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$old->query_safe('SELECT count(*) FROM deleted');
$node->safe_psql('postgres', q{DELETE FROM deleted WHERE k = 2; SELECT doltgres_test_fold('deleted')});
$node->safe_psql('postgres', 'VACUUM deleted');
is(held_values('deleted'), 2, 'VACUUM keeps a folded deleted row\'s value while an older snapshot may read it');
is($old->query_safe('SELECT length(v) FROM deleted WHERE k = 2'),
	'3000', 'the older snapshot still reads the deleted row\'s value');
$old->query_safe('COMMIT');
$old->quit;
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('deleted'); VACUUM deleted});
is(held_values('deleted'), 1, 'VACUUM frees a folded deleted row\'s value once no snapshot may read it');
is($node->safe_psql('postgres', 'SELECT k, length(v) FROM deleted'),
	'3|3000', 'the row left keeps its value');

# An update that keeps one value and replaces another frees only the replaced
# one, and the kept value's count holds for the row that still names it.
$node->safe_psql(
	'postgres', q{
CREATE TABLE kept (k int4 PRIMARY KEY, a text, b text);
ALTER TABLE kept ALTER COLUMN a SET STORAGE EXTERNAL;
ALTER TABLE kept ALTER COLUMN b SET STORAGE EXTERNAL;
INSERT INTO kept VALUES (1, repeat('a', 3000), repeat('b', 3000));
SELECT doltgres_test_fold('kept');
UPDATE kept SET b = repeat('c', 3000);
});
is(held_values('kept'), 3, 'an update keeping a and replacing b holds three values');
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('kept'); VACUUM kept});
is(held_values('kept'), 2, 'VACUUM frees the replaced value and keeps the kept one');
$node->safe_psql('postgres', q{DELETE FROM kept; SELECT doltgres_test_fold('kept'); VACUUM kept});
is(held_values('kept'), 0, 'deleting the row frees the kept value too, its count held at one');

# An aborted insert's values go at its abort, and a rolled-back savepoint's at
# its ROLLBACK TO, so VACUUM takes none of them down again.
made('aborted', 1);
$node->safe_psql('postgres',
	q{BEGIN; INSERT INTO aborted VALUES (2, repeat('x', 3000)); ROLLBACK});
is(held_values('aborted'), 1, 'an aborted insert\'s value goes at its abort');
$node->safe_psql(
	'postgres', q{
BEGIN;
SAVEPOINT s;
INSERT INTO aborted VALUES (3, repeat('y', 3000));
ROLLBACK TO s;
INSERT INTO aborted VALUES (4, repeat('z', 3000));
COMMIT;
});
is(held_values('aborted'), 2, 'a rolled-back savepoint\'s value goes at its ROLLBACK TO');
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('aborted'); VACUUM aborted});
is(held_values('aborted'), 2, 'VACUUM takes no aborted or rolled-back value down again');
is($node->safe_psql('postgres', 'SELECT k, length(v) FROM aborted ORDER BY k'),
	"1|3000\n4|3000", 'every committed row reads its value');

# A folded and vacuumed insert keeps its value.
made('folded', 2);
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('folded'); VACUUM folded});
is(held_values('folded'), 2, 'VACUUM frees no value of a live row');
is($node->safe_psql('postgres', 'SELECT sum(length(v)) FROM folded'), '6000', 'both rows read their values');

# A TOAST relation's size comes back to its start once its table's rows are all
# deleted and vacuumed.
made('sized', 0);
my $toast = $node->safe_psql('postgres',
	q{SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'sized'});
my $start = $node->safe_psql('postgres', "SELECT pg_relation_size('$toast')");
$node->safe_psql('postgres',
	q{INSERT INTO sized SELECT g, repeat('s', 3000) FROM generate_series(1, 20) g});
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('sized'); DELETE FROM sized; SELECT doltgres_test_fold('sized')});
$node->safe_psql('postgres', 'VACUUM sized');
is($node->safe_psql('postgres', "SELECT pg_relation_size('$toast')"),
	$start, 'a TOAST relation\'s size comes back to its start after its rows are deleted and vacuumed');

# A row inserted before a crash and deleted after the restart frees its value
# once, its mark replayed with its row.
made('crashed', 2);
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', q{DELETE FROM crashed WHERE k = 1; SELECT doltgres_test_fold('crashed'); VACUUM crashed});
is(held_values('crashed'), 1, 'a row written before a crash frees its value once after the restart');
is($node->safe_psql('postgres', 'SELECT k, length(v) FROM crashed'), '2|3000', 'the other row reads its value');

# PRevPF 8's balance: a base row naming V and W, an update keeping V and
# replacing W, and a delete of the update's row before the fold that applies
# both: the base row's pending free takes V and W down and the update's take
# takes V and the new W down, so every value goes.
$node->safe_psql(
	'postgres', q{
CREATE TABLE balanced (k int4 PRIMARY KEY, v text, w text);
ALTER TABLE balanced ALTER COLUMN v SET STORAGE EXTERNAL;
ALTER TABLE balanced ALTER COLUMN w SET STORAGE EXTERNAL;
INSERT INTO balanced VALUES (1, repeat('v', 3000), repeat('w', 3000));
SELECT doltgres_test_fold('balanced');
UPDATE balanced SET w = repeat('x', 3000);
DELETE FROM balanced;
SELECT doltgres_test_fold('balanced');
VACUUM balanced;
});
is(held_values('balanced'), 0, 'a base row, an update keeping a value, and a delete before the fold free every value');

# A crash after a free's record is flushed and before its take-downs reach the
# map: redo takes the values down once from the record, and a later VACUUM
# takes none down again.
made('logged', 2);
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', q{DELETE FROM logged WHERE k = 1; SELECT doltgres_test_fold('logged')});
my $vacuum = $node->background_psql('postgres', on_error_stop => 0);
$vacuum->query_safe(
	"SELECT injection_points_set_local(); SELECT injection_points_attach('doltgres-vacuum-logged', 'wait')");
$vacuum->query_until(qr/start/, "\\echo start\nVACUUM logged;\n");
$node->wait_for_event('client backend', 'doltgres-vacuum-logged');
$node->safe_psql('postgres', 'CREATE TABLE flushed (k int4)');
$node->stop('immediate');
$vacuum->quit;
$node->start;
is(held_values('logged'), 1, 'redo of a free cut short after its record takes its value down');
$node->safe_psql('postgres', 'VACUUM logged');
is(held_values('logged'), 1, 'a later VACUUM takes no value down again');
is($node->safe_psql('postgres', 'SELECT k, length(v) FROM logged'), '2|3000', 'the live row reads its value');

# A unique check's wait, then the row written at its spent take in a later
# window and aborted: its values go at the abort, VACUUM takes none down, and
# a later write of the key and its delete frees its values once.
made('waited', 0);
my $holder = $node->background_psql('postgres');
$holder->query_safe(q{BEGIN; INSERT INTO waited VALUES (1, repeat('h', 3000))});
my $waiter = $node->background_psql('postgres', on_error_stop => 0);
$waiter->query_until(qr/start/,
	"\\echo start\nBEGIN;\nINSERT INTO waited VALUES (1, repeat('w', 3000));\n");
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock'}),
	'the second insert waits on the first');
$holder->query_safe('ROLLBACK');
$holder->quit;
$waiter->query_safe('ROLLBACK');
$waiter->quit;
is(held_values('waited'), 0, 'an aborted write at a spent take leaves no value');
$node->safe_psql('postgres',
	q{INSERT INTO waited VALUES (1, repeat('z', 3000)); SELECT doltgres_test_fold('waited'); VACUUM waited});
is(held_values('waited'), 1, 'VACUUM takes no aborted value down again');
$node->safe_psql('postgres',
	q{DELETE FROM waited; SELECT doltgres_test_fold('waited'); VACUUM waited});
is(held_values('waited'), 0, 'a later delete of the key frees its value once');

# An UPDATE keeping V, paused after its read of V's base entry, while VACUUM
# takes V down for the replaced version of an earlier update that kept it:
# V's count ends at the rows that name it, so a delete frees every value.
$node->safe_psql(
	'postgres', q{
CREATE TABLE paused (k int4 PRIMARY KEY, v text, w text);
ALTER TABLE paused ALTER COLUMN v SET STORAGE EXTERNAL;
ALTER TABLE paused ALTER COLUMN w SET STORAGE EXTERNAL;
INSERT INTO paused VALUES (1, repeat('v', 3000), repeat('w', 3000));
SELECT doltgres_test_fold('paused');
UPDATE paused SET w = repeat('x', 3000);
SELECT doltgres_test_fold('paused');
});
$node->safe_psql('postgres',
	"SELECT injection_points_attach('doltgres-kept-read', 'wait')");
my $updater = $node->background_psql('postgres');
$updater->query_until(qr/start/,
	"\\echo start\nUPDATE paused SET w = repeat('y', 3000);\n");
$node->wait_for_event('client backend', 'doltgres-kept-read');
$node->safe_psql('postgres', 'VACUUM paused');
$node->safe_psql('postgres', q{
SELECT injection_points_detach('doltgres-kept-read');
SELECT injection_points_wakeup('doltgres-kept-read');
});
$updater->quit;
is( $node->safe_psql('postgres', 'SELECT length(v), left(w, 1) FROM paused'),
	'3000|y', 'the paused update reads its kept value');
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('paused'); VACUUM paused});
is(held_values('paused'), 2, 'the kept value and the newest replaced one are held');
$node->safe_psql('postgres',
	q{DELETE FROM paused; SELECT doltgres_test_fold('paused'); VACUUM paused});
is(held_values('paused'), 0, 'a paused update\'s kept value counts once, so a delete frees it');

# The same with a second UPDATE of the row keeping V in VACUUM's place: in
# whichever order the two write, each adds to the newest version of V's entry,
# chosen under the range's lock, so V counts the rows that name it.
$node->safe_psql(
	'postgres', q{
INSERT INTO paused VALUES (1, repeat('v', 3000), repeat('w', 3000));
SELECT doltgres_test_fold('paused');
SELECT injection_points_attach('doltgres-kept-read', 'wait');
});
$updater = $node->background_psql('postgres');
$updater->query_until(qr/start/,
	"\\echo start\nUPDATE paused SET w = repeat('y', 3000);\n");
$node->wait_for_event('client backend', 'doltgres-kept-read');
$node->safe_psql('postgres', "SELECT injection_points_detach('doltgres-kept-read')");
my $second = $node->background_psql('postgres');
$second->query_until(qr/start/,
	"\\echo start\nUPDATE paused SET w = repeat('z', 3000);\n");
$node->safe_psql('postgres', "SELECT injection_points_wakeup('doltgres-kept-read')");
$updater->quit;
$second->quit;
is($node->safe_psql('postgres', 'SELECT length(v) FROM paused'), '3000', 'the row reads its kept value');
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('paused'); VACUUM paused});
is(held_values('paused'), 2, 'the kept value and the last update\'s are held');
$node->safe_psql('postgres',
	q{DELETE FROM paused; SELECT doltgres_test_fold('paused'); VACUUM paused});
is(held_values('paused'), 0, 'the kept value counts once for each row naming it, so a delete frees it');

# The unique check's wait with the second insert committed: its row, written
# at the spent take, is deleted before the fold, and VACUUM's free takes its
# value down through the mark.
made('marked', 0);
$holder = $node->background_psql('postgres');
$holder->query_safe(q{BEGIN; INSERT INTO marked VALUES (1, repeat('h', 3000))});
$waiter = $node->background_psql('postgres', on_error_stop => 0);
$waiter->query_until(qr/start/,
	"\\echo start\nBEGIN;\nINSERT INTO marked VALUES (1, repeat('w', 3000));\n");
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock'}),
	'the second insert waits on the first again');
$holder->query_safe('ROLLBACK');
$holder->quit;
$waiter->query_safe('COMMIT');
$waiter->quit;
is(held_values('marked'), 1, 'the committed write at the spent take holds its value');
$node->safe_psql('postgres',
	q{DELETE FROM marked; SELECT doltgres_test_fold('marked'); VACUUM marked});
is(held_values('marked'), 0, 'VACUUM frees the value of a row written at a spent take through its mark');

# A storage emptied in place, by a truncate of a table its own transaction
# made: the values of the rows before it go with the TOAST storage, VACUUM
# takes none of them down, and the later row's value goes once it is deleted.
$node->safe_psql(
	'postgres', q{
BEGIN;
CREATE TABLE emptied (k int4 PRIMARY KEY, v text);
ALTER TABLE emptied ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO emptied SELECT g, repeat('e', 3000) FROM generate_series(1, 2) g;
TRUNCATE emptied;
INSERT INTO emptied VALUES (3, repeat('f', 3000));
COMMIT;
});
is(held_values('emptied'), 1, 'a truncate in place leaves only the later row\'s value');
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('emptied'); VACUUM emptied});
is(held_values('emptied'), 1, 'VACUUM takes no emptied row\'s value down');
$node->safe_psql('postgres',
	q{DELETE FROM emptied; SELECT doltgres_test_fold('emptied'); VACUUM emptied});
is(held_values('emptied'), 0, 'the later row\'s value goes once it is deleted and vacuumed');

# spent_take makes a table whose one row a second insert wrote at the take a
# unique check's wait left spent, its first insert rolled back.
sub spent_take
{
	my ($table) = @_;
	made($table, 0);
	my $first = $node->background_psql('postgres');
	$first->query_safe(qq{BEGIN; INSERT INTO $table VALUES (1, repeat('h', 3000))});
	my $second = $node->background_psql('postgres', on_error_stop => 0);
	$second->query_until(qr/start/,
		"\\echo start\nBEGIN;\nINSERT INTO $table VALUES (1, repeat('w', 3000));\n");
	$node->poll_query_until('postgres',
		q{SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock'})
	  or die 'the second insert never waited';
	$first->query_safe('ROLLBACK');
	$first->quit;
	$second->query_safe('COMMIT');
	$second->quit;
	return;
}

# A row written at a spent take, then a crash: redo writes its row and its mark
# once, so a delete after the restart frees its value once.
spent_take('redone');
$node->stop('immediate');
$node->start;
is(held_values('redone'), 1, 'a row written at a spent take holds its value after a crash');
$node->safe_psql('postgres',
	q{DELETE FROM redone; SELECT doltgres_test_fold('redone'); VACUUM redone});
is(held_values('redone'), 0, 'its delete after the restart frees its value once');

# A rewrite's carried version, a deleted row an older snapshot may still read,
# names its value in the new TOAST relation's map, from which VACUUM frees it
# once that snapshot ends.
made('carried', 2);
my $older = $node->background_psql('postgres');
$older->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$older->query_safe('SELECT count(*) FROM pg_class');
$node->safe_psql('postgres', 'DELETE FROM carried WHERE k = 1');
$node->safe_psql('postgres', 'VACUUM FULL carried');
is(held_values('carried'), 2, 'a rewrite carries a deleted row\'s value into its new TOAST relation');
$older->query_safe('COMMIT');
$older->quit;
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('carried'); VACUUM carried});
is(held_values('carried'), 1, 'VACUUM frees the carried version\'s value from the new TOAST relation');
is($node->safe_psql('postgres', 'SELECT k, length(v) FROM carried'), '2|3000', 'the live row reads its value');

# A standby snapshot taken before a free that may still read the deleted row:
# the replayed free cancels it, and after the replay the standby's map equals
# the primary's, a row written at a spent take and freed included.
made('followed', 2);
$node->backup('frees_backup');
my $standby = PostgreSQL::Test::Cluster->new('frees_standby');
$standby->init_from_backup($node, 'frees_backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'max_standby_streaming_delay = 0');
$standby->start;
spent_take('replayed');
$node->wait_for_replay_catchup($standby);
my $reader = $standby->background_psql('postgres', on_error_stop => 0);
$reader->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($reader->query_safe('SELECT length(v) FROM followed WHERE k = 1'),
	'3000', 'the standby snapshot reads the row before its delete');
my $offset = -s $standby->logfile;
$node->safe_psql('postgres',
	q{DELETE FROM followed WHERE k = 1; SELECT doltgres_test_fold('followed'); VACUUM followed;
DELETE FROM replayed; SELECT doltgres_test_fold('replayed'); VACUUM replayed});
$node->wait_for_replay_catchup($standby);
ok($standby->wait_for_log(qr/terminating connection due to conflict with recovery/, $offset),
	'the replayed free cancels the standby snapshot that may read the row');
eval { $reader->quit };
is(held_values('followed', $standby), held_values('followed'),
	'the standby\'s map equals the primary\'s after the replay');
is(held_values('followed'), 1, 'the primary holds the live row\'s value');
is(held_values('replayed', $standby), held_values('replayed'),
	'the standby\'s map of a row written at a spent take and freed equals the primary\'s');
is(held_values('replayed'), 0, 'the primary frees that row\'s value once');
$standby->stop;

# PRevPF 8's P1: a fold of the TOAST storage between the free's reads of two
# values' base entries moves the TOAST base, so the free reads both again, and
# every value's count ends at the rows that name it.
$node->safe_psql(
	'postgres', q{
CREATE TABLE twice (k int4 PRIMARY KEY, v text, w text);
ALTER TABLE twice ALTER COLUMN v SET STORAGE EXTERNAL;
ALTER TABLE twice ALTER COLUMN w SET STORAGE EXTERNAL;
INSERT INTO twice VALUES (1, repeat('v', 3000), repeat('w', 3000)), (2, repeat('x', 3000), repeat('y', 3000));
DELETE FROM twice WHERE k = 1;
SELECT doltgres_test_fold('twice');
});
$node->safe_psql('postgres', "SELECT injection_points_attach('doltgres-vacuum-based', 'wait')");
my $based = $node->background_psql('postgres', on_error_stop => 0);
$based->query_until(qr/start/, "\\echo start\nVACUUM twice;\n");
$node->wait_for_event('client backend', 'doltgres-vacuum-based');
$node->safe_psql('postgres',
	q{SELECT doltgres_test_fold(reltoastrelid::regclass) FROM pg_class WHERE relname = 'twice'});
$node->safe_psql('postgres', q{
SELECT injection_points_detach('doltgres-vacuum-based');
SELECT injection_points_wakeup('doltgres-vacuum-based');
});
ok( $node->poll_query_until(
		'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity WHERE query = 'VACUUM twice;' AND state = 'active'}),
	'the VACUUM paused between its reads of two values finishes');
$based->quit;
is(held_values('twice'), 2, 'a free whose TOAST storage was folded between its reads takes its values down once');
is($node->safe_psql('postgres', 'SELECT k, length(v), length(w) FROM twice'), '2|3000|3000', 'the live row reads both values');

# PRevPF 8's P2: an update that prunes its page kills the tuple id of a row an
# earlier update replaced, and VACUUM's later free of that tuple id takes the
# killed row's values down, so deleting every row frees every value.
$node->safe_psql(
	'postgres', q{
CREATE TABLE pruning (k int4 PRIMARY KEY, v text, w text);
ALTER TABLE pruning ALTER COLUMN w SET STORAGE EXTERNAL;
INSERT INTO pruning SELECT k, repeat('x', 1000), repeat(chr(64 + k), 3000) FROM generate_series(1, 14) k;
UPDATE pruning SET v = repeat('y', 1000) WHERE k = 1;
UPDATE pruning SET v = repeat('z', 1000) WHERE k = 3;
SELECT doltgres_test_fold('pruning');
VACUUM pruning;
});
is(held_values('pruning'), 14, 'the rows hold their values after an update prunes its page');
$node->safe_psql('postgres', q{DELETE FROM pruning; SELECT doltgres_test_fold('pruning'); VACUUM pruning});
is(held_values('pruning'), 0, 'a row a prune killed frees its values with the rest');

# PRevPF 8's Q1: a toasted insert refused by ON CONFLICT DO NOTHING, and one
# refused by a unique violation a savepoint catches under a top that commits,
# leave no value behind.
made('refused', 1);
$node->safe_psql('postgres',
	q{INSERT INTO refused VALUES (1, repeat('n', 3000)) ON CONFLICT DO NOTHING});
$node->safe_psql(
	'postgres', q{
DO $$
BEGIN
	BEGIN
		INSERT INTO refused VALUES (1, repeat('s', 3000));
	EXCEPTION WHEN unique_violation THEN NULL;
	END;
END
$$;
});
is(held_values('refused'), 1, 'refused inserts add no value');
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('refused'); VACUUM refused});
is(held_values('refused'), 1, 'after a fold and VACUUM only the live row\'s value is held');
$node->safe_psql('postgres', q{DELETE FROM refused; SELECT doltgres_test_fold('refused'); VACUUM refused});
is(held_values('refused'), 0, 'the live row\'s value goes with it');

# A move carries a deleted row's values mark with its free space versions (PLAN's
# kind 12): VACUUM's free in the new tablespace takes the value down, after a
# crash that replays the move too.
made('moved', 3);
$node->safe_psql(
	'postgres', q{
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE frees_moved LOCATION '';
DELETE FROM moved WHERE k = 1;
ALTER TABLE moved SET TABLESPACE frees_moved;
});
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', q{SELECT doltgres_test_fold('moved'); VACUUM moved});
is(held_values('moved'), 2, 'VACUUM frees a moved table\'s deleted row\'s value');

$node->stop;
done_testing();
