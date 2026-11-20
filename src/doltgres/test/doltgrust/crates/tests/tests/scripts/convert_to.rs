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
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_convert_to() {
    run_scripts(&[
        ScriptTest {
            name: "convert_to",
            set_up_script: &[
                "CREATE TABLE supported_encodings (name TEXT PRIMARY KEY);",
                "INSERT INTO supported_encodings VALUES ('SQL_ASCII'),('SQLASCII'),('EUC_JP'),('EUC_KR'),('UTF8'),('utf-8'),('UNICODE'),('LATIN1'),('ISO_8859_1'),('LATIN2'),('ISO_8859_2'),('LATIN3'),('ISO_8859_3'),('LATIN4'),('ISO_8859_4'),('LATIN5'),('ISO_8859_9'),('LATIN6'),('ISO_8859_10'),('LATIN7'),('ISO_8859_13'),('LATIN8'),('ISO_8859_14'),('LATIN9'),('ISO_8859_15'),('LATIN10'),('ISO_8859_16'),('WIN1256'),('WIN1258'),('WIN866'),('ALT'),('WIN874'),('KOI8R'),('WIN1251'),('WIN'),('WIN1252'),('ISO_8859_5'),('ISO_8859_6'),('ISO_8859_7'),('ISO_8859_8'),('WIN1250'),('WIN1253'),('WIN1254'),('WIN1255'),('WIN1257'),('KOI8U'),('SJIS'),('SHIFT_JIS'),('BIG5'),('GBK'),('UHC'),('GB18030'),('WINDOWS-866'),('WINDOWS-874'),('WINDOWS-1250'),('WINDOWS-1251'),('WINDOWS-1252'),('WINDOWS-1253'),('WINDOWS-1254'),('WINDOWS-1255'),('WINDOWS-1256'),('WINDOWS-1257'),('WINDOWS-1258');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), bool_and(pg_char_to_encoding(name) >= 0), bool_and(encode(convert_to('x', name::name), 'hex') = '78') FROM supported_encodings;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("bool_and", BOOL), Column("bool_and", BOOL)],
                        rows: &[
                            &[T("63"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('x', 'UTF8'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT)],
                        rows: &[
                            &[T("78")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('café', 'utf-8'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT)],
                        rows: &[
                            &[T("636166c3a9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('日本', 'EUC_JP'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT)],
                        rows: &[
                            &[T("c6fccbdc")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('한', 'UHC'), 'hex'), encode(convert_to('Ж', 'ALT'), 'hex'), encode(convert_to('Ж', 'WIN'), 'hex'), encode(convert_to('Ж', 'ISO_8859_5'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("c7d1"), T("86"), T("c6"), T("b6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('Ж', 'KOI8R'), 'hex'), encode(convert_to('日本', 'SJIS'), 'hex'), encode(convert_to('한', 'EUC_KR'), 'hex'), encode(convert_to('한', 'UHC'), 'hex'), encode(convert_to('中文', 'BIG5'), 'hex'), encode(convert_to('中文', 'GBK'), 'hex'), encode(convert_to('😄', 'GB18030'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("f6"), T("93fa967b"), T("c7d1"), T("c7d1"), T("a4a4a4e5"), T("d6d0cec4"), T("9439fd30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('€', 'WIN1252'), 'hex'), encode(convert_to('€', 'WINDOWS-1252'), 'hex'), encode(convert_to('Ж', 'WIN866'), 'hex'), encode(convert_to('Ж', 'WINDOWS-866'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("80"), T("80"), T("86"), T("86")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('café', 'LATIN1'), 'hex'), encode(convert_to('café', 'ISO_8859_1'), 'hex'), encode(convert_to('日本', 'SJIS'), 'hex'), encode(convert_to('日本', 'SHIFT_JIS'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("636166e9"), T("636166e9"), T("93fa967b"), T("93fa967b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_char_to_encoding('ALT'), pg_char_to_encoding('WIN'), pg_char_to_encoding('UHC'), pg_char_to_encoding('ISO_8859_5'), pg_char_to_encoding('EUC_CN');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4)],
                        rows: &[
                            &[T("20"), T("23"), T("38"), T("25"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_char_to_encoding('WINDOWS-866'), pg_char_to_encoding('WINDOWS-874'), pg_char_to_encoding('WINDOWS-1250'), pg_char_to_encoding('WINDOWS-1251'), pg_char_to_encoding('WINDOWS-1252'), pg_char_to_encoding('WINDOWS-1253'), pg_char_to_encoding('WINDOWS-1254'), pg_char_to_encoding('WINDOWS-1255'), pg_char_to_encoding('WINDOWS-1256'), pg_char_to_encoding('WINDOWS-1257'), pg_char_to_encoding('WINDOWS-1258');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4), Column("pg_char_to_encoding", INT4)],
                        rows: &[
                            &[T("20"), T("21"), T("29"), T("23"), T("24"), T("30"), T("31"), T("32"), T("18"), T("33"), T("19")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('', 'UTF8'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to(NULL, 'UTF8'), convert_to('x', NULL);",
                    expected: Expected::Rows {
                        columns: &[Column("convert_to", BYTEA), Column("convert_to", BYTEA)],
                        rows: &[
                            &[Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to('€', 'LATIN1');",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: r#"character with byte sequence 0xe2 0x82 0xac in encoding "UTF8" has no equivalent in encoding "LATIN1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to('x', 'EUC_CN');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to('x', 'bogus');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"invalid destination encoding name "bogus""#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_encoding_conversions() {
    run_scripts(&[
        ScriptTest {
            name: "encoding conversions: every encoding, MULE_INTERNAL and local pairs, errors, and CREATE CONVERSION",
            set_up_script: &[
                "CREATE ROLE regress_conv_user LOGIN SUPERUSER;",
                "SET SESSION AUTHORIZATION 'regress_conv_user';",
                "CREATE ROLE regress_conv_owner;",
                "CREATE SCHEMA conv_s;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT e, encode(convert_to(t, e), 'hex') AS converted, convert_from(convert_to(t, e), e) = t AS round_trip FROM (VALUES ('LATIN1', 'café'), ('LATIN2', 'Kraków łódź'), ('LATIN9', '€uro'), ('KOI8R', 'Жук'), ('KOI8U', 'Їжак'), ('WIN1251', 'Жук'), ('WIN866', 'Жук'), ('ISO_8859_5', 'Жук'), ('WIN1252', '€ café'), ('WIN1250', 'Łódź'), ('EUC_JP', 'あ漢'), ('SJIS', 'あ漢ｱ'), ('EUC_JIS_2004', 'あ漢'), ('SHIFT_JIS_2004', 'あ漢'), ('EUC_KR', '한국'), ('UHC', '한국'), ('EUC_CN', '汉字'), ('GBK', '汉字'), ('GB18030', '汉字ђ€😀'), ('BIG5', '漢字'), ('EUC_TW', '漢字'), ('ISO_8859_7', 'αβγ'), ('ISO_8859_8', 'שלום'), ('WIN1256', 'سلام'), ('WIN874', 'ไทย'), ('WIN1258', 'Đà')) v(e, t) ORDER BY e;",
                    expected: Expected::Rows {
                        columns: &[Column("e", TEXT), Column("converted", TEXT), Column("round_trip", BOOL)],
                        rows: &[
                            &[T("BIG5"), T("ba7ea672"), T("t")],
                            &[T("EUC_CN"), T("babad7d6"), T("t")],
                            &[T("EUC_JIS_2004"), T("a4a2b4c1"), T("t")],
                            &[T("EUC_JP"), T("a4a2b4c1"), T("t")],
                            &[T("EUC_KR"), T("c7d1b1b9"), T("t")],
                            &[T("EUC_TW"), T("e9c7c7f3"), T("t")],
                            &[T("GB18030"), T("babad7d68130d330a2e39439fc36"), T("t")],
                            &[T("GBK"), T("babad7d6"), T("t")],
                            &[T("ISO_8859_5"), T("b6e3da"), T("t")],
                            &[T("ISO_8859_7"), T("e1e2e3"), T("t")],
                            &[T("ISO_8859_8"), T("f9ece5ed"), T("t")],
                            &[T("KOI8R"), T("f6d5cb"), T("t")],
                            &[T("KOI8U"), T("b7d6c1cb"), T("t")],
                            &[T("LATIN1"), T("636166e9"), T("t")],
                            &[T("LATIN2"), T("4b72616bf37720b3f364bc"), T("t")],
                            &[T("LATIN9"), T("a475726f"), T("t")],
                            &[T("SHIFT_JIS_2004"), T("82a08abf"), T("t")],
                            &[T("SJIS"), T("82a08abfb1"), T("t")],
                            &[T("UHC"), T("c7d1b1b9"), T("t")],
                            &[T("WIN1250"), T("a3f3649f"), T("t")],
                            &[T("WIN1251"), T("c6f3ea"), T("t")],
                            &[T("WIN1252"), T("8020636166e9"), T("t")],
                            &[T("WIN1256"), T("d3e1c7e3"), T("t")],
                            &[T("WIN1258"), T("d0e0"), T("t")],
                            &[T("WIN866"), T("86e3aa"), T("t")],
                            &[T("WIN874"), T("e4b7c2"), T("t")],
                        ],
                        tag: "SELECT 26",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert_to('한국', 'JOHAB'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT)],
                        rows: &[
                            &[T("d0658a82")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from(convert_to('한국', 'JOHAB'), 'JOHAB');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"JOHAB\": 0xd0 0x65", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert(convert_to('かか゚', 'EUC_JIS_2004'), 'EUC_JIS_2004', 'UTF8'), 'hex'), encode(convert_to('かか゚', 'EUC_JIS_2004'), 'hex'), encode(convert_to('かか゚', 'SHIFT_JIS_2004'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("e3818be3818be3829a"), T("a4aba4f7"), T("82a982f5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert(convert_to('あ漢ｱ', 'EUC_JP'), 'EUC_JP', 'SJIS'), 'hex'), encode(convert(convert_to('あ漢ｱ', 'SJIS'), 'SJIS', 'EUC_JP'), 'hex'), encode(convert(convert_to('あ漢', 'EUC_JP'), 'EUC_JP', 'MULE_INTERNAL'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("82a08abfb1"), T("a4a2b4c18eb1"), T("92a4a292b4c1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert(convert_to('漢字', 'BIG5'), 'BIG5', 'EUC_TW'), 'hex'), encode(convert(convert_to('漢字', 'EUC_TW'), 'EUC_TW', 'BIG5'), 'hex'), encode(convert(convert_to('漢字', 'BIG5'), 'BIG5', 'MULE_INTERNAL'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("e9c7c7f3"), T("ba7ea672"), T("95e9c795c7f3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert(convert_to('Жук', 'KOI8R'), 'KOI8R', 'WIN1251'), 'hex'), encode(convert(convert_to('Жук', 'WIN866'), 'WIN866', 'ISO_8859_5'), 'hex'), encode(convert(convert_to('Łódź', 'LATIN2'), 'LATIN2', 'WIN1250'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("c6f3ea"), T("b6e3da"), T("a3f3649f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert(convert_to('한국', 'EUC_KR'), 'EUC_KR', 'MULE_INTERNAL'), 'hex'), encode(convert(convert_to('汉字', 'EUC_CN'), 'EUC_CN', 'MULE_INTERNAL'), 'hex'), encode(convert('\\xe9'::bytea, 'LATIN1', 'MULE_INTERNAL'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("93c7d193b1b9"), T("91baba91d7d6"), T("81e9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert('\\x8130d330'::bytea, 'GB18030', 'UTF8'), 'hex'), encode(convert('\\xe3329a35'::bytea, 'GB18030', 'UTF8'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("d192"), T("f48fbfbf")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to('漢', 'LATIN1');",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xe6 0xbc 0xa2 in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert('\\xff'::bytea, 'UTF8', 'LATIN1');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"UTF8\": 0xff", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert('\\x8e'::bytea, 'EUC_JP', 'UTF8');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"EUC_JP\": 0x8e", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('\\xa4'::bytea, 'BIG5');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"BIG5\": 0xa4", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert('\\x00'::bytea, 'LATIN1', 'UTF8');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"LATIN1\": 0x00", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert('abc'::bytea, 'nope', 'UTF8');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid source encoding name \"nope\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert('abc'::bytea, 'UTF8', 'nope');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid destination encoding name \"nope\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to('abc', 'nope');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid destination encoding name \"nope\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('abc', 'nope');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid source encoding name \"nope\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert('abc'::bytea, 'SQL_ASCII', 'LATIN1'), 'hex'), encode(convert('\\xffff'::bytea, 'LATIN1', 'SQL_ASCII'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT), Column("encode", TEXT)],
                        rows: &[
                            &[T("616263"), T("ffff")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert('\\xff'::bytea, 'SQL_ASCII', 'UTF8');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"UTF8\": 0xff", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length('\\x82a082a0'::bytea, 'SJIS'), length('\\x636166c3a9'::bytea, 'UTF8'), length('\\xe9e9'::bytea, 'LATIN1');",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("2"), T("4"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length('\\x82'::bytea, 'SJIS');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"SJIS\": 0x82", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length('\\x6100'::bytea, 'LATIN1');",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"LATIN1\": 0x00", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length('abc'::bytea, 'nope');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid encoding name \"nope\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(*) FILTER (WHERE condefault) FROM pg_conversion;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("128"), T("128")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_encoding_to_char(conforencoding), pg_encoding_to_char(contoencoding), conproc::regproc, condefault, pg_conversion_is_visible(oid) FROM pg_conversion WHERE conname IN ('utf8_to_iso_8859_1', 'koi8_r_to_mic', 'big5_to_euc_tw', 'gb18030_to_utf8') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_encoding_to_char", NAME), Column("pg_encoding_to_char", NAME), Column("conproc", REGPROC), Column("condefault", BOOL), Column("pg_conversion_is_visible", BOOL)],
                        rows: &[
                            &[T("big5_to_euc_tw"), T("BIG5"), T("EUC_TW"), T("big5_to_euc_tw"), T("t"), T("t")],
                            &[T("gb18030_to_utf8"), T("GB18030"), T("UTF8"), T("gb18030_to_utf8"), T("t"), T("t")],
                            &[T("koi8_r_to_mic"), T("KOI8R"), T("MULE_INTERNAL"), T("koi8r_to_mic"), T("t"), T("t")],
                            &[T("utf8_to_iso_8859_1"), T("UTF8"), T("LATIN1"), T("utf8_to_iso8859_1"), T("t"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CONVERSION conv_s.myconv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;",
                    expected: Expected::Tag("CREATE CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CONVERSION conv_s.myconv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "conversion \"myconv\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DEFAULT CONVERSION conv_s.mydef FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;",
                    expected: Expected::Tag("CREATE CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DEFAULT CONVERSION conv_s.mydef2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "default conversion for LATIN1 to UTF8 already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CONVERSION conv_s.bad FOR 'SQL_ASCII' TO 'UTF8' FROM iso8859_1_to_utf8;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "encoding conversion to or from \"SQL_ASCII\" is not supported", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CONVERSION conv_s.bad FOR 'NOPE' TO 'UTF8' FROM iso8859_1_to_utf8;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "source encoding \"NOPE\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CONVERSION conv_s.bad FOR 'LATIN1' TO 'NOPE' FROM iso8859_1_to_utf8;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "destination encoding \"NOPE\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CONVERSION conv_s.bad FOR 'LATIN1' TO 'UTF8' FROM no_such_function;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function no_such_function(integer, integer, cstring, internal, integer, boolean) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE CONVERSION conv_s.bad FOR 'LATIN2' TO 'UTF8' FROM iso8859_1_to_utf8;",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "expected source encoding \"LATIN1\", but got \"LATIN2\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, connamespace::regnamespace, conowner::regrole, condefault, pg_conversion_is_visible(oid), pg_describe_object('pg_conversion'::regclass, oid, 0) FROM pg_conversion WHERE connamespace = 'conv_s'::regnamespace ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("connamespace", REGNAMESPACE), Column("conowner", REGROLE), Column("condefault", BOOL), Column("pg_conversion_is_visible", BOOL), Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("myconv"), T("conv_s"), T("regress_conv_user"), T("f"), T("f"), T("conversion conv_s.myconv")],
                            &[T("mydef"), T("conv_s"), T("regress_conv_user"), T("t"), T("f"), T("conversion conv_s.mydef")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object(classid, objid, 0)).*, (pg_identify_object_as_address(classid, objid, 0)).* FROM pg_get_object_address('conversion', '{conv_s,myconv}', '{}');",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT), Column("type", TEXT), Column("object_names", TEXT_ARRAY), Column("object_args", TEXT_ARRAY)],
                        rows: &[
                            &[T("conversion"), T("conv_s"), T("myconv"), T("conv_s.myconv"), T("conversion"), T("{conv_s,myconv}"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = conv_s, public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT encode(convert('\\xe9'::bytea, 'LATIN1', 'UTF8'), 'hex');",
                    expected: Expected::Rows {
                        columns: &[Column("encode", TEXT)],
                        rows: &[
                            &[T("c3a9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, pg_conversion_is_visible(oid) FROM pg_conversion WHERE connamespace = 'conv_s'::regnamespace ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("pg_conversion_is_visible", BOOL)],
                        rows: &[
                            &[T("myconv"), T("t")],
                            &[T("mydef"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET search_path;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON CONVERSION conv_s.myconv IS 'mine';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(oid, 'pg_conversion') FROM pg_conversion WHERE conname = 'myconv';",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("mine")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER CONVERSION conv_s.myconv RENAME TO mydef;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "conversion \"mydef\" already exists in schema \"conv_s\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER CONVERSION conv_s.myconv RENAME TO renamed;",
                    expected: Expected::Tag("ALTER CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER CONVERSION conv_s.renamed OWNER TO regress_conv_owner;",
                    expected: Expected::Tag("ALTER CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER CONVERSION conv_s.renamed SET SCHEMA public;",
                    expected: Expected::Tag("ALTER CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT conname, connamespace::regnamespace, conowner::regrole FROM pg_conversion WHERE conname = 'renamed';",
                    expected: Expected::Rows {
                        columns: &[Column("conname", NAME), Column("connamespace", REGNAMESPACE), Column("conowner", REGROLE)],
                        rows: &[
                            &[T("renamed"), T("public"), T("regress_conv_owner")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP CONVERSION no_such_conversion;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "conversion \"no_such_conversion\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP CONVERSION IF EXISTS no_such_conversion;",
                    notices: &[Diagnostic { code: "00000", message: "conversion \"no_such_conversion\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP CONVERSION IF EXISTS no_schema.no_such_conversion;",
                    notices: &[Diagnostic { code: "00000", message: "schema \"no_schema\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_conv_owner;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "role \"regress_conv_owner\" cannot be dropped because some objects depend on it", detail: "owner of conversion renamed", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP CONVERSION renamed;",
                    expected: Expected::Tag("DROP CONVERSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA conv_s CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to conversion conv_s.mydef", ..N }],
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_conversion WHERE conname IN ('renamed', 'mydef');",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_ascii('Kraków', 'LATIN2'), to_ascii('café', 8), to_ascii('x', 'WIN1250');",
                    expected: Expected::Rows {
                        columns: &[Column("to_ascii", TEXT), Column("to_ascii", TEXT), Column("to_ascii", TEXT)],
                        rows: &[
                            &[T("KrakAlw"), T("cafAC"), T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_ascii('x');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "encoding conversion from UTF8 to ASCII not supported", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_ascii('x', 'nope');",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "nope is not a valid encoding name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_ascii('x', 99);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "99 is not a valid encoding code", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_ascii('x', 'KOI8R');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "encoding conversion from KOI8R to ASCII not supported", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE regress_conv_owner;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
