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
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_client_encoding() {
    run_scripts(&[
        ScriptTest {
            name: "client encoding SJIS",
            set_up_script: &[
                "SET client_encoding = 'SJIS';",
                "CREATE TABLE sj (id INT PRIMARY KEY, \"名前\" TEXT);",
                "INSERT INTO sj VALUES (1, '表'), (2, 'ソ\\'), (3, '能力'), (4, 'abc');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SHOW client_encoding;",
                    expected: Expected::Rows {
                        columns: &[Column("client_encoding", TEXT)],
                        rows: &[
                            &[T("SJIS")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_client_encoding();",
                    expected: Expected::Rows {
                        columns: &[Column("pg_client_encoding", NAME)],
                        rows: &[
                            &[T("SJIS")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '表ソ能' AS \"列ソ\", length('表ソ能'), octet_length('表ソ能');",
                    expected: Expected::Rows {
                        columns: &[Column("列ソ", TEXT), Column("length", INT4), Column("octet_length", INT4)],
                        rows: &[
                            &[T("表ソ能"), T("3"), T("9")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, \"名前\", length(\"名前\") FROM sj ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("名前", TEXT), Column("length", INT4)],
                        rows: &[
                            &[T("1"), T("表"), T("1")],
                            &[T("2"), T("ソ\\"), T("2")],
                            &[T("3"), T("能力"), T("2")],
                            &[T("4"), T("abc"), T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT \"名前\" FROM sj WHERE \"名前\" LIKE '%ソ%';",
                    expected: Expected::Rows {
                        columns: &[Column("名前", TEXT)],
                        rows: &[
                            &[T("ソ\\")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '表'::int;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"表\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('\\xc3a9', 'UTF8');",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xc3 0xa9 in encoding \"UTF8\" has no equivalent in encoding \"SJIS\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to('表', 'SJIS'), convert_to('表', 'UTF8');",
                    expected: Expected::Rows {
                        columns: &[Column("convert_to", BYTEA), Column("convert_to", BYTEA)],
                        rows: &[
                            &[T("\\x955c"), T("\\xe8a1a8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT E'\\u8868', U&'\\8868';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT), Column("?column?", TEXT)],
                        rows: &[
                            &[T("表"), T("表")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x FROM (VALUES ('能'), ('ソ')) v(x) ORDER BY x COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("x", TEXT)],
                        rows: &[
                            &[T("ソ")],
                            &[T("能")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sj;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT $1::text || 'ソ' AS x, length($1::text);",
                    bind_vars: &[BindVar::Str("表能")],
                    expected: Expected::Rows {
                        columns: &[Column("x", TEXT), Column("length", INT4)],
                        rows: &[
                            &[T("表能ソ"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM sj ORDER BY id) TO STDOUT;",
                    copy_to_stdout_file: "client-encoding-sjis.txt",
                    expected: Expected::Tag("COPY 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY (SELECT * FROM sj ORDER BY id) TO STDOUT (FORMAT csv, HEADER);",
                    copy_to_stdout_file: "client-encoding-sjis.csv",
                    expected: Expected::Tag("COPY 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COPY sj FROM STDIN;",
                    copy_from_stdin_file: "client-encoding-sjis-in.txt",
                    expected: Expected::Tag("COPY 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, \"名前\", length(\"名前\") FROM sj WHERE id >= 5 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("名前", TEXT), Column("length", INT4)],
                        rows: &[
                            &[T("5"), T("表ソ"), T("2")],
                            &[T("6"), T("x\\y\tz"), T("5")],
                            &[T("7"), Null, Null],
                            &[T("8"), T("能\\"), T("2")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "client encoding EUC_JP",
            set_up_script: &[
                "SET client_encoding = 'EUC_JP';",
                "CREATE TABLE ej (id INT PRIMARY KEY, \"見出し\" TEXT);",
                "INSERT INTO ej VALUES (1, '日本語'), (2, 'ｶﾀｶﾅ'), (3, '漢字かな');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, \"見出し\", length(\"見出し\"), octet_length(\"見出し\") FROM ej ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("見出し", TEXT), Column("length", INT4), Column("octet_length", INT4)],
                        rows: &[
                            &[T("1"), T("日本語"), T("3"), T("9")],
                            &[T("2"), T("ｶﾀｶﾅ"), T("4"), T("12")],
                            &[T("3"), T("漢字かな"), T("4"), T("12")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_to('日本', 'EUC_JP'), convert_to('ｶ', 'EUC_JP');",
                    expected: Expected::Rows {
                        columns: &[Column("convert_to", BYTEA), Column("convert_to", BYTEA)],
                        rows: &[
                            &[T("\\xc6fccbdc"), T("\\x8eb6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '日本'::int;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"日本\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('\\xc3a9', 'UTF8');",
                    expected: Expected::Rows {
                        columns: &[Column("convert_from", TEXT)],
                        rows: &[
                            &[T("é")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "client encoding LATIN1",
            set_up_script: &[
                "SET client_encoding = 'LATIN1';",
                "CREATE TABLE l1 (id INT PRIMARY KEY, \"café\" TEXT);",
                "INSERT INTO l1 VALUES (1, 'naïve'), (2, 'Ångström'), (3, '¿qué?');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, \"café\", length(\"café\"), octet_length(\"café\") FROM l1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("café", TEXT), Column("length", INT4), Column("octet_length", INT4)],
                        rows: &[
                            &[T("1"), T("naïve"), T("5"), T("6")],
                            &[T("2"), T("Ångström"), T("8"), T("10")],
                            &[T("3"), T("¿qué?"), T("5"), T("7")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'é'::int;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"é\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('\\xe8a1a8', 'UTF8');",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xe8 0xa1 0xa8 in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'ok' AS \"résumé\";",
                    expected: Expected::Rows {
                        columns: &[Column("résumé", TEXT)],
                        rows: &[
                            &[T("ok")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('\\xe8a1a8', 'UTF8')::int;",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xe8 0xa1 0xa8 in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE EXCEPTION 'déjà vu' USING DETAIL = 'détail', HINT = 'indice'; END$$;",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "déjà vu", detail: "détail", hint: "indice", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET client_encoding = 'MULE_INTERNAL';",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "invalid value for parameter \"client_encoding\": \"MULE_INTERNAL\"", detail: "Conversion between MULE_INTERNAL and UTF8 is not supported.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET client_encoding = 'nonesuch';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for parameter \"client_encoding\": \"nonesuch\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET client_encoding = 'unicode';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW client_encoding;",
                    expected: Expected::Rows {
                        columns: &[Column("client_encoding", TEXT)],
                        rows: &[
                            &[T("UTF8")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET client_encoding = 'iso_8859_1';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW client_encoding;",
                    expected: Expected::Rows {
                        columns: &[Column("client_encoding", TEXT)],
                        rows: &[
                            &[T("LATIN1")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'é' || 'ü';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("éü")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT $1::text || 'ü' AS x, length($1::text);",
                    bind_vars: &[BindVar::Str("çà")],
                    expected: Expected::Rows {
                        columns: &[Column("x", TEXT), Column("length", INT4)],
                        rows: &[
                            &[T("çàü"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE NOTICE '%', convert_from('\\xe8a1a8', 'UTF8'); END$$;",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xe8 0xa1 0xa8 in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN RAISE NOTICE 'before'; RAISE NOTICE '%', chr(26085); RAISE NOTICE 'after'; END$$;",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xe6 0x97 0xa5 in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"", ..E }),
                    notices: &[Diagnostic { code: "00000", message: "before", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$BEGIN PERFORM 1 / 0; EXCEPTION WHEN division_by_zero THEN RAISE WARNING 'w %', chr(26085); END$$;",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xe6 0x97 0xa5 in encoding \"UTF8\" has no equivalent in encoding \"LATIN1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "25P02", message: "current transaction is aborted, commands ignored until end of transaction block", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'still here';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("still here")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "client encoding WIN1252",
            set_up_script: &[
                "SET client_encoding = 'WIN1252';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT '€ – “quoted”' AS t, length('€ – “quoted”'), convert_to('€', 'UTF8');",
                    expected: Expected::Rows {
                        columns: &[Column("t", TEXT), Column("length", INT4), Column("convert_to", BYTEA)],
                        rows: &[
                            &[T("€ – “quoted”"), T("12"), T("\\xe282ac")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('\\xc5a0', 'UTF8'), convert_from('\\xe28098', 'UTF8');",
                    expected: Expected::Rows {
                        columns: &[Column("convert_from", TEXT), Column("convert_from", TEXT)],
                        rows: &[
                            &[T("Š"), T("‘")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT convert_from('\\xe8a1a8', 'UTF8');",
                    expected: Expected::Error(Diagnostic { code: "22P05", message: "character with byte sequence 0xe8 0xa1 0xa8 in encoding \"UTF8\" has no equivalent in encoding \"WIN1252\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
