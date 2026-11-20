// Copyright 2026 Dolthub, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use harness::oid::*;
use harness::script::Cell::Text as T;
use harness::script::{A, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_collations() {
    run_scripts(&[
        ScriptTest {
            name: "ICU collations sort, compare, and case map by their language",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ',' ORDER BY v COLLATE "en-US-x-icu") FROM (VALUES ('b'), ('A'), ('a'), ('B'), ('ä'), ('Ä')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("a,A,ä,Ä,b,B")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v FROM (VALUES ('ö'), ('z'), ('o'), ('å'), ('ä')) t(v) ORDER BY v COLLATE "sv-x-icu";"#,
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT)],
                        rows: &[&[T("o")], &[T("z")], &[T("å")], &[T("ä")], &[T("ö")]],
                        tag: "SELECT 5",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'a' < 'B' COLLATE "en-US-x-icu", 'ö' < 'z' COLLATE "sv-x-icu", 'ö' < 'z' COLLATE "de-x-icu", 'é' = U&'e\0301' COLLATE "unicode", bttextcmp('é' COLLATE "unicode", U&'e\0301'), 'a10' < 'a2' COLLATE "en-x-icu";"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("bttextcmp", INT4), Column("?column?", BOOL)],
                        rows: &[&[T("t"), T("f"), T("t"), T("f"), T("94"), T("t")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT upper('istanbul ß' COLLATE "tr-x-icu"), lower('ΣΑΣ ΑΣ' COLLATE "el-x-icu"), initcap('o''neil ijssel' COLLATE "nl-x-icu"), casefold('İI' COLLATE "tr-x-icu"), casefold('ẞ' COLLATE "und-x-icu");"#,
                    expected: Expected::Rows {
                        columns: &[Column("upper", TEXT), Column("lower", TEXT), Column("initcap", TEXT), Column("casefold", TEXT), Column("casefold", TEXT)],
                        rows: &[&[T("İSTANBUL SS"), T("σας ας"), T("O'neil IJssel"), T("iı"), T("ss")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'é' ~ '^[[:alpha:]]$' COLLATE "en-x-icu", 'İ' ~* 'i' COLLATE "tr-x-icu", regexp_replace('a1 b2' COLLATE "unicode", '[[:digit:]]', '#', 'g');"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("regexp_replace", TEXT)],
                        rows: &[&[T("t"), T("f"), T("a# b#")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a' < 'B', upper('é'), initcap('ǆemal'), 'é' ~ '^[[:alpha:]]$', pg_collation_actual_version(100);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("upper", TEXT), Column("initcap", TEXT), Column("?column?", BOOL), Column("pg_collation_actual_version", TEXT)],
                        rows: &[&[T("f"), T("É"), T("Ǆemal"), T("t"), T("1")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_collation_actual_version(963), icu_unicode_version();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_collation_actual_version", TEXT), Column("icu_unicode_version", TEXT)],
                        rows: &[&[T("153.136"), T("17.0")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    skip: Some("ICU collations report the version of Doltgrust's ICU4X data, 2.3.0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_collation_actual_version(1);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "collation with OID 1 does not exist", ..E }),
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE DATABASE with locales",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DATABASE d1 LOCALE 'en_US.UTF-8' TEMPLATE template0;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: r#"invalid locale name "en_US.UTF-8" for builtin provider"#, ..E }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d3 LOCALE_PROVIDER icu ICU_LOCALE 'en_US' TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    notices: &[Diagnostic { code: "00000", message: r#"using standard form "en-US" for ICU locale "en_US""#, ..N }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d4 LOCALE_PROVIDER icu LOCALE 'de-DE-u-co-phonebk' TEMPLATE template0;",
                    expected: Expected::Error(Diagnostic {
                        code: "42809",
                        message: r#"invalid LC_COLLATE locale name: "de-DE-u-co-phonebk""#,
                        hint: "If the locale name is specific to the ICU provider, use ICU_LOCALE.",
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d5 LOCALE_PROVIDER icu TEMPLATE template0;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "LOCALE or ICU_LOCALE must be specified", ..E }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d7 LOCALE_PROVIDER builtin BUILTIN_LOCALE 'PG_UNICODE_FAST' TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d8 LOCALE 'xx_YY.UTF-8' TEMPLATE template0;",
                    expected: Expected::Error(Diagnostic {
                        code: "42809",
                        message: r#"invalid LC_COLLATE locale name: "xx_YY.UTF-8""#,
                        hint: "If the locale name is specific to the builtin provider, use BUILTIN_LOCALE.",
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d9 LOCALE 'en_US.ISO8859-1' TEMPLATE template0;",
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"encoding "UTF8" does not match locale "en_US.ISO8859-1""#,
                        detail: r#"The chosen LC_CTYPE setting requires encoding "LATIN1"."#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d10 LC_COLLATE 'C' LC_CTYPE 'tr_TR.UTF-8' LOCALE_PROVIDER libc TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d11 LOCALE_PROVIDER icu ICU_LOCALE 'xx' TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    notices: &[Diagnostic {
                        severity: "WARNING",
                        code: "22023",
                        message: r#"ICU locale "xx" has unknown language "xx""#,
                        hint: r#"To disable ICU locale validation, set the parameter "icu_validation_level" to "disabled"."#,
                        ..N
                    }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d12 LOCALE_PROVIDER icu ICU_LOCALE 'en' ICU_RULES '&a < b' TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d13 ICU_LOCALE 'en' TEMPLATE template0;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "ICU locale cannot be specified unless locale provider is ICU", ..E }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d14 LOCALE_PROVIDER libc LOCALE 'sv_SE.UTF-8' TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d17 TEMPLATE d3;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d18 LOCALE_PROVIDER icu ICU_LOCALE 'fr' TEMPLATE d3;",
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: "new ICU locale (fr) is incompatible with the ICU locale of the template database (en-US)",
                        hint: "Use the same ICU locale as in the template database, or use template0 as template.",
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE d20 LOCALE_PROVIDER libc LOCALE 'C.UTF-8' TEMPLATE template0;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE COLLATION and nondeterministic collations",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION ai (provider = icu, locale = '@colStrength=primary', deterministic = false);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    notices: &[Diagnostic { code: "00000", message: r#"using standard form "und-u-ks-level1" for ICU locale "@colStrength=primary""#, ..N }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION de_pb (provider = icu, locale = 'de@collation=phonebook');",
                    expected: Expected::Tag("CREATE COLLATION"),
                    notices: &[Diagnostic { code: "00000", message: r#"using standard form "de-u-co-phonebk" for ICU locale "de@collation=phonebook""#, ..N }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION lc (provider = libc, locale = 'sv_SE.UTF-8');",
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION reordered (provider = icu, locale = 'und-u-kr-grek-latn');",
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION ruled (provider = icu, locale = 'und', rules = '&a < g');",
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION nd_libc (provider = libc, locale = 'C', deterministic = false);",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "nondeterministic collations not supported with this provider", ..E }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION xx (provider = libc, locale = 'xx_YY.UTF-8');",
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not create locale "xx_YY.UTF-8": No such file or directory"#,
                        detail: r#"The operating system could not find any locale data for the locale name "xx_YY.UTF-8"."#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE nd2 (id int PRIMARY KEY, v text COLLATE ci);",
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO nd2 VALUES (1, 'a'), (2, 'A'), (3, 'b'), (4, 'B'), (5, 'á'), (6, 'ä'), (7, 'Á');",
                    expected: Expected::Tag("INSERT 0 7"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v, count(*) FROM nd2 GROUP BY v ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TEXT), Column("count", INT8)],
                        rows: &[&[T("a"), T("2")], &[T("á"), T("2")], &[T("ä"), T("1")], &[T("b"), T("2")]],
                        tag: "SELECT 4",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(DISTINCT v), string_agg(v, ',' ORDER BY v) FROM nd2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("string_agg", TEXT)],
                        rows: &[&[T("4"), T("A,a,Á,á,ä,B,b")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a' = 'A' COLLATE ci, 'a' = 'á' COLLATE ci, 'a' = 'á' COLLATE ai, hashtext('a' COLLATE ci) = hashtext('A' COLLATE ci);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[&[T("t"), T("f"), T("t"), T("t")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strpos('bA' COLLATE ci, 'a'), strpos('xxA' COLLATE ai, 'ä'), strpos('straße' COLLATE ai, 'ss'), strpos('strasse' COLLATE ai, 'ß');",
                    expected: Expected::Rows {
                        columns: &[Column("strpos", INT4), Column("strpos", INT4), Column("strpos", INT4), Column("strpos", INT4)],
                        rows: &[&[T("2"), T("3"), T("5"), T("5")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT replace('aAbBaA' COLLATE ci, 'a', 'x'), replace('Straße strasse' COLLATE ai, 'SS', '_'), split_part('a,b;A,B' COLLATE ci, 'A', 2), split_part('xSSyssz' COLLATE ai, 'ß', -1);",
                    expected: Expected::Rows {
                        columns: &[Column("replace", TEXT), Column("replace", TEXT), Column("split_part", TEXT), Column("split_part", TEXT)],
                        rows: &[&[T("xxbBxx"), T("Stra_e stra_e"), T(",b;"), T("z")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_to_array('aXbxc' COLLATE ci, 'x'), string_to_array('a,b,NULL,null' COLLATE ci, ',', 'null');",
                    expected: Expected::Rows {
                        columns: &[Column("string_to_array", TEXT_ARRAY), Column("string_to_array", TEXT_ARRAY)],
                        rows: &[&[T("{a,b,c}"), T("{a,b,NULL,NULL}")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT starts_with('Abc' COLLATE ci, 'a');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "nondeterministic collations are not supported for substring searches", ..E }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a' ~ 'a' COLLATE ci;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "nondeterministic collations are not supported for regular expressions", ..E }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT string_agg(v, ',' ORDER BY v COLLATE de_pb), string_agg(v, ',' ORDER BY v COLLATE lc) FROM (VALUES ('Ärger'), ('Alpha'), ('Affe'), ('Aerde'), ('ö'), ('z')) t(v);",
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT), Column("string_agg", TEXT)],
                        rows: &[&[T("Aerde,Ärger,Affe,Alpha,ö,z"), T("Aerde,Affe,Alpha,z,Ärger,ö")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP COLLATION ci;",
                    expected: Expected::Error(Diagnostic {
                        code: "2BP01",
                        message: "cannot drop collation ci because other objects depend on it",
                        detail: "column v of table nd2 depends on collation ci",
                        hint: "Use DROP ... CASCADE to drop the dependent objects too.",
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION de_pb RENAME TO phonebook;",
                    expected: Expected::Tag("ALTER COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP COLLATION phonebook, lc;",
                    expected: Expected::Tag("DROP COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "LIKE and ILIKE under nondeterministic collations",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE COLLATION ctest_det (provider = icu, locale = '', deterministic = true);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    notices: &[Diagnostic { code: "00000", message: r#"using standard form "und" for ICU locale """#, ..N }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION ctest_nondet (provider = icu, locale = '', deterministic = false);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    notices: &[Diagnostic { code: "00000", message: r#"using standard form "und" for ICU locale """#, ..N }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION case_insensitive (provider = icu, locale = '@colStrength=secondary', deterministic = false);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    notices: &[Diagnostic { code: "00000", message: r#"using standard form "und-u-ks-level2" for ICU locale "@colStrength=secondary""#, ..N }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION ignore_accents (provider = icu, locale = '@colStrength=primary;colCaseLevel=yes', deterministic = false);",
                    expected: Expected::Tag("CREATE COLLATION"),
                    notices: &[Diagnostic { code: "00000", message: r#"using standard form "und-u-kc-ks-level1" for ICU locale "@colStrength=primary;colCaseLevel=yes""#, ..N }],
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'abc' LIKE 'abc' COLLATE ctest_det, 'abc' LIKE 'a\bc' COLLATE ctest_det, 'abc' LIKE 'abc' COLLATE ctest_nondet, 'abc' LIKE 'a\bc' COLLATE ctest_nondet, 'AB' LIKE 'ab' COLLATE case_insensitive, 'AB' LIKE 'a\b' COLLATE case_insensitive, 'AB' LIKE '\ab' COLLATE case_insensitive, 'AB' LIKE '\a%' COLLATE case_insensitive, 'AB' LIKE '\a\%' COLLATE case_insensitive;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[&[T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("f")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT U&'\0061\0308bc' LIKE U&'\00E4_c' COLLATE ignore_accents, U&'\00E4bc' LIKE U&'\0061\0308_c' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'\00E4%c' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'\00E4%%c' COLLATE ignore_accents, U&'cb\0061\0308' LIKE U&'c%%\00E4' COLLATE ignore_accents, U&'cb\0061\0308' LIKE U&'cb_' COLLATE ignore_accents, U&'cb\0061\0308' LIKE U&'cb__' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'%\00E4bc' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'%\00E4%c' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'\00E4bc%' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'\00E4%c%' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'_bc' COLLATE ignore_accents, U&'\0308bc' = 'bc' COLLATE ignore_accents, U&'\0061\0308bc' LIKE U&'__bc' COLLATE ignore_accents;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[&[T("t"), T("t"), T("t"), T("t"), T("t"), T("f"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t"), T("t")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'Türkiye' COLLATE "en-x-icu" ILIKE '%KI%', 'Türkiye' COLLATE "tr-x-icu" ILIKE '%KI%', 'bıt' ILIKE 'BIT' COLLATE "en-x-icu", 'bıt' ILIKE 'BIT' COLLATE "tr-x-icu";"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[&[T("t"), T("f"), T("f"), T("t")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'AB' ILIKE 'ab' COLLATE case_insensitive;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "nondeterministic collations are not supported for ILIKE", ..E }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'abc' NOT LIKE 'A%' COLLATE case_insensitive, 'xabcy' LIKE '%B_' COLLATE case_insensitive, 'abc'::name LIKE 'A%' COLLATE case_insensitive;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[&[T("f"), T("f"), T("t")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "indexes and primary keys under ICU and libc collations",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION lc (provider = libc, locale = 'sv_SE.UTF-8');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE pk_en (v text COLLATE "en-US-x-icu" PRIMARY KEY, n int);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO pk_en VALUES ('b', 1), ('A', 2), ('a', 3), ('B', 4), ('ä', 5), (U&'a\0308', 6), ('c', 7);"#,
                    expected: Expected::Tag("INSERT 0 7"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT n FROM pk_en WHERE v = 'a';"#,
                    expected: Expected::Rows { columns: &[Column("n", INT4)], rows: &[&[T("3")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ',' ORDER BY v) FROM pk_en WHERE v >= 'B' AND v < 'c';"#,
                    expected: Expected::Rows { columns: &[Column("string_agg", TEXT)], rows: &[&[T("B")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ',' ORDER BY v DESC) FROM pk_en;"#,
                    expected: Expected::Rows { columns: &[Column("string_agg", TEXT)], rows: &[&[T("c,B,b,ä,a\u{308},A,a")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE ci_t (id int PRIMARY KEY, v text COLLATE ci);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE UNIQUE INDEX ci_t_v ON ci_t (v);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO ci_t VALUES (1, 'abc');"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ci_t VALUES (2, 'ABC');",
                    expected: Expected::Error(Diagnostic {
                        code: "23505",
                        message: r#"duplicate key value violates unique constraint "ci_t_v""#,
                        detail: "Key (v)=(ABC) already exists.",
                        schema: "public",
                        table: "ci_t",
                        constraint: "ci_t_v",
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO ci_t VALUES (3, 'def'), (4, 'Ghi');"#,
                    expected: Expected::Tag("INSERT 0 2"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT id FROM ci_t WHERE v = 'GHI';"#,
                    expected: Expected::Rows { columns: &[Column("id", INT4)], rows: &[&[T("4")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v FROM ci_t ORDER BY v;"#,
                    expected: Expected::Rows { columns: &[Column("v", TEXT)], rows: &[&[T("abc")], &[T("def")], &[T("Ghi")]], tag: "SELECT 3" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX ci_t_v2 ON ci_t (v DESC);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v FROM ci_t WHERE v > 'ABC' ORDER BY v DESC;"#,
                    expected: Expected::Rows { columns: &[Column("v", TEXT)], rows: &[&[T("Ghi")], &[T("def")]], tag: "SELECT 2" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE sv_t (id int PRIMARY KEY, v text COLLATE lc);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX ON sv_t (v);"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO sv_t VALUES (1, 'ö'), (2, 'z'), (3, 'å'), (4, 'a'), (5, 'o');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ',' ORDER BY v) FROM sv_t WHERE v > 'o';"#,
                    expected: Expected::Rows { columns: &[Column("string_agg", TEXT)], rows: &[&[T("z,å,ö")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "libc collations with glibc's names are built in",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT 'a' < 'B' COLLATE "en_US", 'ch' < 'd' COLLATE "cs_CZ.utf8", 'b' < 'a' COLLATE "C.utf8";"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[&[T("t"), T("f"), T("f")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ',' ORDER BY v COLLATE "sv_SE") FROM (VALUES ('ä'), ('å'), ('z'), ('a')) t(v);"#,
                    expected: Expected::Rows { columns: &[Column("string_agg", TEXT)], rows: &[&[T("a,z,å,ä")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT upper('istanbul' COLLATE "tr_TR.utf8");"#,
                    expected: Expected::Rows { columns: &[Column("upper", TEXT)], rows: &[&[T("İSTANBUL")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE de_t (v text COLLATE "de_DE" PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO de_t VALUES ('p'), ('ö'), ('o');"#,
                    expected: Expected::Tag("INSERT 0 3"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ',' ORDER BY v) FROM de_t;"#,
                    expected: Expected::Rows { columns: &[Column("string_agg", TEXT)], rows: &[&[T("o,ö,p")]], tag: "SELECT 1" },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'a' COLLATE "en_US.UTF-8";"#,
                    expected: Expected::Error(Diagnostic {
                        code: "42704",
                        message: r#"collation "en_US.UTF-8" for encoding "UTF8" does not exist"#,
                        position: 12,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ICU collations with backward secondary weights",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION fr_kb (provider = icu, locale = 'fr-u-kb');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION frca_nokb (provider = icu, locale = 'fr-CA-u-kb-false');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ',' ORDER BY v COLLATE fr_kb), string_agg(v, ',' ORDER BY v COLLATE "fr-x-icu"), string_agg(v, ',' ORDER BY v COLLATE frca_nokb), string_agg(v, ',' ORDER BY v COLLATE "fr-CA-x-icu") FROM (VALUES ('côte'), ('coté'), ('cote'), ('côté')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT), Column("string_agg", TEXT), Column("string_agg", TEXT), Column("string_agg", TEXT)],
                        rows: &[&[T("cote,côte,coté,côté"), T("cote,coté,côte,côté"), T("cote,coté,côte,côté"), T("cote,côte,coté,côté")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION bad_kb (provider = icu, locale = 'fr-u-kb-maybe');"#,
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not open collator for locale "fr-u-kb-maybe": U_ILLEGAL_ARGUMENT_ERROR"#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE kb_t (v text COLLATE fr_kb PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO kb_t VALUES ('côte'), ('coté'), ('cote'), ('côté');"#,
                    expected: Expected::Tag("INSERT 0 4"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v FROM kb_t WHERE v > 'côte' ORDER BY v;"#,
                    expected: Expected::Rows { columns: &[Column("v", TEXT)], rows: &[&[T("coté")], &[T("côté")]], tag: "SELECT 2" },
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ICU collations with script reordering",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION kr1 (provider = icu, locale = 'und-u-kr-grek-latn');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION kr2 (provider = icu, locale = 'ru-u-kr-zzzz');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION kr3 (provider = icu, locale = 'en-u-kr-latn-digit');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION kr4 (provider = icu, locale = 'und-u-kr-zzzz-latn');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE kr1), string_agg(v, ' ' ORDER BY v COLLATE kr2), string_agg(v, ' ' ORDER BY v COLLATE "ru-x-icu"), string_agg(v, ' ' ORDER BY v COLLATE kr3), string_agg(v, ' ' ORDER BY v COLLATE kr4) FROM (VALUES ('a'), ('α'), ('1'), ('_'), ('я'), ('$'), ('ア'), ('-')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT), Column("string_agg", TEXT), Column("string_agg", TEXT), Column("string_agg", TEXT), Column("string_agg", TEXT)],
                        rows: &[&[T("_ - $ 1 α a я ア"), T("_ - $ 1 a α я ア"), T("_ - $ 1 я a α ア"), T("_ - $ a 1 α я ア"), T("_ - $ 1 α я ア a")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION bad1 (provider = icu, locale = 'und-u-kr-latn-latn');"#,
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not open collator for locale "und-u-kr-latn-latn": U_ILLEGAL_ARGUMENT_ERROR"#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION bad2 (provider = icu, locale = 'und-u-kr-others');"#,
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not open collator for locale "und-u-kr-others": U_ILLEGAL_ARGUMENT_ERROR"#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE kr_t (v text COLLATE kr1 PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO kr_t VALUES ('a'), ('α'), ('b'), ('β'), ('я');"#,
                    expected: Expected::Tag("INSERT 0 5"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT v FROM kr_t WHERE v < 'b' ORDER BY v;"#,
                    expected: Expected::Rows { columns: &[Column("v", TEXT)], rows: &[&[T("α")], &[T("β")], &[T("a")]], tag: "SELECT 3" },
                    flow: Flow::Simple,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "ICU collations with rules sort by the locale's rules followed by theirs",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r1 (provider = icu, locale = 'und', rules = '&a < g');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r1) FROM (VALUES ('a'), ('b'), ('g'), ('h'), ('A'), ('G')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("a A g b G h")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r2 (provider = icu, locale = 'und', rules = '&a < g <<');"#,
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not open collator for locale "und" with rules "&a < g <<": U_INVALID_FORMAT_ERROR"#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r3 (provider = icu, locale = 'und', rules = '[import xx-u-co-foo]');"#,
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not open collator for locale "und" with rules "[import xx-u-co-foo]": U_MISSING_RESOURCE_ERROR"#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r4 (provider = icu, locale = 'und', rules = '[reorder Grek Grek]');"#,
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not open collator for locale "und" with rules "[reorder Grek Grek]": U_ILLEGAL_ARGUMENT_ERROR"#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r5 (provider = icu, locale = 'und-u-ks-level2', rules = '[strength 1]&a < b', deterministic = false);"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT 'a' = 'A' COLLATE r5, 'a' < 'B' COLLATE r5, 'b' < 'a' COLLATE r5;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[&[T("f"), T("t"), T("f")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r6 (provider = icu, locale = 'da', rules = '&z < w');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r6) FROM (VALUES ('w'), ('v'), ('z'), ('å'), ('ø'), ('æ'), ('aa'), ('A'), ('a')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("A a v z w æ ø å aa")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r7 (provider = icu, locale = 'de-u-co-phonebk', rules = '&z < x');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r7) FROM (VALUES ('ä'), ('ae'), ('af'), ('ad'), ('x'), ('z'), ('y')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("ad ae ä af y z x")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r8 (provider = icu, locale = 'und', rules = '[caseFirst upper][numericOrdering on][alternate shifted]');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r8) FROM (VALUES ('a10'), ('a2'), ('A2'), ('a-1'), ('a 3')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("a-1 A2 a2 a 3 a10")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r9 (provider = icu, locale = 'und', rules = '&ch < cx [suppressContractions [ч]] &[before 1]a < ä');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r9) FROM (VALUES ('cx'), ('ch'), ('ci'), ('a'), ('ä'), ('Ä'), ('9')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("9 ä a Ä ch cx ci")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE rt (v text COLLATE r1 PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO rt VALUES ('a'), ('b'), ('g'), ('h'), ('ab'), ('gb');"#,
                    expected: Expected::Tag("INSERT 0 6"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ') FROM (SELECT v FROM rt WHERE v < 'b' ORDER BY v) s;"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("a ab g gb")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT collicurules FROM pg_collation WHERE collname = 'r6';"#,
                    expected: Expected::Rows {
                        columns: &[Column("collicurules", TEXT)],
                        rows: &[&[T("&z < w")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r12 (provider = icu, locale = 'zh', rules = '&[last regular] < z');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r12) FROM (VALUES ('中'), ('国'), ('人'), ('a'), ('z'), ('ア')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("国 人 中 z a ア")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r13 (provider = icu, locale = 'und', rules = '&a < [');"#,
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: r#"could not open collator for locale "und" with rules "&a < [": U_INVALID_FORMAT_ERROR"#,
                        ..E
                    }),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE DATABASE dr1 LOCALE_PROVIDER icu ICU_LOCALE 'und' ICU_RULES '&a < g' TEMPLATE template0;"#,
                    expected: Expected::Tag("CREATE DATABASE"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT daticurules FROM pg_database WHERE datname = 'dr1';"#,
                    expected: Expected::Rows {
                        columns: &[Column("daticurules", TEXT)],
                        rows: &[&[T("&a < g")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r10 (provider = icu, locale = 'und', rules = '&a < bc|d');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    skip: Some("ICU4X can't match a prefix of two starters (audit finding 4003)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r10) FROM (VALUES ('bca'), ('bcd'), ('bcb'), ('bd')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("bca bcd bcb bd")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    skip: Some("ICU4X can't match a prefix of two starters (audit finding 4003)"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE COLLATION r11 (provider = icu, locale = 'und', rules = '[reorder Grek Latn digit]');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT string_agg(v, ' ' ORDER BY v COLLATE r11) FROM (VALUES ('a'), ('α'), ('1'), ('!')) t(v);"#,
                    expected: Expected::Rows {
                        columns: &[Column("string_agg", TEXT)],
                        rows: &[&[T("! α a 1")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT '1' < 'a' COLLATE r11;"#,
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[&[T("t")]],
                        tag: "SELECT 1",
                    },
                    flow: Flow::Simple,
                    skip: Some("ICU compares Latin and digits against the reordering its sort keys follow (audit finding 4004)"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "renaming two collations to one name at once fails the second on the catalog's unique index",
            set_up_script: &[
                "CREATE COLLATION c1 (provider = icu, locale = 'und');",
                "CREATE COLLATION c2 (provider = icu, locale = 'und');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION c1 RENAME TO cx;",
                    expected: Expected::Tag("ALTER COLLATION"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER COLLATION c2 RENAME TO cx;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "pg_collation_name_enc_nsp_index""#, detail: "Key (collname, collencoding, collnamespace)=(cx, -1, 2200) already exists.", schema: "pg_catalog", table: "pg_collation", constraint: "pg_collation_name_enc_nsp_index", ..E }),
                    flow: Flow::Exec,
                    client: "a",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "b",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT collname FROM pg_collation WHERE collname IN ('c1', 'c2', 'cx') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("collname", NAME)],
                        rows: &[&[T("c2")], &[T("cx")]],
                        tag: "SELECT 2",
                    },
                    client: "a",
                    ..A
                },
            ],
            ..S
        },
    ]);
}
