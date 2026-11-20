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

# The three statements the fork trusts the vendored ICU for (design item 12),
# through the calls Postgres itself makes, in a UTF8 and in a LATIN1 database,
# for a collation of each kind keys are stored under: sort keys order and
# equate strings exactly as the collator's comparison does; what the collator
# gives as the first bytes of a key is the start of the whole key; and one
# ICU gives the same keys on every platform, by the SHA-256 of a corpus's
# keys. The corpus is built from code points, so it is the same everywhere.
#
# This file is ASCII by rule: text goes in as escapes, since a character
# beyond ASCII would reach each database through the client's encoding and
# be another string there.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Each collation's name and what it is made from. All but the last compare by
# the collator alone; the last is deterministic, so Postgres orders strings
# the collator calls equal by their bytes.
my @collations = (
	[ 'root', q{LOCALE = 'und'} ],
	[ 'swedish', q{LOCALE = 'sv'} ],
	[ 'turkish', q{LOCALE = 'tr'} ],
	[ 'french_canadian', q{LOCALE = 'fr-CA'} ],
	[ 'pinyin', q{LOCALE = 'zh'} ],
	[ 'thai', q{LOCALE = 'th'} ],
	[ 'japanese', q{LOCALE = 'ja'} ],
	[ 'phone_book', q{LOCALE = 'de-u-co-phonebk'} ],
	[ 'primary_strength', q{LOCALE = 'und-u-ks-level1'} ],
	[ 'secondary_strength', q{LOCALE = 'und-u-ks-level2'} ],
	[ 'case_level', q{LOCALE = 'und-u-ks-level1-kc'} ],
	[ 'identical_strength', q{LOCALE = 'und-u-ks-identic'} ],
	[ 'shifted', q{LOCALE = 'und-u-ka-shifted'} ],
	[ 'shifted_quaternary', q{LOCALE = 'und-u-ka-shifted-ks-level4'} ],
	[ 'numeric', q{LOCALE = 'und-u-kn'} ],
	[ 'upper_first', q{LOCALE = 'und-u-kf-upper'} ],
	[ 'rule_made', q{LOCALE = 'und', RULES = '&a < 9 < 8 &c < ch <<< Ch <<< CH'} ],
	[ 'deterministic', q{LOCALE = 'en'} ]);

# Strings every database's corpus holds: ones that differ at the secondary
# level alone (accents) and at the tertiary level alone (case), ones with
# ignorable and variable characters, digits, contractions, and expansions.
my @narrow = (
	q{''}, q{'a'}, q{'A'}, q{'b'}, q{'abc'}, q{'Abc'}, q{'aBc'}, q{'ABC'},
	q{'role'}, q{E'r\u00f4le'}, q{E'rol\u00e9'}, q{E'r\u00f4l\u00e9'},
	q{'resume'}, q{E'r\u00e9sum\u00e9'}, q{E'R\u00e9sum\u00e9'}, q{'RESUME'},
	q{E'ab\u00adc'}, q{'a b'}, q{'a-b'}, q{'a.b'}, q{'ab'}, q{'a  b'},
	q{'co-op'}, q{'coop'}, q{'co op'}, q{'2'}, q{'02'}, q{'9'}, q{'10'},
	q{'a2'}, q{'a10'}, q{'8'}, q{'c'}, q{'ch'}, q{'Ch'}, q{'CH'}, q{'cz'},
	q{'d'}, q{E'\u00e4'}, q{'ae'}, q{E'\u00f6'}, q{'oe'}, q{'z'}, q{'i'},
	q{'I'}, q{E'\u00df'}, q{'ss'}, q{'SS'}, q{E'\u00e6'}, q{E'\u00c6'},
	q{repeat('a', 200)}, q{repeat('a', 200) || 'b'},
	q{repeat(E'\u00e9', 90)});

# Strings only a UTF8 database's corpus holds: a letter with its accent apart
# and joined, ignorable characters, letters that differ in kind alone, and
# text of other scripts and beyond the first plane.
my @wide = (
	q{E'e\u0301'}, q{E'\u00e9'}, q{E'a\u200bb'}, q{E'a\u2060b'},
	q{E'\u0130'}, q{E'\u0131'}, q{E'\u1e9e'}, q{E'\ufb01'}, q{'fi'},
	q{E'\u3072'}, q{E'\u30d2'}, q{E'\uff8b'}, q{E'\u3073'},
	q{E'\u0e01\u0e32'}, q{E'\u0e40\u0e01'}, q{E'\u0e44\u0e01\u0e48'},
	q{E'\u4e2d'}, q{E'\u6587'}, q{E'\u4e2d\u6587'}, q{E'\U0001F600'},
	q{E'\U0001F1E6\U0001F1FA'}, q{E'\u0627\u0644'}, q{E'\u05d0\u05b8'});

# What each database's corpus is generated from besides its strings: where
# each block of code points starts, and how many it holds.
my %blocks = (
	postgres => [
		'32, 160, 880, 1024, 1488, 1536, 3585, 12353, 19968, 44032, 127744, 768',
		'95, 352, 143, 256, 27, 256, 58, 180, 20000, 11000, 768, 112'
	],
	latin1 => [ '32, 65, 97, 160, 192, 224', '95, 26, 26, 96, 64, 32' ]);

# Each database's recorded answers, by collation: how many pairs of different
# strings have equal keys, and the SHA-256 of the corpus's keys. They were
# read from the fork's build on Windows, the first platform this test ran on;
# every other platform's run of this file compares its keys with them.
my %recorded = (
	postgres => {
		root => '5 8c3c6a77d2dec0c57f07994385e57ad7953a7557f98054afa2200e0ebdb1160b',
		swedish => '5 241e547f38d327c28eb5cccb766ec6c68dedac72cf6b8e5f976cc8d420a2f7c9',
		turkish => '5 3490d48b942fcc842d573b49f822dd8a258b6c2534e26b5bded2cbf0a294f0e1',
		french_canadian => '5 de9936651a01d40738e26cf8f210eb3ba106a6df7e1007f877a709c660e12427',
		pinyin => '5 07e7864ea48967bcfcac7864beae7f180bc754a73369e75d3b299deb916b8661',
		thai => '41 933275608c8dd56968af33981f2f934e7b2b84cdca933bf0d00572f8f52bc8d5',
		japanese => '11 b40d7fe82d18ec2ded00efc5f2fbd1e1f054cd925676ee3600690712186e68a2',
		phone_book => '5 c9eaf056d48dfd2dca3f3628cd0d9c2ef00c1139ac47f38dd077d2067221153e',
		primary_strength => '102 5c8cf2b2623f53e4dbcff1b901d7c8cf41b8c9bdafbc6071fd274d87364d64bf',
		secondary_strength => '34 ec52392b425278261c05771f7bea27df6cb1cb7e638037ce2a26e586a91837e5',
		case_level => '68 024df68093749cb61cf572d97326a35de2ea0bc5d0024582585e0aba3b8ef3e9',
		identical_strength => '1 66cfa2a581dd6ad3f328d62d156aafb06c61c268efddcc85d4b66b8aca78f26d',
		shifted => '41 d22afc8b4c883345c81794289d5b2d44273d6ee0a5bf780a6f733f777fc20569',
		shifted_quaternary => '5 6a2e2bbbf3d7d46e5600d868890561da349db7c5ff4d48fee9fc07fc980d0082',
		numeric => '6 fb3ba4bfd10ca244d3cc4d82e38c7366585d058f81553f78f6ca7b1ab9ca6925',
		upper_first => '5 fc47717c09b07f613e99672926b206ca7a0dc5b3e32cdf4bb86bec6044232796',
		rule_made => '5 8f772c444867754e057d109192e432527c815c6c37b633b3b71ec7e2fbde1500',
		deterministic => '5 8c3c6a77d2dec0c57f07994385e57ad7953a7557f98054afa2200e0ebdb1160b',
	},
	latin1 => {
		root => '1 0efc52aef29c2eab143769d76e3911258b2ba027fd08b6af73f298b449321a71',
		swedish => '1 5976d378b9bb8ed1994fc38caabb3f6da6ee66f97b94085382071cba9ceea693',
		turkish => '1 d689fe85c446d8f834581711a4070e33edb0920c17eae27d7e6e87b1371992ff',
		french_canadian => '1 c4a4e531d2225f3f73c3a5a6571160614e093c541f65767d520c608537c9b359',
		pinyin => '1 6e331f71fe8305cac6e9d1fdc30a7447ee473ee23ae9e63737d9f6e2a939cb9d',
		thai => '30 412a068eae01c5ba012ad5a6435409ef7f049ef97421e95ba0a2d7b6e9cc93b4',
		japanese => '1 2002f86cccfc182dba695a5f612b92d2f160b7d7dd6b9e67ee5c170e7040c0a1',
		phone_book => '1 cd9c11a44cf928798bfeb1ba7503ed323f244590b2e50bfb7dcb24a4632f7e38',
		primary_strength => '90 e9a80b272bb9737161290c930bb897dcc1f89ff2a4ab3a00b13d1a2dc6fc4c9d',
		secondary_strength => '37 3bad0b84f65b99ac291c38e2853b64d382cd671a7764d916c2396368ba014ae4',
		case_level => '30 c3ed22d48bd77f5127206d2094eaf552d6420a97f2ad3a6351b1b510dbf0a1f8',
		identical_strength => '0 1e4faf833a2ef1380cc176605c697eeebb467bee274d38646e1d255e21bc67f3',
		shifted => '30 412a068eae01c5ba012ad5a6435409ef7f049ef97421e95ba0a2d7b6e9cc93b4',
		shifted_quaternary => '1 cf414d79252cbfbce3cad1e360725152bed8ed6cb4dd16edb081012200384f58',
		numeric => '2 ae1fed150471ee259e3af7b6c7e9c1c83337ae0584d8e592880b879c6327e389',
		upper_first => '1 f5924b2a6e0918caa4a4d0a46aa5f454403827ca594f5590f570625e2ef70ee2',
		rule_made => '1 de6631ff7bb623b1f9a35427b804f389b7974fc0e22a7521d4fa5154171f76df',
		deterministic => '1 0efc52aef29c2eab143769d76e3911258b2ba027fd08b6af73f298b449321a71',
	},
);

my $node = PostgreSQL::Test::Cluster->new('collator');
$node->init(
	extra => [ '--locale-provider=icu', '--icu-locale=en-US', '--encoding=UTF8' ]);
$node->start;
$node->safe_psql('postgres',
	q{CREATE DATABASE latin1 TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C' LOCALE_PROVIDER icu ICU_LOCALE 'en-US'}
);

foreach my $database ('postgres', 'latin1')
{
	my ($starts, $spans) = @{ $blocks{$database} };
	my $count = scalar(split(/,/, $starts));
	my @strings = $database eq 'postgres' ? (@narrow, @wide) : @narrow;
	my $strings = join(', ', @strings);

	$node->safe_psql($database, 'CREATE EXTENSION doltgres_test; CALL doltgres_test_twins()');
	$node->safe_psql(
		$database, qq{
		CREATE TABLE corpus AS
		SELECT (row_number() OVER (ORDER BY t COLLATE "C"))::int4 AS i, t
		FROM (SELECT string_agg(chr(first + (s * 31 + k * 17) % span), '' ORDER BY k) AS t
			  FROM generate_series(0, 179) AS s
				   CROSS JOIN LATERAL generate_series(0, s % 4) AS k
				   CROSS JOIN LATERAL (SELECT (ARRAY[$starts])[s % $count + 1] AS first,
											  (ARRAY[$spans])[s % $count + 1] AS span) AS block
			  GROUP BY s
			  UNION
			  SELECT t FROM unnest(ARRAY[$strings]) AS string(t)) AS strings});

	foreach my $collation (@collations)
	{
		my ($name, $made) = @$collation;
		my $deterministic = $name eq 'deterministic' ? 'true' : 'false';
		my ($left, $right) = ('a.k', 'b.k');
		if ($name eq 'deterministic')
		{
			$left = '(a.k, convert_to(a.t, getdatabaseencoding()))';
			$right = '(b.k, convert_to(b.t, getdatabaseencoding()))';
		}

		$node->safe_psql($database,
			qq{CREATE COLLATION $name (PROVIDER = icu, $made, DETERMINISTIC = $deterministic)});

		my $answer = $node->safe_psql(
			$database, qq{
			WITH keys AS MATERIALIZED
				(SELECT i, t, doltgres_test_sort_key(t COLLATE $name) AS k FROM corpus)
			SELECT count(*) FILTER (WHERE (a.t < b.t COLLATE $name) <> ($left < $right)
										  OR (a.t = b.t COLLATE $name) <> ($left = $right)),
				   count(*) FILTER (WHERE a.i < b.i AND a.k = b.k),
				   (SELECT count(*) FROM keys, generate_series(1, 40) AS size
					WHERE doltgres_test_sort_key_prefix(t COLLATE $name, size) <> substring(k FROM 1 FOR size)),
				   (SELECT encode(sha256(string_agg(k, '\\x00'::bytea ORDER BY i)), 'hex') FROM keys)
			FROM keys AS a, keys AS b});
		my ($parted, $equal, $prefixes, $hash) = split(/\|/, $answer);

		is($parted, '0', "$database $name: keys order and equate strings as the comparison does");
		is($prefixes, '0', "$database $name: the first bytes the collator gives are the key's");
		is("$equal $hash", $recorded{$database}{$name}, "$database $name: the keys are the recorded ones");
	}
}

# The test module's function of a key's first bytes refuses a size below zero.
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"\\set VERBOSITY verbose\nSELECT doltgres_test_sort_key_prefix('a' COLLATE root, -1)");
like(
	$stderr,
	qr/ERROR:  22023: size must not be negative/,
	'a size below zero is refused');

$node->stop;
done_testing();
