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

# A libc locale's stored names on the vendored collator (design item 12): a
# libc collation orders, compares, and maps case as the collator of the tag its
# name stands for, in every form of name (POSIX's, a language tag, Windows',
# glibc's aliases) and on every platform; each half of a pair keeps its own
# name's rules; a name the tag holds nothing for warns, and so does a tag ICU
# doesn't know, at the validation's setting capped at a warning. The names go
# straight into the catalog, as an older cluster or another platform stores
# them, since no C library here is asked. Each expectation is the same node's
# answer for the tag's ICU collation or for the builtin C.UTF-8, which
# 020_icu.pl and 021_collator.pl hold against the reference. The statements
# are ASCII: text goes in as Unicode escapes and comes out as the hex of its
# UTF-8 bytes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('libc_locale');
$node->init(extra => [ '--encoding=UTF8', '--locale=C' ]);
$node->start;

# libc stores a libc collation of a collate name and a ctype name in a
# database, the postgres one unless another is named, as a row of the
# catalog's own form.
my $oid = 70000;

sub libc
{
	my ($name, $collate, $ctype, $database) = @_;
	$node->safe_psql($database // 'postgres',
		"INSERT INTO pg_collation (oid, collname, collnamespace, collowner, collprovider, collisdeterministic, collencoding, collcollate, collctype) VALUES ($oid, '$name', 'pg_catalog'::regnamespace, 10, 'c', true, -1, '$collate', '$ctype')"
	);
	$oid++;
	return;
}

# The words each order and case mapping is read over: Swedish's letters after
# z, German's sharp s, Turkish's dotted and dotless i, and ASCII.
my $words =
  q{(VALUES ('z'), (E'\u00e4'), ('a'), ('A'), (E'\u00f6'), (E'\u00e5'), ('o'), (E'\u00df'), ('i'), ('I'), (E'\u0131'), (E'\u0130'), ('b')) v(x)};

# same returns whether a collation orders, compares, and maps case over the
# words as another does: its order, each word against "b", and upper and lower.
sub same
{
	my ($collation, $as) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT (string_agg(x, ',' ORDER BY x COLLATE "$collation") COLLATE "C") = (string_agg(x, ',' ORDER BY x COLLATE "$as") COLLATE "C"), bool_and((x < 'b' COLLATE "$collation") = (x < 'b' COLLATE "$as")), bool_and((upper(x COLLATE "$collation") COLLATE "C") = (upper(x COLLATE "$as") COLLATE "C") AND (lower(x COLLATE "$collation") COLLATE "C") = (lower(x COLLATE "$as") COLLATE "C")) FROM $words}
	);
}

# ordered returns whether a collation orders the words as another does.
sub ordered
{
	my ($collation, $as) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT (string_agg(x, ',' ORDER BY x COLLATE "$collation") COLLATE "C") = (string_agg(x, ',' ORDER BY x COLLATE "$as") COLLATE "C") FROM $words}
	);
}

# cased returns whether a collation maps the words' case as another does.
sub cased
{
	my ($collation, $as) = @_;
	return $node->safe_psql('postgres',
		qq{SELECT bool_and((upper(x COLLATE "$collation") COLLATE "C") = (upper(x COLLATE "$as") COLLATE "C") AND (lower(x COLLATE "$collation") COLLATE "C") = (lower(x COLLATE "$as") COLLATE "C")) FROM $words}
	);
}

# Every form of a name stands for its tag: POSIX's, the tag itself, Windows',
# and glibc's alias, whatever its letters' case.
libc('sv_posix', 'sv_SE.UTF-8', 'sv_SE.UTF-8');
libc('sv_tag', 'sv-SE', 'sv-SE');
libc('sv_windows', 'Swedish_Sweden.1252', 'Swedish_Sweden.1252');
libc('sv_glibc', 'Swedish', 'Swedish');
for my $name (qw(sv_posix sv_tag sv_windows sv_glibc))
{
	is(same($name, 'sv-x-icu'), 't|t|t',
		"a libc collation orders, compares, and maps case as the collator of its name's tag"
	);
}
isnt(ordered('sv_posix', 'de-x-icu'),
	't', "and not as another language's collator");

# A libc collation's version is the collator's for its tag.
is( $node->safe_psql(
		'postgres',
		q{SELECT pg_collation_actual_version(c.oid) = pg_collation_actual_version(i.oid) FROM pg_collation c, pg_collation i WHERE c.collname = 'sv_posix' AND i.collname = 'sv-x-icu'}
	),
	't',
	"a libc collation reports the collator's version");

# A modifier the tag holds nothing for is said in a warning, and the name is
# its language's.
libc('sv_modified', 'sv_SE.UTF-8@nonsense', 'sv_SE.UTF-8@nonsense');
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	qq{\\set VERBOSITY verbose\nSELECT string_agg(x, ',' ORDER BY x COLLATE "sv_modified") = string_agg(x, ',' ORDER BY x COLLATE "sv-x-icu") FROM $words}
);
is($stdout, 't', "a name with an unknown modifier warns and is its language's");
like(
	$stderr,
	qr/WARNING:  22023: locale name "sv_SE.UTF-8\@nonsense" has unknown modifier "nonsense"\nDETAIL:  The locale is ordered as "sv-SE"./,
	"a name with an unknown modifier warns and is its language's");

# A tag ICU doesn't know warns at the setting's level, capped at a warning so
# no setting keeps a database from opening, and says nothing when validation
# is disabled; the setting is unchanged. Each leg opens the locale in a session
# of its own, since a session keeps a locale it opened.
libc('unknown_language', 'xx_YY.UTF-8', 'xx_YY.UTF-8');
for my $leg ([ 'warning', 1 ], [ 'error', 1 ], [ 'disabled', 0 ])
{
	my ($level, $warns) = @$leg;
	($ret, $stdout, $stderr) = $node->psql('postgres',
		qq{SET icu_validation_level = $level;\nSELECT 'a' < 'b' COLLATE "unknown_language";\nSHOW icu_validation_level});
	is($stdout, "t\n$level",
		"the validation's level is the setting's, capped at a warning, and the setting is unchanged"
	);
	is( $stderr =~ /WARNING:  ICU locale "xx-YY" has unknown language "xx"/
		? 1
		: 0,
		$warns,
		"the validation's level is the setting's, capped at a warning, and the setting is unchanged"
	);
}

# Two names of different languages keep each half's own: Swedish's order and
# Turkish's case mapping.
libc('sv_tr', 'sv_SE.UTF-8', 'tr_TR.UTF-8');
is(ordered('sv_tr', 'sv-x-icu'), 't',
	"two names of different languages keep each half's own");
is(cased('sv_tr', 'tr-x-icu'), 't',
	"two names of different languages keep each half's own");
isnt(cased('sv_tr', 'sv-x-icu'),
	't', "two names of different languages keep each half's own");

# A collate name of C orders by bytes, and a named ctype maps case by its name.
libc('c_tr', 'C', 'tr_TR.UTF-8');
is(ordered('c_tr', 'C'), 't',
	"a collate name of C with a named ctype orders by bytes and maps case by the name"
);
is(cased('c_tr', 'tr-x-icu'), 't',
	"a collate name of C with a named ctype orders by bytes and maps case by the name"
);

# A ctype name of C maps only ASCII's case, and a named collate name orders by
# its name.
libc('sv_c', 'sv_SE.UTF-8', 'C');
is(ordered('sv_c', 'sv-x-icu'), 't',
	"a ctype name of C with a named collate name orders by the name and maps only ASCII's case"
);
is( $node->safe_psql(
		'postgres',
		q{SELECT encode(convert_to(upper(E'\u00e4i\u00df' COLLATE "sv_c"), 'UTF8'), 'hex')}),
	'c3a449c39f',
	"a ctype name of C with a named collate name orders by the name and maps only ASCII's case"
);

# A ctype name of C.UTF-8 classifies and maps case as the builtin C.UTF-8 does,
# over every code point of the first three planes' first 12,287 (none a
# surrogate), and its simple case mapping leaves a sharp s as it is where the
# collator's full mapping makes it SS; a named collate name orders by its name.
libc('sv_c_utf8', 'sv_SE.UTF-8', 'C.UTF-8');
is(ordered('sv_c_utf8', 'sv-x-icu'), 't',
	"a ctype name of C.UTF-8 with a named collate name orders by the name and classifies as the builtin C.UTF-8 does"
);
is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM generate_series(1, 12287) AS c, LATERAL (SELECT chr(c) COLLATE "sv_c_utf8" AS p, chr(c) COLLATE "pg_c_utf8" AS b) s WHERE (upper(p) COLLATE "C") <> (upper(b) COLLATE "C") OR (lower(p) COLLATE "C") <> (lower(b) COLLATE "C") OR (initcap(p) COLLATE "C") <> (initcap(b) COLLATE "C") OR (p ~ '[[:alpha:]]') <> (b ~ '[[:alpha:]]') OR (p ~ '[[:digit:]]') <> (b ~ '[[:digit:]]') OR (p ~ '[[:alnum:]]') <> (b ~ '[[:alnum:]]') OR (p ~ '[[:upper:]]') <> (b ~ '[[:upper:]]') OR (p ~ '[[:lower:]]') <> (b ~ '[[:lower:]]') OR (p ~ '[[:punct:]]') <> (b ~ '[[:punct:]]') OR (p ~ '[[:space:]]') <> (b ~ '[[:space:]]') OR (p ~ '[[:print:]]') <> (b ~ '[[:print:]]') OR (p ~ '[[:graph:]]') <> (b ~ '[[:graph:]]') OR (p ~ '\w') <> (b ~ '\w')}
	),
	'0',
	"a ctype name of C.UTF-8 with a named collate name orders by the name and classifies as the builtin C.UTF-8 does"
);
is( $node->safe_psql(
		'postgres',
		q{SELECT encode(convert_to(upper(E'\u00df' COLLATE "sv_c_utf8"), 'UTF8'), 'hex'), encode(convert_to(upper(E'\u00df' COLLATE "sv-x-icu"), 'UTF8'), 'hex')}
	),
	'c39f|5353',
	"a ctype name of C.UTF-8 with a named collate name orders by the name and classifies as the builtin C.UTF-8 does"
);

# A ctype name of C.UTF-8 in a SQL_ASCII database, the one database of another
# encoding Postgres lets it into, classifies as C: stock Postgres takes the
# C library's single-byte functions there, under which glibc's C.UTF-8 finds no
# letter above 127 and maps only ASCII's case. macOS's C library reads such a
# byte as its Latin-1 code point (isalpha_l of 0xE9 is 1 there), so stock
# Postgres on macOS answers otherwise; the fork gives glibc's answer on every
# platform. The case is a collation of that database, not its own default:
# Postgres' setlocale of a database's names at connection is unchanged, so a
# database whose names the operating system lacks opens nowhere else, which is
# Order 5's question with the remotes.
$node->safe_psql('postgres',
	q{CREATE DATABASE ascii TEMPLATE template0 ENCODING 'SQL_ASCII' LOCALE 'C'});
libc('ascii_c_utf8', 'sv_SE.UTF-8', 'C.UTF-8', 'ascii');
is( $node->safe_psql(
		'ascii',
		q{SELECT count(*) FILTER (WHERE chr(c) COLLATE "ascii_c_utf8" ~ '[[:alpha:]]'), count(*) FILTER (WHERE upper(chr(c) COLLATE "ascii_c_utf8") <> chr(c)) FROM generate_series(1, 255) AS c}
	),
	'52|26',
	"a ctype name of C.UTF-8 in a SQL_ASCII database classifies as C, as glibc's C.UTF-8 does there"
);

# A libc pair of the C family but C and POSIX alone is the builtin provider's
# locale its ctype name stands for: both halves sort by bytes, a C.UTF-8 ctype
# classifies and maps case as pg_c_utf8 does over the first 12,287 code
# points, a C ctype maps only ASCII's case, a C. name of another encoding is
# C's, and in a SQL_ASCII database a C.UTF-8 ctype is C's.
my $case = "a libc pair of the C family but C and POSIX alone is the builtin provider's locale of its ctype name";
libc('c_c_utf8', 'C', 'C.UTF-8');
libc('c_utf8_both', 'C.UTF-8', 'C.utf8');
libc('c_utf8_c', 'C.UTF-8', 'C');
for my $name (qw(c_c_utf8 c_utf8_both))
{
	is(ordered($name, 'C'), 't', $case);
	is( $node->safe_psql(
			'postgres',
			qq{SELECT count(*) FROM generate_series(1, 12287) AS c, LATERAL (SELECT chr(c) COLLATE "$name" AS p, chr(c) COLLATE "pg_c_utf8" AS b) s WHERE (upper(p) COLLATE "C") <> (upper(b) COLLATE "C") OR (lower(p) COLLATE "C") <> (lower(b) COLLATE "C") OR (initcap(p) COLLATE "C") <> (initcap(b) COLLATE "C") OR (p ~ '[[:alpha:]]') <> (b ~ '[[:alpha:]]') OR (p ~ '[[:upper:]]') <> (b ~ '[[:upper:]]') OR (p ~ '[[:punct:]]') <> (b ~ '[[:punct:]]') OR (p ~ '[[:space:]]') <> (b ~ '[[:space:]]') OR (p ~ '\\w') <> (b ~ '\\w')}
		),
		'0',
		$case);
}
is(ordered('c_utf8_c', 'C'), 't', $case);
is( $node->safe_psql(
		'postgres',
		q{SELECT encode(convert_to(upper(E'\u00e4i' COLLATE "c_utf8_c"), 'UTF8'), 'hex')}),
	'c3a449',
	$case);
libc('latin_c', 'C.ISO-8859-1', 'C');
is(ordered('latin_c', 'C'), 't', $case);
is( $node->safe_psql(
		'postgres',
		q{SELECT encode(convert_to(upper(E'\u00e4i' COLLATE "latin_c"), 'UTF8'), 'hex')}),
	'c3a449',
	$case);
libc('ascii_c_family', 'C', 'C.UTF-8', 'ascii');
is( $node->safe_psql(
		'ascii',
		q{SELECT count(*) FILTER (WHERE chr(c) COLLATE "ascii_c_family" ~ '[[:alpha:]]'), count(*) FILTER (WHERE upper(chr(c) COLLATE "ascii_c_family") <> chr(c)) FROM generate_series(1, 255) AS c}
	),
	'52|26',
	$case);

# A new collation's names are asked of the C library first, as Postgres asks
# them, so a name no C library knows is refused with Postgres' own error and
# the collation isn't made; the names stored above never were, nor is a stored
# name renamed, nor one a new collation copies. The refused load leaves the
# session's stored names as they were.
libc('nonsense_stored', 'nonsense', 'nonsense');
is( $node->safe_psql(
		'postgres', q{SET icu_validation_level = disabled; SELECT 'a' < 'B' COLLATE nonsense_stored}),
	't',
	'a stored name no C library knows keeps its order');
($ret, $stdout, $stderr) = $node->psql('postgres',
	qq{\\set VERBOSITY verbose\nCREATE COLLATION nonsense_new (provider = libc, locale = 'nonsense');\nSET icu_validation_level = disabled;\nSELECT 'a' < 'B' COLLATE nonsense_stored},
	on_error_stop => 0);
like(
	$stderr,
	qr/ERROR:  22023: could not create locale "nonsense"/,
	"a new collation of a name the C library doesn't know raises Postgres' error");
is($stdout, 't', "a refused new collation leaves the session's stored names as they were");
is( $node->safe_psql(
		'postgres', q{SELECT count(*) FROM pg_collation WHERE collname = 'nonsense_new'}),
	'0',
	'the refused collation is not made');
is( $node->safe_psql(
		'postgres',
		q{SET icu_validation_level = disabled; BEGIN; ALTER COLLATION nonsense_stored RENAME TO nonsense_renamed; SELECT 'a' < 'B' COLLATE nonsense_renamed; COMMIT}
	),
	't',
	'a stored collation renamed in a transaction keeps its order');
is( $node->safe_psql(
		'postgres',
		q{SET icu_validation_level = disabled; CREATE COLLATION copied_stored FROM nonsense_renamed; SELECT 'a' < 'B' COLLATE copied_stored}
	),
	't',
	'a new collation copied from a stored name no C library knows keeps its order');

# A new collation of a name the C library knows is the vendored collator's in
# the session and transaction that make it, as in every later one: its case
# mapping and its order where the platform's C library differs (Turkish's
# dotted capital I, and a hyphen most C libraries order past).
my $turkish = $windows_os ? 'tr-TR' : 'tr_TR.UTF-8';
my $mapped =
  q{SELECT encode(convert_to(upper('i' COLLATE tr_new) || string_agg(x, ',' ORDER BY x COLLATE tr_new), 'UTF8'), 'hex') FROM (VALUES ('coal'), ('co-op')) v(x)};
($ret, $stdout, $stderr) = $node->psql('postgres',
	qq{BEGIN;\nCREATE COLLATION tr_new (provider = libc, locale = '$turkish');\nSET icu_validation_level = disabled;\n$mapped;\nCOMMIT;});
SKIP:
{
	skip "the C library has no locale $turkish", 1 if $stderr =~ /could not create locale/;
	is( $stdout,
		$node->safe_psql('postgres', "SET icu_validation_level = disabled; $mapped"),
		'a new collation maps case and orders in its own transaction as in a later session');
}

$node->stop;
done_testing();
