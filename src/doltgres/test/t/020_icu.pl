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

# The vendored ICU (design item 12), which Postgres' ICU support is built
# against alone, with its filtered data compiled in: a cluster made with an
# ICU locale answers each statement here as stock Postgres 19 with a full
# ICU of the same version answers it. The statements are ASCII: text goes in
# as Unicode escapes and comes out as the hex of its UTF-8 bytes. Each
# expectation was read from the reference.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('icu');
$node->init(
	extra => [ '--locale-provider=icu', '--icu-locale=en-US', '--encoding=UTF8' ]);
$node->start;

is( $node->safe_psql(
		'postgres',
		q{SELECT datlocprovider, datlocale FROM pg_database WHERE datname = 'postgres'}
	),
	'i|en-US',
	'the cluster is made with an ICU locale');

# Each statement, what the reference answers, and what the case shows.
my @cases = (
	[
		q{SELECT count(*) FROM pg_collation WHERE collprovider = 'i'},
		q{908},
		"the vendored ICU lists the collations a full ICU lists"
	],
	[
		q{SELECT string_agg(collname || '=' || collversion, ',' ORDER BY collname) FROM pg_collation WHERE collname IN ('de-x-icu', 'en-x-icu', 'sv-x-icu', 'und-x-icu', 'zh-x-icu')},
		q{de-x-icu=153.144,en-x-icu=153.144,sv-x-icu=153.144.49,und-x-icu=153.144,zh-x-icu=153.144.49},
		"collations report the vendored collator's versions"
	],
	[
		q{SELECT 'a' < 'B' COLLATE "en-x-icu", 'a' < 'B' COLLATE "C"},
		q{t|f},
		"an ICU collation orders letters before case, and C by bytes"
	],
	[
		q{SELECT encode(convert_to(string_agg(x, ',' ORDER BY x COLLATE "sv-x-icu"), 'UTF8'), 'hex') FROM (VALUES ('z'), (E'\u00e4'), ('a'), (E'\u00f6'), (E'\u00e5'), ('Z'), ('o')) v(x)},
		q{612c6f2c7a2c5a2cc3a52cc3a42cc3b6},
		"Swedish order"
	],
	[
		q{SELECT encode(convert_to(string_agg(x, ',' ORDER BY x COLLATE "de-x-icu"), 'UTF8'), 'hex') FROM (VALUES ('z'), (E'\u00e4'), ('a'), (E'\u00f6'), (E'\u00e5'), ('Z'), ('o')) v(x)},
		q{612cc3a52cc3a42c6f2cc3b62c7a2c5a},
		"German order"
	],
	[
		q{SELECT encode(convert_to(string_agg(x, ',' ORDER BY x COLLATE "C"), 'UTF8'), 'hex') FROM (VALUES ('z'), (E'\u00e4'), ('a'), (E'\u00f6'), (E'\u00e5'), ('Z'), ('o')) v(x)},
		q{5a2c612c6f2c7a2cc3a42cc3a52cc3b6},
		"byte order"
	],
	[
		q{SELECT encode(convert_to(string_agg(x, ',' ORDER BY x COLLATE "zh-x-icu"), 'UTF8'), 'hex') FROM (VALUES (E'\u4e2d'), (E'\u6587'), (E'\u6392'), (E'\u5e8f'), ('abc')) v(x)},
		q{e68e922ce696872ce5ba8f2ce4b8ad2c616263},
		"pinyin order"
	],
	[
		q{SELECT encode(convert_to(string_agg(x, ',' ORDER BY x COLLATE "th-x-icu"), 'UTF8'), 'hex') FROM (VALUES (E'\u0e44\u0e01\u0e48'), (E'\u0e01\u0e32'), (E'\u0e40\u0e01'), (E'\u0e02\u0e32')) v(x)},
		q{e0b881e0b8b22ce0b980e0b8812ce0b984e0b881e0b9882ce0b882e0b8b2},
		"Thai order"
	],
	[
		q{SELECT encode(convert_to(upper(E'\u00e9\u00dfi' COLLATE "en-x-icu"), 'UTF8'), 'hex')},
		q{c389535349},
		"upper case in English"
	],
	[
		q{SELECT encode(convert_to(upper('i' COLLATE "tr-x-icu") || lower(E'\u0130I' COLLATE "tr-x-icu"), 'UTF8'), 'hex')},
		q{c4b069c4b1},
		"upper and lower case in Turkish"
	],
	[
		q{SELECT encode(convert_to(initcap(E'hello wOrld \u01c6x' COLLATE "en-x-icu"), 'UTF8'), 'hex')},
		q{48656c6c6f20576f726c6420c78578},
		"title case in English"
	],
	[
		q{SELECT encode(convert_to(initcap(E'\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35\u0e04\u0e23\u0e31\u0e1a \u0e20\u0e32\u0e29\u0e32\u0e44\u0e17\u0e22' COLLATE "th-x-icu"), 'UTF8'), 'hex')},
		q{e0b8aae0b8a7e0b8b1e0b8aae0b894e0b8b5e0b884e0b8a3e0b8b1e0b89a20e0b8a0e0b8b2e0b8a9e0b8b2e0b984e0b897e0b8a2},
		"title case of Thai text, whose words the break dictionaries find"
	],
	[
		q{SELECT encode(convert_to(initcap(E'\u4e2d\u6587 \u6392\u5e8f' COLLATE "zh-x-icu"), 'UTF8'), 'hex')},
		q{e4b8ade6968720e68e92e5ba8f},
		"title case of Han text"
	],
	[
		q{CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false); SELECT 'ABC' = 'abc' COLLATE ci, 'a' < 'B' COLLATE ci},
		q{t|t},
		"a nondeterministic collation compares without case"
	],
	[
		q{CREATE COLLATION digits_last (provider = icu, locale = 'und', rules = '&a < 9 < 8'); SELECT string_agg(x, ',' ORDER BY x COLLATE digits_last) FROM (VALUES ('8'), ('9'), ('a'), ('b')) v(x)},
		q{a,9,8,b},
		"a collation made from rules orders by them"
	],
	[
		q{SELECT E'\u314e' ~ '[[:alpha:]]' COLLATE "en-x-icu", E'\u0663' ~ '[[:digit:]]' COLLATE "en-x-icu"},
		q{t|t},
		"character classes under an ICU collation"
	],
	[
		q{SELECT encode(convert_to(string_agg(x, ',' ORDER BY x COLLATE "fr-CA-x-icu"), 'UTF8'), 'hex') FROM (VALUES (E'r\u00e9sum\u00e9'), ('resume'), (E'R\u00e9sum\u00e9'), ('RESUME')) v(x)},
		q{726573756d652c524553554d452c72c3a973756dc3a92c52c3a973756dc3a9},
		"French Canadian accent order"
	],
	[
		q{CREATE COLLATION named_set (PROVIDER = icu, LOCALE = 'und', RULES = '&a < 9 [suppressContractions [[:name=LATIN SMALL LETTER C:]]]'); SELECT string_agg(x, ',' ORDER BY x COLLATE named_set) FROM (VALUES ('8'), ('9'), ('a'), ('b')) v(x)},
		q{8,a,9,b},
		"a collation made from rules that give a set of characters by a character's name"
	]);

foreach my $case (@cases)
{
	my ($sql, $answer, $name) = @$case;
	is($node->safe_psql('postgres', $sql), $answer, $name);
}

$node->stop;
done_testing();
