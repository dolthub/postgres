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
use harness::script::{A, Column, Expected, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_aggregates_over_out_of_line_values() {
    run_scripts(&[
        ScriptTest {
            name: "aggregates and arrays over values kept out of line, through transition, combine, and final functions",
            set_up_script: &[
                "CREATE TABLE big (id INT, b TEXT, y BYTEA, j JSON, jb JSONB);",
                "INSERT INTO big SELECT i, repeat(substr('abcd', i, 1), 300000), repeat(substr('ABCD', i, 1), 300000)::bytea, to_json(repeat(substr('abcd', i, 1), 300000)), to_jsonb(repeat(substr('abcd', i, 1), 300000)) FROM (VALUES (1), (2), (3), (4)) v (i);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 299999, 4), substr(s, 900000, 4), substr(s, 1199999, 5) FROM (SELECT string_agg(b, ',' ORDER BY id) AS s FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT), Column("substr", TEXT), Column("substr", TEXT)],
                        rows: &[
                            &[T("1200003"), T("aa,b"), T("ccc,"), T("ddddd")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 299999, 4), substr(s, 900000, 4) FROM (SELECT string_agg(y, ','::bytea ORDER BY id) AS s FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", BYTEA), Column("substr", BYTEA)],
                        rows: &[
                            &[T("1200003"), T(r#"\x41412c42"#), T(r#"\x4343432c"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 1, 3), substr(s, 300000, 6), substr(s, length(s) - 2, 3) FROM (SELECT array_agg(b ORDER BY id)::text AS s FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT), Column("substr", TEXT), Column("substr", TEXT)],
                        rows: &[
                            &[T("1200005"), T("{aa"), T("aa,bbb"), T("dd}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 1, 4), substr(s, 300001, 7) FROM (SELECT array_agg(ARRAY[b] ORDER BY id)::text AS s FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT), Column("substr", TEXT)],
                        rows: &[
                            &[T("1200013"), T("{{aa"), T("aa},{bb")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 300001, 7), length(sb), substr(sb, 300001, 7) FROM (SELECT json_agg(b ORDER BY id)::text AS s, jsonb_agg(b ORDER BY id)::text AS sb FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT), Column("length", INT4), Column("substr", TEXT)],
                        rows: &[
                            &[T("1200016"), T(r#"aa", "b"#), T("1200016"), T(r#"aa", "b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 300001, 7), length(sb), substr(sb, 300001, 7) FROM (SELECT json_agg(j ORDER BY id)::text AS s, jsonb_agg(jb ORDER BY id)::text AS sb FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT), Column("length", INT4), Column("substr", TEXT)],
                        rows: &[
                            &[T("1200016"), T(r#"aa", "b"#), T("1200016"), T(r#"aa", "b"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 1, 10), length(sb), substr(sb, 1, 10) FROM (SELECT json_object_agg(id, b ORDER BY id)::text AS s, jsonb_object_agg(id, b)::text AS sb FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT), Column("length", INT4), Column("substr", TEXT)],
                        rows: &[
                            &[T("1200042"), T(r#"{ "1" : "a"#), T("1200036"), T(r#"{"1": "aaa"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(s), substr(s, 300001, 12), length(sb), substr(sb, 300001, 12) FROM (SELECT json_object_agg(b, id ORDER BY id)::text AS s, jsonb_object_agg(b, id)::text AS sb FROM big) t;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("substr", TEXT), Column("length", INT4), Column("substr", TEXT)],
                        rows: &[
                            &[T("1200034"), T(r#"aaa" : 1, "b"#), T("1200028"), T(r#"aa": 1, "bbb"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(ARRAY[b, b]::text), length((ARRAY[b])[1]) FROM big WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("600003"), T("300000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(min(b)), length(max(b)), substr(max(b), 1, 3) FROM big;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("length", INT4), Column("substr", TEXT)],
                        rows: &[
                            &[T("300000"), T("300000"), T("ddd")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_setup_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_tuple_cost = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET min_parallel_table_scan_size = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET parallel_leader_participation = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT length(string_agg(b, ',')) FROM big;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Finalize Aggregate"#)],
                            &[T(r#"  ->  Gather"#)],
                            &[T(r#"        Workers Planned: 2"#)],
                            &[T(r#"        ->  Partial Aggregate"#)],
                            &[T(r#"              ->  Parallel Seq Scan on big"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(string_agg(b, ',')), length(string_agg(y, ','::bytea)) FROM big;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("1200003"), T("1200003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(array_agg(b)::text) FROM big;",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4)],
                        rows: &[
                            &[T("1200005")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_substring_of_multibyte_values() {
    run_scripts(&[
        ScriptTest {
            name: "SUBSTRING counts the characters of long values of 3-byte and 4-byte characters",
            set_up_script: &[
                "CREATE TABLE toast_utf8 (c TEXT);",
                "INSERT INTO toast_utf8 VALUES (repeat(U&'\\2026', 4000));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT SUBSTRING(c FROM 1 FOR 1), SUBSTRING(c FROM 4000 FOR 1), SUBSTRING(c FROM 4001 FOR 1), length(SUBSTRING(c FROM 3999)) FROM toast_utf8;",
                    expected: Expected::Rows {
                        columns: &[Column("substring", TEXT), Column("substring", TEXT), Column("substring", TEXT), Column("length", INT4)],
                        rows: &[
                            &[T("\u{2026}"), T("\u{2026}"), T(""), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE toast_utf8 SET c = repeat(U&'\\+01F680', 3000);",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT SUBSTRING(c FROM 3000 FOR 1), octet_length(SUBSTRING(c FROM 2 FOR 2000)), SUBSTRING(c FROM -2147483647 FOR 2147483647) FROM toast_utf8;",
                    expected: Expected::Rows {
                        columns: &[Column("substring", TEXT), Column("octet_length", INT4), Column("substring", TEXT)],
                        rows: &[
                            &[T("\u{1f680}"), T("8000"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
