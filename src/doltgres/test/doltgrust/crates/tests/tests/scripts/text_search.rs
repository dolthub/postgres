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
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};


#[test]
fn test_text_search_types() {
    run_scripts(&[
        ScriptTest {
            name: "tsvector and tsquery input, output, and send, with their errors",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'a fat cat sat on a mat and ate a fat rat'::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("tsvector", TSVECTOR)],
                        rows: &[
                            &[T("'a' 'and' 'ate' 'cat' 'fat' 'mat' 'on' 'rat' 'sat'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'b:3 a:1 b:1 a:2,2 c'::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("tsvector", TSVECTOR)],
                        rows: &[
                            &[T("'a':1,2 'b':1,3 'c'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1A b:2,3B c:4C,5D d:6d'::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("tsvector", TSVECTOR)],
                        rows: &[
                            &[T("'a':1A 'b':2,3B 'c':4C,5 'd':6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT $$'don''t' 'a\\\\b' 'sp ace' \\:x$$::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("tsvector", TSVECTOR)],
                        rows: &[
                            &[T("':x' 'a\\\\b' 'don''t' 'sp ace'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:16384 b:20000,1'::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("tsvector", TSVECTOR)],
                        rows: &[
                            &[T("'a':16383 'b':1,16383")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ''::tsvector, '   '::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("tsvector", TSVECTOR), Column("tsvector", TSVECTOR)],
                        rows: &[
                            &[T(""), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:0'::tsvector;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "wrong position info in tsvector: \"a:0\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1,'::tsvector;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error in tsvector: \"a:1,\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:'::tsvector;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error in tsvector: \"a:\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT $$'a$$::tsvector;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error in tsvector: \"'a\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT repeat('x', 2048)::tsvector;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "word is too long (2048 bytes, max 2046 bytes)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT length(repeat('x', 2047)::tsvector);",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "word is too long (2047 bytes, max 2046 bytes)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('a:1 b:2', 'tsvector'), pg_input_is_valid('a:', 'tsvector');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL), Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info($$'a$$, 'tsvector');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("syntax error in tsvector: \"'a\""), Null, Null, T("42601")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsvectorsend('a:1A,2 b c:3D'::tsvector);",
                    expected: Expected::Rows {
                        columns: &[Column("tsvectorsend", BYTEA)],
                        rows: &[
                            &[T("\\x0000000361000002c001000262000000630000010003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a & b | c'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("'a' & 'b' | 'c'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '!a <-> b'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("!'a' <-> 'b'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a <2> (b | c) <0> d'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("'a' <2> ( 'b' | 'c' ) <0> 'd'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:*AB & b:C* & c:*'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("'a':*AB & 'b':*C & 'c':*")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '(a | b) & !(c | d)'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("( 'a' | 'b' ) & !( 'c' | 'd' )")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a & (b & (c & d))'::tsquery, '((a & b) & c) & d'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY), Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("'a' & 'b' & 'c' & 'd'"), T("'a' & 'b' & 'c' & 'd'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '!!a & !(!b)'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("!!'a' & !!'b'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT $$'a b' & 'it''s'$$::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("'a b' & 'it''s'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ''::tsquery;",
                    notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", position: 8, ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("tsquery", TSQUERY)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a &'::tsquery;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "no operand in tsquery: \"a &\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '!'::tsquery;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "no operand in tsquery: \"!\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '(a | b'::tsquery;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error in tsquery: \"(a | b\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a <16385> b'::tsquery;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "distance in phrase operator must be an integer value between zero and 16384 inclusive", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a <-1> b'::tsquery;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error in tsquery: \"a <-1> b\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:Z'::tsquery;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error in tsquery: \"a:Z\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT repeat('x', 2048)::tsquery;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "word is too long in tsquery: \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('a & b', 'tsquery'), pg_input_is_valid('a &', 'tsquery');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL), Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('a <-> (b', 'tsquery');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("syntax error in tsquery: \"a <-> (b\""), Null, Null, T("42601")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsquerysend('!a & b:*A <3> c'::tsquery);",
                    expected: Expected::Rows {
                        columns: &[Column("tsquerysend", BYTEA)],
                        rows: &[
                            &[T("\\x000000060202020400030100006300010801620002010100006100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2'::tsvector::text, 'a <-> b'::tsquery::text;",
                    expected: Expected::Rows {
                        columns: &[Column("text", TEXT), Column("text", TEXT)],
                        rows: &[
                            &[T("'a':1 'b':2"), T("'a' <-> 'b'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_column_size('a:1 b'::tsvector), pg_column_size('a & b'::tsquery), pg_column_size(''::tsvector);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_column_size", INT4), Column("pg_column_size", INT4), Column("pg_column_size", INT4)],
                        rows: &[
                            &[T("23"), T("48"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("tsvector and tsquery are stored in Doltgrust's own form, whose size differs from Postgres' stored form"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tsvector functions",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT length('a:1 b:2,3 c'::tsvector), length(''::tsvector);",
                    expected: Expected::Rows {
                        columns: &[Column("length", INT4), Column("length", INT4)],
                        rows: &[
                            &[T("3"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT strip('a:1A b:2,3B c'::tsvector);",
                    expected: Expected::Rows {
                        columns: &[Column("strip", TSVECTOR)],
                        rows: &[
                            &[T("'a' 'b' 'c'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setweight('a:1 b:2B c'::tsvector, 'A');",
                    expected: Expected::Rows {
                        columns: &[Column("setweight", TSVECTOR)],
                        rows: &[
                            &[T("'a':1A 'b':2A 'c'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setweight('a:1 b:2B c:3'::tsvector, 'c', '{a,c,z}');",
                    expected: Expected::Rows {
                        columns: &[Column("setweight", TSVECTOR)],
                        rows: &[
                            &[T("'a':1C 'b':2B 'c':3C")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setweight('a:1'::tsvector, 'x');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized weight: \"x\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT setweight('a:1'::tsvector, 'b', '{a,NULL}');",
                    expected: Expected::Rows {
                        columns: &[Column("setweight", TSVECTOR)],
                        rows: &[
                            &[T("'a':1B")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2'::tsvector || 'b:1 c:3A'::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TSVECTOR)],
                        rows: &[
                            &[T("'a':1 'b':2,3 'c':5A")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a b'::tsvector || 'c:1 a:2'::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TSVECTOR)],
                        rows: &[
                            &[T("'a':2 'b' 'c':1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_delete('a:1 b:2 c:3'::tsvector, 'b'), ts_delete('a:1 b:2 c:3'::tsvector, 'z');",
                    expected: Expected::Rows {
                        columns: &[Column("ts_delete", TSVECTOR), Column("ts_delete", TSVECTOR)],
                        rows: &[
                            &[T("'a':1 'c':3"), T("'a':1 'b':2 'c':3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_delete('a:1 b:2 c:3'::tsvector, ARRAY['a', 'c', 'z']);",
                    expected: Expected::Rows {
                        columns: &[Column("ts_delete", TSVECTOR)],
                        rows: &[
                            &[T("'b':2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_delete('a:1'::tsvector, ARRAY['a', NULL]);",
                    expected: Expected::Rows {
                        columns: &[Column("ts_delete", TSVECTOR)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM unnest('a:1A,2 b c:3D'::tsvector);",
                    expected: Expected::Rows {
                        columns: &[Column("lexeme", TEXT), Column("positions", INT2_ARRAY), Column("weights", TEXT_ARRAY)],
                        rows: &[
                            &[T("a"), T("{1,2}"), T("{A,D}")],
                            &[T("b"), Null, Null],
                            &[T("c"), T("{3}"), T("{D}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lexeme, positions FROM unnest('b:3 a:1,2'::tsvector) ORDER BY lexeme DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("lexeme", TEXT), Column("positions", INT2_ARRAY)],
                        rows: &[
                            &[T("b"), T("{3}")],
                            &[T("a"), T("{1,2}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsvector_to_array('c:1 a b:2'::tsvector);",
                    expected: Expected::Rows {
                        columns: &[Column("tsvector_to_array", TEXT_ARRAY)],
                        rows: &[
                            &[T("{a,b,c}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_to_tsvector(ARRAY['b', 'a', 'b', 'c']);",
                    expected: Expected::Rows {
                        columns: &[Column("array_to_tsvector", TSVECTOR)],
                        rows: &[
                            &[T("'a' 'b' 'c'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_to_tsvector(ARRAY['a', NULL]);",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "lexeme array may not contain nulls", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_to_tsvector(ARRAY['a', '']);",
                    expected: Expected::Error(Diagnostic { code: "2200F", message: "lexeme array may not contain empty strings", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_filter('a:1A b:2B c:3C,4A d'::tsvector, '{a,c}');",
                    expected: Expected::Rows {
                        columns: &[Column("ts_filter", TSVECTOR)],
                        rows: &[
                            &[T("'a':1A 'c':3C,4A")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_filter('a:1A'::tsvector, '{x}');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized weight: \"x\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_filter('a:1A'::tsvector, '{a,NULL}');",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "weight array may not contain nulls", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tsquery functions, operators, and comparisons",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'a & b'::tsquery && 'c | d'::tsquery, 'a & b'::tsquery || 'c'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TSQUERY), Column("?column?", TSQUERY)],
                        rows: &[
                            &[T("'a' & 'b' & ( 'c' | 'd' )"), T("'a' & 'b' | 'c'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT !! 'a & b'::tsquery, !! 'a'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TSQUERY), Column("?column?", TSQUERY)],
                        rows: &[
                            &[T("!( 'a' & 'b' )"), T("!'a'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a'::tsquery <-> 'b | c'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TSQUERY)],
                        rows: &[
                            &[T("'a' <-> ( 'b' | 'c' )")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsquery_phrase('a'::tsquery, 'b <-> c'::tsquery), tsquery_phrase('a'::tsquery, 'b'::tsquery, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("tsquery_phrase", TSQUERY), Column("tsquery_phrase", TSQUERY)],
                        rows: &[
                            &[T("'a' <-> ( 'b' <-> 'c' )"), T("'a' <3> 'b'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsquery_phrase('a'::tsquery, 'b'::tsquery, -1);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "distance in phrase operator must be an integer value between zero and 16384 inclusive", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsquery_phrase('a'::tsquery, 'b'::tsquery, 20000);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "distance in phrase operator must be an integer value between zero and 16384 inclusive", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT numnode('a & b | !c'::tsquery), numnode('a'::tsquery), numnode(''::text::tsquery);",
                    notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("numnode", INT4), Column("numnode", INT4), Column("numnode", INT4)],
                        rows: &[
                            &[T("6"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT querytree('a & !b'::tsquery), querytree('!a'::tsquery), querytree('!a | b'::tsquery), querytree('a <-> !b'::tsquery);",
                    expected: Expected::Rows {
                        columns: &[Column("querytree", TEXT), Column("querytree", TEXT), Column("querytree", TEXT), Column("querytree", TEXT)],
                        rows: &[
                            &[T("'a'"), T("T"), T("T"), T("'a'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a & b | c'::tsquery @> 'b & a'::tsquery, 'a & b'::tsquery @> 'c'::tsquery, 'a'::tsquery <@ 'a | b'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a & b'::tsquery, 'a'::tsquery, 'c | d'::tsquery);",
                    expected: Expected::Rows {
                        columns: &[Column("ts_rewrite", TSQUERY)],
                        rows: &[
                            &[T("'b' & ( 'c' | 'd' )")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a & b & c'::tsquery, 'b & a'::tsquery, 'x'::tsquery);",
                    expected: Expected::Rows {
                        columns: &[Column("ts_rewrite", TSQUERY)],
                        rows: &[
                            &[T("'c' & 'x'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a <-> b | c'::tsquery, 'a <-> b'::tsquery, 'x <2> y'::tsquery);",
                    expected: Expected::Rows {
                        columns: &[Column("ts_rewrite", TSQUERY)],
                        rows: &[
                            &[T("'c' | 'x' <2> 'y'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a & b'::tsquery, 'a & b'::tsquery, ''::text::tsquery);",
                    notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("ts_rewrite", TSQUERY)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a & b'::tsquery, 'z'::tsquery, 'y'::tsquery);",
                    expected: Expected::Rows {
                        columns: &[Column("ts_rewrite", TSQUERY)],
                        rows: &[
                            &[T("'b' & 'a'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:A & b'::tsquery = 'a & b'::tsquery, 'a:*'::tsquery = 'a'::tsquery, 'a & b'::tsquery = 'b & a'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a'::tsquery < 'b'::tsquery, 'a & b'::tsquery < 'a'::tsquery, 'a <1> b'::tsquery < 'a <2> b'::tsquery, 'a'::tsquery >= 'a'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "tsvector @@ tsquery with phrases, prefixes, weights, and negation",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2'::tsvector @@ 'a <-> b'::tsquery, 'a:1 b:2'::tsvector @@ 'b <-> a'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:3'::tsvector @@ 'a <2> b'::tsquery, 'a:1 b:3'::tsvector @@ 'a <-> b'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2'::tsvector @@ '!c'::tsquery, 'a:1 b:2'::tsvector @@ '!a'::tsquery, 'a:1 b:2'::tsvector @@ 'a & !b'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'abc:1 def:2'::tsvector @@ 'ab:*'::tsquery, 'abc:1'::tsvector @@ 'abd:*'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1A b:2B'::tsvector @@ 'a:A'::tsquery, 'a:1A b:2B'::tsvector @@ 'b:A'::tsquery, 'a:1A b:2B'::tsvector @@ 'b:AB'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a b'::tsvector @@ 'a <-> b'::tsquery, 'a b'::tsvector @@ 'a:A'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2 c:3'::tsvector @@ '(a <-> b) <-> c'::tsquery, 'a:1 b:2 c:3'::tsvector @@ 'a <-> (b <-> c)'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2 c:3'::tsvector @@ 'a <-> !c'::tsquery, 'a:1 b:2 c:3'::tsvector @@ 'a <-> !b'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2 c:5'::tsvector @@ '(a | c) <-> b'::tsquery, 'a:1,4 b:2 c:5'::tsvector @@ 'a <-> (b | c)'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a:1 b:2 c:3 d:4'::tsvector @@ '(a <-> b) & (c <-> d)'::tsquery, 'a:1 b:2 c:3 d:4'::tsvector @@ '(a & b) <-> c'::tsquery;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'a b c'::tsquery @@ 'a:1 b:2'::tsvector;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error in tsquery: \"a b c\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'b & c'::tsquery @@ 'a:1 b:2'::tsvector, 'a <0> a'::tsquery @@ 'a:1'::tsvector;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ''::tsvector @@ 'a'::tsquery, 'a'::tsvector @@ ''::text::tsquery;",
                    notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", ..N }],
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("f"), T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "primary keys and btree indexes over tsvector and tsquery",
            set_up_script: &[
                "CREATE TABLE v (v TSVECTOR PRIMARY KEY);",
                "CREATE TABLE q (q TSQUERY PRIMARY KEY);",
                "CREATE TABLE t (id INT PRIMARY KEY, v TSVECTOR, q TSQUERY);",
                "CREATE INDEX t_v ON t (v);",
                "CREATE INDEX t_q ON t (q);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO v VALUES ('b a'), ('c'), ('a:1 b'), ('a:2 b'), ('a:1A b'), ('a:1,2 b'), (''), ('ab'), ('a');",
                    expected: Expected::Tag("INSERT 0 9"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM v ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TSVECTOR)],
                        rows: &[
                            &[T("")],
                            &[T("'a'")],
                            &[T("'c'")],
                            &[T("'ab'")],
                            &[T("'a' 'b'")],
                            &[T("'a':2 'b'")],
                            &[T("'a':1A 'b'")],
                            &[T("'a':1 'b'")],
                            &[T("'a':1,2 'b'")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM v ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TSVECTOR)],
                        rows: &[
                            &[T("'a':1,2 'b'")],
                            &[T("'a':1 'b'")],
                            &[T("'a':1A 'b'")],
                            &[T("'a':2 'b'")],
                            &[T("'a' 'b'")],
                            &[T("'ab'")],
                            &[T("'c'")],
                            &[T("'a'")],
                            &[T("")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v VALUES ('a b');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"v_pkey\"", detail: "Key (v)=('a' 'b') already exists.", schema: "public", table: "v", constraint: "v_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO v VALUES ('b:1 a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM v WHERE v > 'a b' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TSVECTOR)],
                        rows: &[
                            &[T("'a' 'b':1")],
                            &[T("'a':2 'b'")],
                            &[T("'a':1A 'b'")],
                            &[T("'a':1 'b'")],
                            &[T("'a':1,2 'b'")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES ('a & b'), ('a | b'), ('a'), ('b'), ('!a'), ('a <-> b'), ('a <2> b'), ('a & b & c'), ('ab'), ('(a | b) & c');",
                    expected: Expected::Tag("INSERT 0 10"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT q FROM q ORDER BY q;",
                    expected: Expected::Rows {
                        columns: &[Column("q", TSQUERY)],
                        rows: &[
                            &[T("'a'")],
                            &[T("'b'")],
                            &[T("'ab'")],
                            &[T("!'a'")],
                            &[T("'a' <2> 'b'")],
                            &[T("'a' <-> 'b'")],
                            &[T("'a' | 'b'")],
                            &[T("'a' & 'b'")],
                            &[T("( 'a' | 'b' ) & 'c'")],
                            &[T("'a' & 'b' & 'c'")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT q FROM q ORDER BY q DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("q", TSQUERY)],
                        rows: &[
                            &[T("'a' & 'b' & 'c'")],
                            &[T("( 'a' | 'b' ) & 'c'")],
                            &[T("'a' & 'b'")],
                            &[T("'a' | 'b'")],
                            &[T("'a' <-> 'b'")],
                            &[T("'a' <2> 'b'")],
                            &[T("!'a'")],
                            &[T("'ab'")],
                            &[T("'b'")],
                            &[T("'a'")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES ('b & a');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO q VALUES ('a:*B');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"q_pkey\"", detail: "Key (q)=('a':*B) already exists.", schema: "public", table: "q", constraint: "q_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (1, 'a b', 'a & b'), (2, 'c', 'c | d'), (3, 'a:1 b:2', 'a <-> b'), (4, NULL, NULL);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE v = 'a b';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE q = 'b & a' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v FROM t WHERE v >= 'a b' ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", TSVECTOR)],
                        rows: &[
                            &[T("1"), T("'a' 'b'")],
                            &[T("3"), T("'a':1 'b':2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, q FROM t WHERE q > 'c' ORDER BY q;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("q", TSQUERY)],
                        rows: &[
                            &[T("3"), T("'a' <-> 'b'")],
                            &[T("2"), T("'c' | 'd'")],
                            &[T("1"), T("'a' & 'b'")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE v @@ 'a <-> b' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE v SET v = v || 'z' WHERE v = 'c';",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM v WHERE v = 'c z';",
                    expected: Expected::Rows {
                        columns: &[Column("v", TSVECTOR)],
                        rows: &[
                            &[T("'c' 'z'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM q WHERE q = 'a <2> b';",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM q;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("10")],
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
fn test_text_search_catalogs() {
    run_scripts(&[
        ScriptTest {
            name: "text search configurations and their mappings",
            set_up_script: &[
                "CREATE SCHEMA s;",
                "CREATE ROLE ts_owner;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 'english'::regconfig, 'pg_catalog.simple'::regconfig, 'english'::regconfig::oid, 'simple'::regdictionary, 'english_stem'::regdictionary::oid;",
                    expected: Expected::Rows {
                        columns: &[Column("regconfig", REGCONFIG), Column("regconfig", REGCONFIG), Column("oid", OID), Column("regdictionary", REGDICTIONARY), Column("oid", OID)],
                        rows: &[
                            &[T("english"), T("simple"), T("13654"), T("simple"), T("13653")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'nope'::regconfig;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search configuration \"nope\" does not exist", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'nope'::regdictionary;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search dictionary \"nope\" does not exist", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 3748::regconfig, 0::regconfig, 99999::regconfig, 3765::regdictionary;",
                    expected: Expected::Rows {
                        columns: &[Column("regconfig", REGCONFIG), Column("regconfig", REGCONFIG), Column("regconfig", REGCONFIG), Column("regdictionary", REGDICTIONARY)],
                        rows: &[
                            &[T("simple"), T("-"), T("99999"), T("simple")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ts_config;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ts_dict;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT prsname, prsstart::text, prstoken::text, prsend::text, prsheadline::text, prslextype::text FROM pg_ts_parser;",
                    expected: Expected::Rows {
                        columns: &[Column("prsname", NAME), Column("prsstart", TEXT), Column("prstoken", TEXT), Column("prsend", TEXT), Column("prsheadline", TEXT), Column("prslextype", TEXT)],
                        rows: &[
                            &[T("default"), T("prsd_start"), T("prsd_nexttoken"), T("prsd_end"), T("prsd_headline"), T("prsd_lextype")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tmplname, tmplinit::text, tmpllexize::text FROM pg_ts_template ORDER BY tmplname;",
                    expected: Expected::Rows {
                        columns: &[Column("tmplname", NAME), Column("tmplinit", TEXT), Column("tmpllexize", TEXT)],
                        rows: &[
                            &[T("ispell"), T("dispell_init"), T("dispell_lexize")],
                            &[T("simple"), T("dsimple_init"), T("dsimple_lexize")],
                            &[T("snowball"), T("dsnowball_init"), T("dsnowball_lexize")],
                            &[T("synonym"), T("dsynonym_init"), T("dsynonym_lexize")],
                            &[T("thesaurus"), T("thesaurus_init"), T("thesaurus_lexize")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(DISTINCT mapcfg) FROM pg_ts_config_map;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("570"), T("30")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT maptokentype, mapseqno, mapdict::regdictionary FROM pg_ts_config_map WHERE mapcfg = 'english'::regconfig ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("maptokentype", INT4), Column("mapseqno", INT4), Column("mapdict", REGDICTIONARY)],
                        rows: &[
                            &[T("1"), T("1"), T("english_stem")],
                            &[T("2"), T("1"), T("english_stem")],
                            &[T("3"), T("1"), T("simple")],
                            &[T("4"), T("1"), T("simple")],
                            &[T("5"), T("1"), T("simple")],
                            &[T("6"), T("1"), T("simple")],
                            &[T("7"), T("1"), T("simple")],
                            &[T("8"), T("1"), T("simple")],
                            &[T("9"), T("1"), T("simple")],
                            &[T("10"), T("1"), T("english_stem")],
                            &[T("11"), T("1"), T("english_stem")],
                            &[T("15"), T("1"), T("simple")],
                            &[T("16"), T("1"), T("english_stem")],
                            &[T("17"), T("1"), T("english_stem")],
                            &[T("18"), T("1"), T("simple")],
                            &[T("19"), T("1"), T("simple")],
                            &[T("20"), T("1"), T("simple")],
                            &[T("21"), T("1"), T("simple")],
                            &[T("22"), T("1"), T("simple")],
                        ],
                        tag: "SELECT 19",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c1 (COPY = english);",
                    expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cfgname, cfgnamespace::regnamespace, cfgparser FROM pg_ts_config WHERE cfgname = 'c1';",
                    expected: Expected::Rows {
                        columns: &[Column("cfgname", NAME), Column("cfgnamespace", REGNAMESPACE), Column("cfgparser", OID)],
                        rows: &[
                            &[T("c1"), T("public"), T("3722")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ts_config_map WHERE mapcfg = 'c1'::regconfig;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("19")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c1 (COPY = english);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"pg_ts_config_cfgname_index\"", detail: "Key (cfgname, cfgnamespace)=(c1, 2200) already exists.", schema: "pg_catalog", table: "pg_ts_config", constraint: "pg_ts_config_cfgname_index", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION s.c2 (PARSER = default);",
                    expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ts_config_map WHERE mapcfg = 's.c2'::regconfig;",
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
                    query: "SELECT 'c2'::regconfig;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search configuration \"c2\" does not exist", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 's.c2'::regconfig;",
                    expected: Expected::Rows {
                        columns: &[Column("regconfig", REGCONFIG)],
                        rows: &[
                            &[T("s.c2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 ADD MAPPING FOR asciiword, word WITH simple, english_stem;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT maptokentype, mapseqno, mapdict::regdictionary FROM pg_ts_config_map WHERE mapcfg = 's.c2'::regconfig ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("maptokentype", INT4), Column("mapseqno", INT4), Column("mapdict", REGDICTIONARY)],
                        rows: &[
                            &[T("1"), T("1"), T("simple")],
                            &[T("1"), T("2"), T("english_stem")],
                            &[T("2"), T("1"), T("simple")],
                            &[T("2"), T("2"), T("english_stem")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 ADD MAPPING FOR nosuch WITH simple;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "token type \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 ALTER MAPPING FOR word WITH english_stem;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 ALTER MAPPING REPLACE simple WITH english_stem;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 ALTER MAPPING FOR asciiword REPLACE english_stem WITH simple;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT maptokentype, mapseqno, mapdict::regdictionary FROM pg_ts_config_map WHERE mapcfg = 's.c2'::regconfig ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("maptokentype", INT4), Column("mapseqno", INT4), Column("mapdict", REGDICTIONARY)],
                        rows: &[
                            &[T("1"), T("1"), T("simple")],
                            &[T("1"), T("2"), T("simple")],
                            &[T("2"), T("1"), T("english_stem")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 DROP MAPPING FOR word;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 DROP MAPPING FOR word;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "mapping for token type \"word\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 DROP MAPPING IF EXISTS FOR word, email;",
                    notices: &[Diagnostic { code: "00000", message: "mapping for token type \"word\" does not exist, skipping", ..N }, Diagnostic { code: "00000", message: "mapping for token type \"email\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT maptokentype, mapseqno, mapdict::regdictionary FROM pg_ts_config_map WHERE mapcfg = 's.c2'::regconfig ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("maptokentype", INT4), Column("mapseqno", INT4), Column("mapdict", REGDICTIONARY)],
                        rows: &[
                            &[T("1"), T("1"), T("simple")],
                            &[T("1"), T("2"), T("simple")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c2 ALTER MAPPING FOR asciiword WITH nosuch_dict;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search dictionary \"nosuch_dict\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c3 (PARSER = nosuch);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search parser \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c3 (PARSER = default, COPY = english);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot specify both PARSER and COPY options", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c3 (FOO = bar);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "text search configuration parameter \"foo\" not recognized", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c3 ();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \")\"", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION c1 RENAME TO c3;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c4 (COPY = c3);",
                    expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION c3 RENAME TO c4;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "text search configuration \"c4\" already exists in schema \"public\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION c4 SET SCHEMA s;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c4 (COPY = c3);",
                    expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION c4 SET SCHEMA s;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "text search configuration \"c4\" already exists in schema \"s\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH CONFIGURATION c4;",
                    expected: Expected::Tag("DROP TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION s.c4 OWNER TO ts_owner;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cfgname, cfgnamespace::regnamespace, pg_get_userbyid(cfgowner) FROM pg_ts_config WHERE cfgname LIKE 'c_' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("cfgname", NAME), Column("cfgnamespace", REGNAMESPACE), Column("pg_get_userbyid", NAME)],
                        rows: &[
                            &[T("c2"), T("s"), T("postgres")],
                            &[T("c3"), T("public"), T("postgres")],
                            &[T("c4"), T("s"), T("ts_owner")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TEXT SEARCH CONFIGURATION s.c4 IS 'a copy';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('s.c4'::regconfig, 'pg_ts_config');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("a copy")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_ts_config'::regclass, 's.c4'::regconfig, 0), pg_describe_object('pg_ts_dict'::regclass, 'simple'::regdictionary, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("text search configuration s.c4"), T("text search dictionary simple")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_identify_object('pg_ts_config'::regclass, 's.c4'::regconfig, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("text search configuration"), T("s"), T("c4"), T("s.c4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_identify_object('pg_ts_dict'::regclass, 'english_stem'::regdictionary, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("text search dictionary"), T("pg_catalog"), T("english_stem"), T("pg_catalog.english_stem")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_identify_object_as_address('pg_ts_config'::regclass, 's.c4'::regconfig, 0);",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("object_names", TEXT_ARRAY), Column("object_args", TEXT_ARRAY)],
                        rows: &[
                            &[T("text search configuration"), T("{s,c4}"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, refclassid::regclass, deptype FROM pg_depend WHERE objid = 's.c4'::regconfig ORDER BY 2;",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("refclassid", REGCLASS), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_ts_config"), T("pg_namespace"), T("n")],
                            &[T("pg_ts_config"), T("pg_ts_dict"), T("n")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_ts_config_is_visible('s.c4'::regconfig), pg_ts_config_is_visible('c3'::regconfig), pg_ts_dict_is_visible('simple'::regdictionary);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_ts_config_is_visible", BOOL), Column("pg_ts_config_is_visible", BOOL), Column("pg_ts_dict_is_visible", BOOL)],
                        rows: &[
                            &[T("f"), T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_ts_parser_is_visible(oid) FROM pg_ts_parser WHERE prsname = 'default';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_ts_parser_is_visible", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_ts_template_is_visible(oid) FROM pg_ts_template WHERE tmplname = 'snowball';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_ts_template_is_visible", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = s, public;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_ts_config_is_visible('s.c4'::regconfig), 'c4'::regconfig;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_ts_config_is_visible", BOOL), Column("regconfig", REGCONFIG)],
                        rows: &[
                            &[T("t"), T("c4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET search_path;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH CONFIGURATION nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search configuration \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH CONFIGURATION IF EXISTS nosuch;",
                    notices: &[Diagnostic { code: "00000", message: "text search configuration \"nosuch\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH CONFIGURATION IF EXISTS nosuch_schema.nosuch;",
                    notices: &[Diagnostic { code: "00000", message: "schema \"nosuch_schema\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH CONFIGURATION s.c4, s.c2;",
                    expected: Expected::Tag("DROP TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ts_config_map WHERE mapcfg NOT IN (SELECT oid FROM pg_ts_config);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "text search parsers, templates, and dictionaries",
            set_up_script: &[
                "CREATE SCHEMA s;",
                "CREATE ROLE ts_user;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH PARSER p1 (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype);",
                    expected: Expected::Tag("CREATE TEXT SEARCH PARSER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT prsname, prsstart::text, prstoken::text, prsend::text, prsheadline, prslextype::text FROM pg_ts_parser WHERE prsname = 'p1';",
                    expected: Expected::Rows {
                        columns: &[Column("prsname", NAME), Column("prsstart", TEXT), Column("prstoken", TEXT), Column("prsend", TEXT), Column("prsheadline", REGPROC), Column("prslextype", TEXT)],
                        rows: &[
                            &[T("p1"), T("prsd_start"), T("prsd_nexttoken"), T("prsd_end"), T("-"), T("prsd_lextype")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH PARSER p2 (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "text search parser lextypes method is required", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH PARSER p2 (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype, HEADLINE = prsd_headline, FOO = bar);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "text search parser parameter \"foo\" not recognized", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH PARSER s.p2 (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype, HEADLINE = prsd_headline);",
                    expected: Expected::Tag("CREATE TEXT SEARCH PARSER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT refclassid::regclass, refobjid = 's'::regnamespace, deptype FROM pg_depend WHERE classid = 'pg_ts_parser'::regclass AND objid IN (SELECT oid FROM pg_ts_parser WHERE prsname = 'p2') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("refclassid", REGCLASS), Column("?column?", BOOL), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_namespace"), T("t"), T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH CONFIGURATION c1 (PARSER = p1);",
                    expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION c1 ADD MAPPING FOR word WITH simple;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH PARSER p1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop text search parser p1 because other objects depend on it", detail: "text search configuration c1 depends on text search parser p1", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH PARSER p1 RENAME TO p3;",
                    expected: Expected::Tag("ALTER TEXT SEARCH PARSER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH PARSER p3 SET SCHEMA s;",
                    expected: Expected::Tag("ALTER TEXT SEARCH PARSER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cfgparser = (SELECT oid FROM pg_ts_parser WHERE prsname = 'p3') FROM pg_ts_config WHERE cfgname = 'c1';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH TEMPLATE t1 (LEXIZE = dsimple_lexize);",
                    expected: Expected::Tag("CREATE TEXT SEARCH TEMPLATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH TEMPLATE t2 (INIT = dsimple_init);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "text search template lexize method is required", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH TEMPLATE t2 (LEXIZE = dsimple_lexize, BAR = baz);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "text search template parameter \"bar\" not recognized", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tmplname, tmplinit, tmpllexize::text FROM pg_ts_template WHERE tmplname = 't1';",
                    expected: Expected::Rows {
                        columns: &[Column("tmplname", NAME), Column("tmplinit", REGPROC), Column("tmpllexize", TEXT)],
                        rows: &[
                            &[T("t1"), T("-"), T("dsimple_lexize")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH DICTIONARY d1 (TEMPLATE = t1);",
                    expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH DICTIONARY d2 (TEMPLATE = t1, StopWords = english);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "text search template \"t1\" does not accept options", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH DICTIONARY d2 (FOO = bar);",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "text search template is required", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH DICTIONARY d2 (TEMPLATE = nosuch);",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search template \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dictname, dicttemplate = (SELECT oid FROM pg_ts_template WHERE tmplname = 't1'), dictinitoption, pg_get_userbyid(dictowner) FROM pg_ts_dict WHERE dictname = 'd1';",
                    expected: Expected::Rows {
                        columns: &[Column("dictname", NAME), Column("?column?", BOOL), Column("dictinitoption", TEXT), Column("pg_get_userbyid", NAME)],
                        rows: &[
                            &[T("d1"), T("t"), Null, T("postgres")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION c1 ALTER MAPPING FOR word WITH d1, simple;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH DICTIONARY d1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop text search dictionary d1 because other objects depend on it", detail: "text search configuration c1 depends on text search dictionary d1", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH TEMPLATE t1;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop text search template t1 because other objects depend on it", detail: "text search dictionary d1 depends on text search template t1\ntext search configuration c1 depends on text search dictionary d1", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH DICTIONARY d1 RENAME TO d3;",
                    expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH DICTIONARY d3 OWNER TO ts_user;",
                    expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH DICTIONARY d3 SET SCHEMA s;",
                    expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 's.d3'::regdictionary;",
                    expected: Expected::Rows {
                        columns: &[Column("regdictionary", REGDICTIONARY)],
                        rows: &[
                            &[T("s.d3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'd3'::regdictionary;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search dictionary \"d3\" does not exist", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT maptokentype, mapseqno, mapdict::regdictionary FROM pg_ts_config_map WHERE mapcfg = 'c1'::regconfig ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("maptokentype", INT4), Column("mapseqno", INT4), Column("mapdict", REGDICTIONARY)],
                        rows: &[
                            &[T("2"), T("1"), T("s.d3")],
                            &[T("2"), T("2"), T("simple")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH TEMPLATE t1 RENAME TO t3;",
                    expected: Expected::Tag("ALTER TEXT SEARCH TEMPLATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TEXT SEARCH TEMPLATE t3 IS 'a template';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TEXT SEARCH PARSER s.p3 IS 'a parser';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TEXT SEARCH DICTIONARY s.d3 IS 'a dictionary';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(oid, 'pg_ts_template') FROM pg_ts_template WHERE tmplname = 't3';",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("a template")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(oid, 'pg_ts_parser') FROM pg_ts_parser WHERE prsname = 'p3';",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("a parser")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description('s.d3'::regdictionary, 'pg_ts_dict');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("a dictionary")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_ts_parser'::regclass, oid, 0) FROM pg_ts_parser WHERE prsname = 'p3';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("text search parser s.p3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object('pg_ts_template'::regclass, oid, 0) FROM pg_ts_template WHERE tmplname = 't3';",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("text search template t3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object('pg_ts_parser'::regclass, oid, 0)).* FROM pg_ts_parser WHERE prsname = 'p3';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("text search parser"), T("s"), T("p3"), T("s.p3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object('pg_ts_template'::regclass, oid, 0)).* FROM pg_ts_template WHERE tmplname = 't3';",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("text search template"), T("public"), T("t3"), T("public.t3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (pg_identify_object_as_address('pg_ts_dict'::regclass, 's.d3'::regdictionary, 0)).*;",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("object_names", TEXT_ARRAY), Column("object_args", TEXT_ARRAY)],
                        rows: &[
                            &[T("text search dictionary"), T("{s,d3}"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ts_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH PARSER p4 (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create text search parsers", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TEXT SEARCH TEMPLATE t4 (LEXIZE = dsimple_lexize);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be superuser to create text search templates", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH PARSER s.p3 RENAME TO p4;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH DICTIONARY s.d3 RENAME TO d4;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema s", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION c1 RENAME TO c4;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of text search configuration c1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH CONFIGURATION c1;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of text search configuration c1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET default_text_search_config = 'c1';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SHOW default_text_search_config;",
                    expected: Expected::Rows {
                        columns: &[Column("default_text_search_config", TEXT)],
                        rows: &[
                            &[T("public.c1")],
                        ],
                        tag: "SHOW",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET default_text_search_config;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH DICTIONARY IF EXISTS nosuch;",
                    notices: &[Diagnostic { code: "00000", message: "text search dictionary \"nosuch\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TEXT SEARCH DICTIONARY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH PARSER IF EXISTS nosuch;",
                    notices: &[Diagnostic { code: "00000", message: "text search parser \"nosuch\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TEXT SEARCH PARSER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH TEMPLATE IF EXISTS nosuch;",
                    notices: &[Diagnostic { code: "00000", message: "text search template \"nosuch\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TEXT SEARCH TEMPLATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TEXT SEARCH PARSER s.p3 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to text search configuration c1", ..N }],
                    expected: Expected::Tag("DROP TEXT SEARCH PARSER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ts_config WHERE cfgname = 'c1';",
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
                    query: "DROP TEXT SEARCH TEMPLATE t3 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to text search dictionary s.d3", ..N }],
                    expected: Expected::Tag("DROP TEXT SEARCH TEMPLATE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_ts_dict WHERE dictname = 'd4';",
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
                    query: "DROP OWNED BY ts_user;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_text_search_parsing_with_the_default_parser_and_simple_dictionaries() {
    run_scripts(&[ScriptTest {
        name: "the default parser, simple dictionaries, to_tsvector and to_tsquery, and default_text_search_config",
        set_up_script: &[
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "SELECT to_tsvector('simple', 'The Fat Rats ate 3.5 cheese-cakes at http://example.com/path?x=1 with foo@bar.com');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'/path?x=1':12 '3.5':5 'at':9 'ate':4 'cakes':8 'cheese':7 'cheese-cakes':6 'example.com':11 'example.com/path?x=1':10 'fat':2 'foo@bar.com':14 'rats':3 'the':1 'with':13")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT * FROM ts_parse('default', 'Hello, wörld! 123 4.5.6 <b>tag</b> &amp; -7 1e5 /usr/local/x.txt a-b-c');",
                expected: Expected::Rows {
                    columns: &[Column("tokid", INT4), Column("token", TEXT)],
                    rows: &[
                        &[T("1"), T("Hello")],
                        &[T("12"), T(", ")],
                        &[T("2"), T("wörld")],
                        &[T("12"), T("! ")],
                        &[T("22"), T("123")],
                        &[T("12"), T(" ")],
                        &[T("8"), T("4.5.6")],
                        &[T("12"), T(" ")],
                        &[T("13"), T("<b>")],
                        &[T("1"), T("tag")],
                        &[T("13"), T("</b>")],
                        &[T("12"), T(" ")],
                        &[T("23"), T("&amp;")],
                        &[T("12"), T(" ")],
                        &[T("21"), T("-7")],
                        &[T("12"), T(" ")],
                        &[T("7"), T("1e5")],
                        &[T("12"), T(" ")],
                        &[T("19"), T("/usr/local/x.txt")],
                        &[T("12"), T(" ")],
                        &[T("16"), T("a-b-c")],
                        &[T("11"), T("a")],
                        &[T("12"), T("-")],
                        &[T("11"), T("b")],
                        &[T("12"), T("-")],
                        &[T("11"), T("c")],
                    ],
                    tag: "SELECT 26",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT * FROM ts_parse(3722, '<script>x y</script> z <!-- c --> 2.0.1 v1.2');",
                expected: Expected::Rows {
                    columns: &[Column("tokid", INT4), Column("token", TEXT)],
                    rows: &[
                        &[T("13"), T("<script>")],
                        &[T("12"), T("x y")],
                        &[T("13"), T("</script>")],
                        &[T("12"), T(" ")],
                        &[T("1"), T("z")],
                        &[T("12"), T(" ")],
                        &[T("13"), T("<!-- c -->")],
                        &[T("12"), T(" ")],
                        &[T("8"), T("2.0.1")],
                        &[T("12"), T(" ")],
                        &[T("19"), T("v1.2")],
                    ],
                    tag: "SELECT 11",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*), min(alias), max(alias) FROM ts_token_type('default');",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8), Column("min", TEXT), Column("max", TEXT)],
                    rows: &[
                        &[T("23"), T("asciihword"), T("word")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT * FROM ts_token_type(3722) WHERE tokid IN (1, 12, 23);",
                expected: Expected::Rows {
                    columns: &[Column("tokid", INT4), Column("alias", TEXT), Column("description", TEXT)],
                    rows: &[
                        &[T("1"), T("asciiword"), T("Word, all ASCII")],
                        &[T("12"), T("blank"), T("Space symbols")],
                        &[T("23"), T("entity"), T("XML entity")],
                    ],
                    tag: "SELECT 3",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('simple', 'Hello'), ts_lexize('simple', '');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{hello}"), T("{}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('simple', 'fat & (rat | cat) & !dog'), plainto_tsquery('simple', 'The fat rats'), phraseto_tsquery('simple', 'the fat rats');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY), Column("plainto_tsquery", TSQUERY), Column("phraseto_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'fat' & ( 'rat' | 'cat' ) & !'dog'"), T("'the' & 'fat' & 'rats'"), T("'the' <-> 'fat' <-> 'rats'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT websearch_to_tsquery('simple', '\"fat rat\" or cat -dog'), websearch_to_tsquery('simple', 'or'), websearch_to_tsquery('simple', 'a or');",
                expected: Expected::Rows {
                    columns: &[Column("websearch_to_tsquery", TSQUERY), Column("websearch_to_tsquery", TSQUERY), Column("websearch_to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'fat' <-> 'rat' | 'cat' & !'dog'"), T("'or'"), T("'a' & 'or'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT websearch_to_tsquery('simple', 'a OR -b \"c d\" (e) & f'), websearch_to_tsquery('simple', '\"unclosed quote');",
                expected: Expected::Rows {
                    columns: &[Column("websearch_to_tsquery", TSQUERY), Column("websearch_to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'a' | !'b' & 'c' <-> 'd' & 'e' & 'f'"), T("'unclosed' <-> 'quote'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('simple', '{\"a\": \"Fat Rat\", \"b\": [1, true, \"cat\"]}'::jsonb), jsonb_to_tsvector('simple', '{\"a\": \"Fat Rat\", \"b\": [1, true]}'::jsonb, '[\"all\"]');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR), Column("jsonb_to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'cat':4 'fat':1 'rat':2"), T("'1':8 'a':1 'b':6 'fat':3 'rat':4 'true':10")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT json_to_tsvector('simple', '{\"a\": \"Fat Rat\", \"b\": [1, true]}'::json, '[\"key\", \"numeric\"]'), to_tsvector('simple', '{\"x\": \"y z\"}'::json);",
                expected: Expected::Rows {
                    columns: &[Column("json_to_tsvector", TSVECTOR), Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'1':5 'a':1 'b':3"), T("'y':1 'z':2")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT jsonb_to_tsvector('simple', '{}'::jsonb, '[\"nope\"]');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "wrong flag in flag array: \"nope\"", hint: "Possible values are: \"string\", \"numeric\", \"boolean\", \"key\", and \"all\".", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT jsonb_to_tsvector('simple', '{}'::jsonb, '{\"a\": 1}');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "wrong flag type, only arrays and scalars are allowed", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT jsonb_to_tsvector('simple', '{}'::jsonb, '[1]');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "flag array element is not a string", hint: "Possible values are: \"string\", \"numeric\", \"boolean\", \"key\", and \"all\".", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('simple', 'a b a b a c'), to_tsvector('simple', ''), to_tsquery('simple', ''), plainto_tsquery('simple', '');",
                notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", ..N }, Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", ..N }],
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR), Column("to_tsvector", TSVECTOR), Column("to_tsquery", TSQUERY), Column("plainto_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'a':1,3,5 'b':2,4 'c':6"), T(""), T(""), T("")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY eng_simple (TEMPLATE = simple, StopWords = english);",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('eng_simple', 'The'), ts_lexize('eng_simple', 'Rats');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{}"), T("{rats}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH CONFIGURATION c (COPY = simple);",
                expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION c ALTER MAPPING FOR asciiword WITH eng_simple;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('c', 'the fat rat and the cat'), to_tsquery('c', 'the & fat'), plainto_tsquery('c', 'the rat of the cat'), phraseto_tsquery('c', 'the rat of the cat');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR), Column("to_tsquery", TSQUERY), Column("plainto_tsquery", TSQUERY), Column("phraseto_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'cat':6 'fat':2 'rat':3"), T("'fat'"), T("'rat' & 'cat'"), T("'rat' <3> 'cat'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('c', 'the');",
                notices: &[Diagnostic { code: "00000", message: "text-search query contains only stop words or doesn't contain lexemes, ignored", ..N }],
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('c', 'fat <-> the <-> rat'), to_tsquery('c', '(the | fat) & !the');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY), Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'fat' <2> 'rat'"), T("'fat'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = simple, foo = 1);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized simple dictionary parameter: \"foo\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = simple, StopWords = english, StopWords = english);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "multiple StopWords parameters", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = simple, StopWords = 'bad/name');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "invalid text search configuration file name \"bad/name\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = simple, Accept = maybe);",
                expected: Expected::Error(Diagnostic { code: "42601", message: "accept requires a Boolean value", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY nostop (TEMPLATE = simple, Accept = false);",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('nostop', 'x');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[Null],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION c ALTER MAPPING FOR asciiword WITH nostop, simple;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('c', 'x y');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'x':1 'y':2")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SET default_text_search_config = 'c';",
                expected: Expected::Tag("SET"),
                ..A
            },
            ScriptTestAssertion {
                query: "SHOW default_text_search_config;",
                expected: Expected::Rows {
                    columns: &[Column("default_text_search_config", TEXT)],
                    rows: &[
                        &[T("public.c")],
                    ],
                    tag: "SHOW",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('the rat'), to_tsquery('rat'), plainto_tsquery('a rat'), phraseto_tsquery('a rat'), websearch_to_tsquery('a rat'), get_current_ts_config()::regconfig;",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR), Column("to_tsquery", TSQUERY), Column("plainto_tsquery", TSQUERY), Column("phraseto_tsquery", TSQUERY), Column("websearch_to_tsquery", TSQUERY), Column("get_current_ts_config", REGCONFIG)],
                    rows: &[
                        &[T("'rat':2 'the':1"), T("'rat'"), T("'a' & 'rat'"), T("'a' <-> 'rat'"), T("'a' & 'rat'"), T("c")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SET default_text_search_config = 'nosuch';",
                expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for parameter \"default_text_search_config\": \"nosuch\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SET default_text_search_config = 'pg_catalog.simple';",
                expected: Expected::Tag("SET"),
                ..A
            },
            ScriptTestAssertion {
                query: "SHOW default_text_search_config;",
                expected: Expected::Rows {
                    columns: &[Column("default_text_search_config", TEXT)],
                    rows: &[
                        &[T("pg_catalog.simple")],
                    ],
                    tag: "SHOW",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH DICTIONARY eng_simple (StopWords = russian);",
                expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('eng_simple', 'the'), ts_lexize('eng_simple', 'и');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{the}"), T("{}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH DICTIONARY eng_simple (StopWords = english);",
                expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "BEGIN;",
                expected: Expected::Tag("BEGIN"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH DICTIONARY eng_simple (StopWords = russian);",
                expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('eng_simple', 'the');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{the}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ROLLBACK;",
                expected: Expected::Tag("ROLLBACK"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('eng_simple', 'the');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_snowball_dictionaries() {
    run_scripts(&[ScriptTest {
        name: "Snowball dictionaries, the built-in language configurations, and their option errors",
        set_up_script: &[
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "SELECT to_tsvector('english', 'The quick brown foxes were jumping over the lazy dogs'), to_tsvector('english', 'a fat cat sat on a mat and ate a fat rat');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR), Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'brown':3 'dog':10 'fox':4 'jump':6 'lazi':9 'quick':2"), T("'ate':9 'cat':3 'fat':2,11 'mat':7 'rat':12 'sat':4")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('english', 'jumping & (foxes | dogs)'), plainto_tsquery('english', 'The running of the bulls'), phraseto_tsquery('english', 'the cats of the town'), websearch_to_tsquery('english', '\"fat rats\" or cats -dogs');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY), Column("plainto_tsquery", TSQUERY), Column("phraseto_tsquery", TSQUERY), Column("websearch_to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'jump' & ( 'fox' | 'dog' )"), T("'run' & 'bull'"), T("'cat' <3> 'town'"), T("'fat' <-> 'rat' | 'cat' & !'dog'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('english', 'supernovae stars') @@ to_tsquery('english', 'supernova'), to_tsvector('english', 'connections connected') @@ to_tsquery('english', 'connect');",
                expected: Expected::Rows {
                    columns: &[Column("?column?", BOOL), Column("?column?", BOOL)],
                    rows: &[
                        &[T("t"), T("t")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('english_stem', 'stars'), ts_lexize('english_stem', 'the'), ts_lexize('russian_stem', 'красивые'), ts_lexize('german_stem', 'häuser'), ts_lexize('french_stem', 'chevaux');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{star}"), T("{}"), T("{красив}"), T("{haus}"), T("{cheval}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('russian', 'москва, столица россии'), to_tsvector('spanish', 'Los niños están jugando en el parque'), to_tsvector('german', 'Die Kinder spielen im Garten');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR), Column("to_tsvector", TSVECTOR), Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'москв':1 'росс':3 'столиц':2"), T("'jug':4 'niñ':2 'parqu':7"), T("'gart':5 'kind':2 'spiel':3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('english_stem', repeat('a', 1001)) = ARRAY[repeat('a', 1001)];",
                expected: Expected::Rows {
                    columns: &[Column("?column?", BOOL)],
                    rows: &[
                        &[T("t")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT cfgname FROM pg_ts_config WHERE cfgparser <> 0 ORDER BY 1 LIMIT 5;",
                expected: Expected::Rows {
                    columns: &[Column("cfgname", NAME)],
                    rows: &[
                        &[T("arabic")],
                        &[T("armenian")],
                        &[T("basque")],
                        &[T("catalan")],
                        &[T("danish")],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY my_stem (TEMPLATE = snowball, Language = Porter);",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('my_stem', 'generalizations'), ts_lexize('my_stem', 'Running');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{gener}"), T("{run}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY my_stem2 (TEMPLATE = snowball, Language = estonian, StopWords = english);",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('my_stem2', 'the'), ts_lexize('my_stem2', 'raamatutest');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{}"), T("{raama}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = snowball);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "missing Language parameter", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = snowball, Language = klingon);",
                expected: Expected::Error(Diagnostic { code: "42704", message: "no Snowball stemmer available for language \"klingon\" and encoding \"UTF8\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = snowball, Language = english, Language = english);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "multiple Language parameters", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (TEMPLATE = snowball, Language = english, Foo = bar);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized Snowball parameter: \"foo\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH CONFIGURATION my_cfg (COPY = english);",
                expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION my_cfg ALTER MAPPING FOR asciiword WITH my_stem;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('my_cfg', 'generalizations of running dogs');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'dog':4 'gener':1 'of':2 'run':3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SET default_text_search_config = 'english';",
                expected: Expected::Tag("SET"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('Running dogs'), to_tsquery('running & dogs'), get_current_ts_config()::regconfig;",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR), Column("to_tsquery", TSQUERY), Column("get_current_ts_config", REGCONFIG)],
                    rows: &[
                        &[T("'dog':2 'run':1"), T("'run' & 'dog'"), T("english")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT 'The fat rats ran' @@ 'rat & fat'::tsquery, 'The fat rats' @@ 'cats'::tsquery, 'The fat cats are sitting' @@ 'sitting cats', 'fat cats' @@ 'fat dogs';",
                expected: Expected::Rows {
                    columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                    rows: &[
                        &[T("t"), T("f"), T("t"), T("f")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH DICTIONARY my_stem (Language = english);",
                expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('my_stem', 'generalizations');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{general}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_synonym_and_thesaurus_dictionaries() {
    run_scripts(&[ScriptTest {
        name: "synonym and thesaurus dictionaries over PG18's sample files, alone and in configurations",
        set_up_script: &[
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY synonym (Template = synonym, Synonyms = synonym_sample);",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('synonym', 'PoStGrEs'), ts_lexize('synonym', 'Gogle'), ts_lexize('synonym', 'indices'), ts_lexize('synonym', 'nothing');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{pgsql}"), T("{googl}"), T("{index}"), Null],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH DICTIONARY synonym (CaseSensitive = 1);",
                expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('synonym', 'PoStGrEs'), ts_lexize('synonym', 'postgres');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[Null, T("{pgsql}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH DICTIONARY synonym (CaseSensitive = 2);",
                expected: Expected::Error(Diagnostic { code: "42601", message: "casesensitive requires a Boolean value", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH DICTIONARY synonym (CaseSensitive = off);",
                expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('synonym', 'PoStGrEs');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{pgsql}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (Template = synonym);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "missing Synonyms parameter", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (Template = synonym, Synonyms = nosuch_file, Foo = 1);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized synonym parameter: \"foo\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY thesaurus (Template = thesaurus, DictFile = thesaurus_sample, Dictionary = english_stem);",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('thesaurus', 'one');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{1}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (Template = thesaurus, DictFile = thesaurus_sample);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "missing Dictionary parameter", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (Template = thesaurus, Dictionary = english_stem);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "missing DictFile parameter", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (Template = thesaurus, DictFile = thesaurus_sample, Dictionary = nosuch);",
                expected: Expected::Error(Diagnostic { code: "42704", message: "text search dictionary \"nosuch\" does not exist", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (Template = thesaurus, DictFile = thesaurus_sample, Dictionary = thesaurus);",
                expected: Expected::Error(Diagnostic { code: "XX000", message: "forbidden call of thesaurus or nested call", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY bad (Template = thesaurus, DictFile = thesaurus_sample, Dictionary = simple, Bar = 1);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized Thesaurus parameter: \"bar\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH CONFIGURATION synonym_tst (COPY = english);",
                expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION synonym_tst ALTER MAPPING FOR asciiword, hword_asciipart, asciihword WITH synonym, english_stem;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('synonym_tst', 'Postgresql is often called as postgres or pgsql and pronounced as postgre');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'call':4 'often':3 'pgsql':1,6,8,12 'pronounc':10")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('synonym_tst', 'Most common mistake is to write Gogle instead of Google');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'common':2 'googl':7,10 'instead':8 'mistak':3 'write':6")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('synonym_tst', 'Indexes or indices - Which is right plural form of index?');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'form':8 'index':1,3,10 'plural':7 'right':6")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('synonym_tst', 'Index & indices');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'index' & 'index':*")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH CONFIGURATION thesaurus_tst (COPY = synonym_tst);",
                expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION thesaurus_tst ALTER MAPPING FOR asciiword, hword_asciipart, asciihword WITH synonym, thesaurus, english_stem;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('thesaurus_tst', 'one postgres one two one two three one');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'1':1,5 '12':3 '123':4 'pgsql':2")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('thesaurus_tst', 'Supernovae star is very new star and usually called supernovae (abbreviation SN)');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'abbrevi':10 'call':8 'new':4 'sn':1,9,11 'star':5 'usual':7")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('thesaurus_tst', 'Booking tickets is looking like a booking a tickets');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'card':3,10 'invit':2,9 'like':6 'look':5 'order':1,8")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('thesaurus_tst', 'one & two & three'), plainto_tsquery('thesaurus_tst', 'supernovae star'), phraseto_tsquery('thesaurus_tst', 'one two');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY), Column("plainto_tsquery", TSQUERY), Column("phraseto_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'1' & '2' & 'three'"), T("'sn'"), T("'12'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_ispell_and_hunspell_dictionaries() {
    run_scripts(&[ScriptTest {
        name: "tsdicts.sql's Ispell and Hunspell dictionaries over PG18's sample files, alone and in configurations",
        set_up_script: &[
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY ispell ( Template=ispell, DictFile=ispell_sample, AffFile=ispell_sample );",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'skies');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{sky}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'bookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'booking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'foot');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'foots');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'rebookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'rebooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'rebook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[Null],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'unbookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'unbooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'unbook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'footklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{footballklubber,foot,ball,klubber,football,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'ballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('ispell', 'footballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell ( Template=ispell, DictFile=ispell_sample, AffFile=hunspell_sample );",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'skies');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{sky}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'bookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'booking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'foot');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'foots');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'rebookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'rebooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'rebook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[Null],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'unbookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'unbooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'unbook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'footklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{footballklubber,foot,ball,klubber,football,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'ballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell', 'footballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_long ( Template=ispell, DictFile=hunspell_sample_long, AffFile=hunspell_sample_long );",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'skies');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{sky}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'bookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'booking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'foot');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'foots');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'rebookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'rebooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'rebook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[Null],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'unbookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'unbooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'unbook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'booked');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'footklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{footballklubber,foot,ball,klubber,football,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'ballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'ballsklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'footballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_long', 'ex-machina');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{ex-,machina}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_num ( Template=ispell, DictFile=hunspell_sample_num, AffFile=hunspell_sample_num );",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'skies');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{sky}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'sk');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{sky}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'bookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'booking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'foot');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'foots');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'rebookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'rebooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{booking,book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'rebook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[Null],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'unbookings');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'unbooking');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'unbook');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'booked');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{book}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'footklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{footballklubber,foot,ball,klubber,football,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'ballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_lexize('hunspell_num', 'footballyklubber');",
                expected: Expected::Rows {
                    columns: &[Column("ts_lexize", TEXT_ARRAY)],
                    rows: &[
                        &[T("{foot,ball,klubber}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_err ( Template=ispell, DictFile=ispell_sample, AffFile=hunspell_sample_long );",
                expected: Expected::Error(Diagnostic { code: "F0000", message: "invalid affix alias \"GJUS\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_err ( Template=ispell, DictFile=ispell_sample, AffFile=hunspell_sample_num );",
                expected: Expected::Error(Diagnostic { code: "F0000", message: "invalid affix flag \"SZ\\\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_invalid_1 ( Template=ispell, DictFile=hunspell_sample_long, AffFile=ispell_sample );",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_invalid_2 ( Template=ispell, DictFile=hunspell_sample_long, AffFile=hunspell_sample_num );",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_invalid_3 ( Template=ispell, DictFile=hunspell_sample_num, AffFile=ispell_sample );",
                expected: Expected::Tag("CREATE TEXT SEARCH DICTIONARY"),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY hunspell_err ( Template=ispell, DictFile=hunspell_sample_num, AffFile=hunspell_sample_long );",
                expected: Expected::Error(Diagnostic { code: "F0000", message: "invalid affix alias \"302,301,202,303\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH CONFIGURATION ispell_tst ( COPY=english );",
                expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION ispell_tst ALTER MAPPING FOR word, numword, asciiword, hword, numhword, asciihword, hword_part, hword_numpart, hword_asciipart WITH ispell, english_stem;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('ispell_tst', 'Booking the skies after rebookings for footballklubber from a foot');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'ball':7 'book':1,5 'booking':1,5 'foot':7,10 'football':7 'footballklubber':7 'klubber':7 'sky':3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('ispell_tst', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'footballklubber' | 'foot' & 'ball' & 'klubber' | 'football' & 'klubber'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('ispell_tst', 'footballyklubber:b & rebookings:A & sky');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'foot':B & 'ball':B & 'klubber':B & ( 'booking':A | 'book':A ) & 'sky'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH CONFIGURATION hunspell_tst ( COPY=ispell_tst );",
                expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION hunspell_tst ALTER MAPPING REPLACE ispell WITH hunspell;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('hunspell_tst', 'Booking the skies after rebookings for footballklubber from a foot');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'ball':7 'book':1,5 'booking':1,5 'foot':7,10 'football':7 'footballklubber':7 'klubber':7 'sky':3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('hunspell_tst', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'footballklubber' | 'foot' & 'ball' & 'klubber' | 'football' & 'klubber'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('hunspell_tst', 'footballyklubber:b & rebookings:A & sky');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'foot':B & 'ball':B & 'klubber':B & ( 'booking':A | 'book':A ) & 'sky'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('hunspell_tst', 'footballyklubber:b <-> sky');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("( 'foot':B & 'ball':B & 'klubber':B ) <-> 'sky'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT phraseto_tsquery('hunspell_tst', 'footballyklubber sky');",
                expected: Expected::Rows {
                    columns: &[Column("phraseto_tsquery", TSQUERY)],
                    rows: &[
                        &[T("( 'foot' & 'ball' & 'klubber' ) <-> 'sky'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION hunspell_tst ALTER MAPPING REPLACE hunspell WITH hunspell_long;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('hunspell_tst', 'Booking the skies after rebookings for footballklubber from a foot');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'ball':7 'book':1,5 'booking':1,5 'foot':7,10 'football':7 'footballklubber':7 'klubber':7 'sky':3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('hunspell_tst', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'footballklubber' | 'foot' & 'ball' & 'klubber' | 'football' & 'klubber'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('hunspell_tst', 'footballyklubber:b & rebookings:A & sky');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'foot':B & 'ball':B & 'klubber':B & ( 'booking':A | 'book':A ) & 'sky'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION hunspell_tst ALTER MAPPING REPLACE hunspell_long WITH hunspell_num;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsvector('hunspell_tst', 'Booking the skies after rebookings for footballklubber from a foot');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsvector", TSVECTOR)],
                    rows: &[
                        &[T("'ball':7 'book':1,5 'booking':1,5 'foot':7,10 'football':7 'footballklubber':7 'klubber':7 'sky':3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('hunspell_tst', 'footballklubber');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'footballklubber' | 'foot' & 'ball' & 'klubber' | 'football' & 'klubber'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT to_tsquery('hunspell_tst', 'footballyklubber:b & rebookings:A & sky');",
                expected: Expected::Rows {
                    columns: &[Column("to_tsquery", TSQUERY)],
                    rows: &[
                        &[T("'foot':B & 'ball':B & 'klubber':B & ( 'booking':A | 'book':A ) & 'sky'")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH DICTIONARY tsdict_case ( Template = ispell, \"DictFile\" = ispell_sample, \"AffFile\" = ispell_sample );",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized Ispell parameter: \"DictFile\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TEXT SEARCH CONFIGURATION dummy_tst (COPY=english);",
                expected: Expected::Tag("CREATE TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst ALTER MAPPING FOR word, word WITH ispell;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst DROP MAPPING FOR not_a_token, not_a_token;",
                expected: Expected::Error(Diagnostic { code: "22023", message: "token type \"not_a_token\" does not exist", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst DROP MAPPING IF EXISTS FOR not_a_token, not_a_token;",
                expected: Expected::Error(Diagnostic { code: "22023", message: "token type \"not_a_token\" does not exist", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst DROP MAPPING FOR word, word;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst DROP MAPPING FOR word;",
                expected: Expected::Error(Diagnostic { code: "42704", message: "mapping for token type \"word\" does not exist", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst DROP MAPPING IF EXISTS FOR word, word;",
                notices: &[Diagnostic { code: "00000", message: "mapping for token type \"word\" does not exist, skipping", ..N }],
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst ADD MAPPING FOR word, word WITH ispell;",
                expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                ..A
            },
            ScriptTestAssertion {
                query: "ALTER TEXT SEARCH CONFIGURATION dummy_tst ADD MAPPING FOR not_a_token WITH ispell;",
                expected: Expected::Error(Diagnostic { code: "22023", message: "token type \"not_a_token\" does not exist", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "DROP TEXT SEARCH CONFIGURATION dummy_tst;",
                expected: Expected::Tag("DROP TEXT SEARCH CONFIGURATION"),
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_objects_named_as_built_in_ones() {
    run_scripts(&[ScriptTest {
        name: "text search objects named as built-in ones fail on the catalogs' unique indexes",
        assertions: &[
            ScriptTestAssertion {
                query: r#"CREATE TEXT SEARCH CONFIGURATION pg_catalog.simple (COPY = simple);"#,
                expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "pg_ts_config_cfgname_index""#, detail: "Key (cfgname, cfgnamespace)=(simple, 11) already exists.", schema: "pg_catalog", table: "pg_ts_config", constraint: "pg_ts_config_cfgname_index", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: r#"CREATE TEXT SEARCH DICTIONARY pg_catalog.simple (TEMPLATE = simple);"#,
                expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "pg_ts_dict_dictname_index""#, detail: "Key (dictname, dictnamespace)=(simple, 11) already exists.", schema: "pg_catalog", table: "pg_ts_dict", constraint: "pg_ts_dict_dictname_index", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: r#"CREATE TEXT SEARCH PARSER pg_catalog."default" (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype);"#,
                expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "pg_ts_parser_prsname_index""#, detail: "Key (prsname, prsnamespace)=(default, 11) already exists.", schema: "pg_catalog", table: "pg_ts_parser", constraint: "pg_ts_parser_prsname_index", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: r#"CREATE TEXT SEARCH TEMPLATE pg_catalog.simple (LEXIZE = dsimple_lexize);"#,
                expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "pg_ts_template_tmplname_index""#, detail: "Key (tmplname, tmplnamespace)=(simple, 11) already exists.", schema: "pg_catalog", table: "pg_ts_template", constraint: "pg_ts_template_tmplname_index", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM pg_ts_config WHERE cfgname = 'simple';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "COMMENT ON FUNCTION pg_catalog.lower(text) IS 'a new comment';",
                expected: Expected::Tag("COMMENT"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT obj_description('pg_catalog.lower(text)'::regprocedure, 'pg_proc');",
                expected: Expected::Rows {
                    columns: &[Column("obj_description", TEXT)],
                    rows: &[
                        &[T("a new comment")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "COMMENT ON FUNCTION pg_catalog.lower(text) IS NULL;",
                expected: Expected::Tag("COMMENT"),
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_gin_indexes_and_estimates() {
    run_scripts(&[ScriptTest {
        name: "tsvector_ops GIN indexes and the estimates of @@ from a tsvector column's most common lexemes",
        set_up_script: &[
            "CREATE TABLE docs (id int, v tsvector);",
            "INSERT INTO docs SELECT i, to_tsvector('simple', 'w' || (i % 7) || ' x' || (i % 13) || ' y' || (i % 50) || ' z' || (i % 400)) FROM generate_series(1, 2000) i;",
            "INSERT INTO docs SELECT i, setweight(to_tsvector('simple', 'w' || (i % 7) || ' heavy'), 'A') FROM generate_series(2001, 2100) i;",
            "INSERT INTO docs SELECT i, NULL FROM generate_series(2101, 2150) i;",
            "INSERT INTO docs VALUES (2151, ''), (2152, 'w1:3 x2:1'), (2153, 'w1 x2');",
            "CREATE FUNCTION estimated_rows(text) RETURNS text LANGUAGE plpgsql AS $$ DECLARE ln text; BEGIN FOR ln IN EXECUTE format('EXPLAIN %s', $1) LOOP RETURN substring(ln FROM 'rows=(\\d+)'); END LOOP; END; $$;",
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w1''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("14")],
                    ],
                    tag: "SELECT 1",
                },
                skip: Some("a table never analyzed is estimated from its pages, which Doltgrust's storage counts differently"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w1 & x2''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w:*''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("56")],
                    ],
                    tag: "SELECT 1",
                },
                skip: Some("a table never analyzed is estimated from its pages, which Doltgrust's storage counts differently"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE ''w1''::tsquery @@ v');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("14")],
                    ],
                    tag: "SELECT 1",
                },
                skip: Some("a table never analyzed is estimated from its pages, which Doltgrust's storage counts differently"),
                ..A
            },
            ScriptTestAssertion {
                query: "ANALYZE docs;",
                expected: Expected::Tag("ANALYZE"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT null_frac, avg_width, n_distinct, array_length(most_common_elems::text::text[], 1), (most_common_elems::text::text[])[1:5], most_common_elem_freqs[1:5], most_common_elem_freqs[array_length(most_common_elem_freqs, 1) - 1:] FROM pg_stats WHERE tablename = 'docs' AND attname = 'v';",
                expected: Expected::Rows {
                    columns: &[Column("null_frac", FLOAT4), Column("avg_width", INT4), Column("n_distinct", FLOAT4), Column("array_length", INT4), Column("most_common_elems", TEXT_ARRAY), Column("most_common_elem_freqs", FLOAT4_ARRAY), Column("most_common_elem_freqs", FLOAT4_ARRAY)],
                    rows: &[
                        &[T("0.02322341"), T("48"), T("-0.9767766"), T("471"), T("{w0,w1,w2,w3,w4}"), T("{0.14265335,0.14360437,0.14265335,0.14265335,0.14265335}"), T("{0.002377556,0.14360437}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w1''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("302")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''heavy''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("100")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''no_such_lexeme''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w1 & x2''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("22")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w1 | x2''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("436")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''!w1''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("1801")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w1 <-> x2''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("22")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''w:*''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("1388")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''y1:*''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("401")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''q:*''');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE ''y12''::tsquery @@ v');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("40")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ ''''::tsquery');",
                notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", ..N }],
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ NULL::tsquery');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("0")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs WHERE v @@ to_tsquery(''simple'', ''w1'')');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("302")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT estimated_rows('SELECT * FROM docs d1, docs d2 WHERE d1.v @@ d1.v::text::tsquery AND d1.id = d2.id');",
                expected: Expected::Rows {
                    columns: &[Column("estimated_rows", TEXT)],
                    rows: &[
                        &[T("11")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX docs_v ON docs USING gin (v);",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SET enable_seqscan = off;",
                expected: Expected::Tag("SET"),
                ..A
            },
            ScriptTestAssertion {
                query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE v @@ 'w1 & x2';",
                expected: Expected::Rows {
                    columns: &[Column("QUERY PLAN", TEXT)],
                    rows: &[
                        &[T("Aggregate")],
                        &[T("  ->  Bitmap Heap Scan on docs")],
                        &[T("        Recheck Cond: (v @@ '''w1'' & ''x2'''::tsquery)")],
                        &[T("        ->  Bitmap Index Scan on docs_v")],
                        &[T("              Index Cond: (v @@ '''w1'' & ''x2'''::tsquery)")],
                    ],
                    tag: "EXPLAIN",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("302")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 & x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("24")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 | x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("434")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ '(w1 & x2) | (w2 & y3)';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("29")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w:*';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("2102")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'y1:* & w2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("62")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'he:* | z39:*';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("155")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'q:*';",
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
                query: "SELECT count(*) FROM docs WHERE v @@ 'no_such_lexeme';",
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
                query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE v @@ '!w1';",
                expected: Expected::Rows {
                    columns: &[Column("QUERY PLAN", TEXT)],
                    rows: &[
                        &[T("Aggregate")],
                        &[T("  ->  Bitmap Heap Scan on docs")],
                        &[T("        Recheck Cond: (v @@ '!''w1'''::tsquery)")],
                        &[T("        ->  Bitmap Index Scan on docs_v")],
                        &[T("              Index Cond: (v @@ '!''w1'''::tsquery)")],
                    ],
                    tag: "EXPLAIN",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ '!w1';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("1801")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ '!no_such_lexeme';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("2103")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 & !x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("278")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 | !x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("1971")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 <-> x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("22")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'x2 <-> y2';",
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
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 <2> y1';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("6")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ '!(w1 <-> x1)';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("2081")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1:A';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("15")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1:B';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ '!w1:A';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("2088")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1:*A';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("15")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 & w1 & x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("24")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ ANY ('{w1,x2}');",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("434")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ '';",
                notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", position: 38, ..N }],
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
                query: "SELECT id FROM docs WHERE v @@ 'w1 & x2' AND id > 2100 ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("2152")],
                        &[T("2153")],
                    ],
                    tag: "SELECT 2",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM docs WHERE v @@ 'z39 & y39' ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("39")],
                        &[T("439")],
                        &[T("839")],
                        &[T("1239")],
                        &[T("1639")],
                    ],
                    tag: "SELECT 5",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE v @@ 'w1' AND v @@ '!x2';",
                expected: Expected::Rows {
                    columns: &[Column("QUERY PLAN", TEXT)],
                    rows: &[
                        &[T("Aggregate")],
                        &[T("  ->  Bitmap Heap Scan on docs")],
                        &[T("        Recheck Cond: ((v @@ '''w1'''::tsquery) AND (v @@ '!''x2'''::tsquery))")],
                        &[T("        ->  Bitmap Index Scan on docs_v")],
                        &[T("              Index Cond: ((v @@ '''w1'''::tsquery) AND (v @@ '!''x2'''::tsquery))")],
                    ],
                    tag: "EXPLAIN",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1' AND v @@ '!x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("278")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "INSERT INTO docs VALUES (3000, to_tsvector('simple', 'fresh w1 x2'));",
                expected: Expected::Tag("INSERT 0 1"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM docs WHERE v @@ 'fresh & w1' ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("3000")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "UPDATE docs SET v = to_tsvector('simple', 'fresher') WHERE id = 3000;",
                expected: Expected::Tag("UPDATE 1"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id FROM docs WHERE v @@ 'fresh:*' ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4)],
                    rows: &[
                        &[T("3000")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "DELETE FROM docs WHERE id = 3000;",
                expected: Expected::Tag("DELETE 1"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'fresh:*';",
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
                query: "CREATE INDEX docs_v2 ON docs USING gin (v tsvector_ops);",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT opcname FROM pg_opclass c JOIN pg_index i ON c.oid = i.indclass[0] WHERE i.indexrelid = 'docs_v2'::regclass;",
                expected: Expected::Rows {
                    columns: &[Column("opcname", NAME)],
                    rows: &[
                        &[T("tsvector_ops")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT pg_get_indexdef('docs_v2'::regclass);",
                expected: Expected::Rows {
                    columns: &[Column("pg_get_indexdef", TEXT)],
                    rows: &[
                        &[T("CREATE INDEX docs_v2 ON public.docs USING gin (v)")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_gist_indexes() {
    run_scripts(&[ScriptTest {
        name: "tsvector_ops and tsquery_ops GiST indexes, tsvector_ops' siglen option, and operator class options' errors",
        set_up_script: &[
            "CREATE TABLE docs (id int, v tsvector, q tsquery, r int4range);",
            "INSERT INTO docs SELECT i, to_tsvector('simple', 'w' || (i % 7) || ' x' || (i % 13) || ' y' || (i % 50)), ('w' || (i % 7) || ' & x' || (i % 3))::tsquery, int4range(i, i + 1) FROM generate_series(1, 1000) i;",
            "INSERT INTO docs SELECT i, setweight(to_tsvector('simple', 'w' || (i % 7) || ' heavy'), 'A'), ('heavy | y' || (i % 50))::tsquery, NULL FROM generate_series(1001, 1050) i;",
            "INSERT INTO docs VALUES (1051, NULL, NULL, NULL), (1052, '', '', NULL), (1053, (SELECT string_agg('long' || g, ' ') FROM generate_series(1, 200) AS g)::tsvector, 'long7 <-> long8', NULL);",
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "CREATE INDEX docs_v ON docs USING gist (v tsvector_ops (siglen = 16));",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT pg_get_indexdef('docs_v'::regclass);",
                expected: Expected::Rows {
                    columns: &[Column("pg_get_indexdef", TEXT)],
                    rows: &[
                        &[T("CREATE INDEX docs_v ON public.docs USING gist (v tsvector_ops (siglen='16'))")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT attname, attoptions FROM pg_attribute WHERE attrelid = 'docs_v'::regclass;",
                expected: Expected::Rows {
                    columns: &[Column("attname", NAME), Column("attoptions", TEXT_ARRAY)],
                    rows: &[
                        &[T("v"), T("{siglen=16}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SET enable_seqscan = off;",
                expected: Expected::Tag("SET"),
                ..A
            },
            ScriptTestAssertion {
                query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE v @@ 'w1 & x2';",
                expected: Expected::Rows {
                    columns: &[Column("QUERY PLAN", TEXT)],
                    rows: &[
                        &[T("Aggregate")],
                        &[T("  ->  Index Scan using docs_v on docs")],
                        &[T("        Index Cond: (v @@ '''w1'' & ''x2'''::tsquery)")],
                    ],
                    tag: "EXPLAIN",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("150")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 & x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("11")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 | x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("216")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ '!w1';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("902")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w:*';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("1050")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 <-> x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("11")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1:A';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("7")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'long7 <-> long8';",
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
                query: "SELECT count(*) FROM docs WHERE v @@ 'long150 & long3';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'no_such_lexeme';",
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
                query: "SELECT count(*) FROM docs WHERE v @@ '';",
                notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", position: 38, ..N }],
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
                query: "CREATE INDEX docs_v2 ON docs USING gist (v);",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT pg_get_indexdef('docs_v2'::regclass);",
                expected: Expected::Rows {
                    columns: &[Column("pg_get_indexdef", TEXT)],
                    rows: &[
                        &[T("CREATE INDEX docs_v2 ON public.docs USING gist (v)")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "DROP INDEX docs_v;",
                expected: Expected::Tag("DROP INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 & x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("11")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX docs_v3 ON docs USING gist (v tsvector_ops (siglen = 1));",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "DROP INDEX docs_v2;",
                expected: Expected::Tag("DROP INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'w1 & x2';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("11")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE v @@ 'heavy';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("50")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX docs_q ON docs USING gist (q);",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE q @> 'w1';",
                expected: Expected::Rows {
                    columns: &[Column("QUERY PLAN", TEXT)],
                    rows: &[
                        &[T("Aggregate")],
                        &[T("  ->  Bitmap Heap Scan on docs")],
                        &[T("        Recheck Cond: (q @> '''w1'''::tsquery)")],
                        &[T("        ->  Bitmap Index Scan on docs_q")],
                        &[T("              Index Cond: (q @> '''w1'''::tsquery)")],
                    ],
                    tag: "EXPLAIN",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE q @> 'w1';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("143")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE q <@ 'w1 & x2 & x1';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("97")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE q @> 'heavy';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("50")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT count(*) FROM docs WHERE q @> 'long7 <-> long8';",
                expected: Expected::Rows {
                    columns: &[Column("count", INT8)],
                    rows: &[
                        &[T("1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE TABLE docs_copy (LIKE docs INCLUDING INDEXES);",
                expected: Expected::Tag("CREATE TABLE"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT pg_get_indexdef(indexrelid) FROM pg_index WHERE indrelid = 'docs_copy'::regclass ORDER BY 1;",
                expected: Expected::Rows {
                    columns: &[Column("pg_get_indexdef", TEXT)],
                    rows: &[
                        &[T("CREATE INDEX docs_copy_q_idx ON public.docs_copy USING gist (q)")],
                        &[T("CREATE INDEX docs_copy_v_idx ON public.docs_copy USING gist (v tsvector_ops (siglen='1'))")],
                    ],
                    tag: "SELECT 2",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (foo = 1));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized parameter \"foo\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (siglen = 0));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "value 0 out of bounds for option \"siglen\"", detail: "Valid values are between \"1\" and \"2024\".", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (siglen = 2025));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "value 2025 out of bounds for option \"siglen\"", detail: "Valid values are between \"1\" and \"2024\".", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (siglen = 2024));",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "DROP INDEX bad;",
                expected: Expected::Tag("DROP INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (siglen = 10, siglen = 20));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "parameter \"siglen\" specified more than once", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (siglen));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for integer option \"siglen\": true", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (siglen = 'many'));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "invalid value for integer option \"siglen\": many", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (x.siglen = 10));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized parameter namespace \"x\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (r range_ops (siglen = 10));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "operator class range_ops has no options", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gin (v tsvector_ops (siglen = 10));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "operator class tsvector_ops has no options", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs (id int4_ops (siglen = 10));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "operator class int4_ops has no options", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (q tsquery_ops (siglen = 10));",
                expected: Expected::Error(Diagnostic { code: "22023", message: "operator class tsquery_ops has no options", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs (id) INCLUDE (v tsvector_ops (siglen = 10));",
                expected: Expected::Error(Diagnostic { code: "42P17", message: "including column does not support an operator class", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "CREATE INDEX bad ON docs USING gist (v tsvector_ops (SIGLEN = 10));",
                expected: Expected::Tag("CREATE INDEX"),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT 'x'::gtsvector;",
                expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot accept a value of type gtsvector", position: 8, ..E }),
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_ranking() {
    run_scripts(&[ScriptTest {
        name: "ts_rank and ts_rank_cd over AND, OR, phrase, NOT, prefix, and weighted queries, with weights, every normalization, and the weight arrays' errors",
        set_up_script: &[
            "CREATE TABLE docs (id int, v tsvector);",
            "INSERT INTO docs VALUES (1, to_tsvector('english', 'The quick brown fox jumps over the lazy dog')), (2, setweight(to_tsvector('english', 'fox hunting'), 'A') || to_tsvector('english', 'a dog and a fox, and another fox near the dog den')), (3, strip(to_tsvector('english', 'the fox and the dog'))), (4, to_tsvector('english', 'nothing to see here')), (5, ''::tsvector), (6, 'fox:1,3,5,7 dog:2,4,6,8 cat:100'::tsvector), (7, 'foxes:1A foxtrot:2B fox:3C dogged:4 dog:5D'::tsvector);",
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, to_tsquery('english', 'fox & dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.09148999")],
                        &[T("2"), T("0.56060374")],
                        &[T("3"), T("1e-16")],
                        &[T("4"), T("1e-20")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.8000629")],
                        &[T("7"), T("0.13930126")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, to_tsquery('english', 'fox | dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.06079271")],
                        &[T("2"), T("0.35293546")],
                        &[T("3"), T("0.06079271")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.08654518")],
                        &[T("7"), T("0.091189064")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, to_tsquery('english', 'fox <-> dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.09148999")],
                        &[T("2"), T("0.56060374")],
                        &[T("3"), T("1e-16")],
                        &[T("4"), T("1e-20")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.8000629")],
                        &[T("7"), T("0.13930126")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, to_tsquery('english', '!fox')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.06079271")],
                        &[T("2"), T("0.62988")],
                        &[T("3"), T("0.06079271")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.08654518")],
                        &[T("7"), T("0.12158542")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, to_tsquery('english', 'fox & !cat')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("1e-20")],
                        &[T("2"), T("1e-20")],
                        &[T("3"), T("1e-20")],
                        &[T("4"), T("1e-20")],
                        &[T("5"), T("0")],
                        &[T("6"), T("8.004708e-14")],
                        &[T("7"), T("1e-20")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, 'fox:*'::tsquery) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.06079271")],
                        &[T("2"), T("0.62988")],
                        &[T("3"), T("0.06079271")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.08654518")],
                        &[T("7"), T("0.97268337")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, 'fox:A & dog'::tsquery) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.09148999")],
                        &[T("2"), T("0.56060374")],
                        &[T("3"), T("1e-16")],
                        &[T("4"), T("1e-20")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.8000629")],
                        &[T("7"), T("0.13930126")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, 'fox & fox & dog'::tsquery) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.09148999")],
                        &[T("2"), T("0.56060374")],
                        &[T("3"), T("1e-16")],
                        &[T("4"), T("1e-20")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.8000629")],
                        &[T("7"), T("0.13930126")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, 'fox'::tsquery) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.06079271")],
                        &[T("2"), T("0.62988")],
                        &[T("3"), T("0.06079271")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.08654518")],
                        &[T("7"), T("0.12158542")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank(v, to_tsquery('english', 'the')) FROM docs ORDER BY id;",
                notices: &[Diagnostic { code: "00000", message: "text-search query contains only stop words or doesn't contain lexemes, ignored", ..N }],
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0")],
                        &[T("2"), T("0")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0")],
                        &[T("7"), T("0")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, n, ts_rank(v, to_tsquery('english', 'fox & dog'), n) FROM docs, (VALUES (0), (1), (2), (4), (8), (16), (32), (63)) AS m(n) WHERE id IN (1, 2, 6) ORDER BY id, n;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("n", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0"), T("0.09148999")],
                        &[T("1"), T("1"), T("0.032589395")],
                        &[T("1"), T("2"), T("0.015248332")],
                        &[T("1"), T("4"), T("0.09148999")],
                        &[T("1"), T("8"), T("0.015248332")],
                        &[T("1"), T("16"), T("0.032589395")],
                        &[T("1"), T("32"), T("0.08382119")],
                        &[T("1"), T("63"), T("0.0003223565")],
                        &[T("2"), T("0"), T("0.56060374")],
                        &[T("2"), T("1"), T("0.16875854")],
                        &[T("2"), T("2"), T("0.062289305")],
                        &[T("2"), T("4"), T("0.56060374")],
                        &[T("2"), T("8"), T("0.093433954")],
                        &[T("2"), T("16"), T("0.19969109")],
                        &[T("2"), T("32"), T("0.35922235")],
                        &[T("2"), T("63"), T("0.001111966")],
                        &[T("6"), T("0"), T("0.8000629")],
                        &[T("6"), T("1"), T("0.24084292")],
                        &[T("6"), T("2"), T("0.08889588")],
                        &[T("6"), T("4"), T("0.8000629")],
                        &[T("6"), T("8"), T("0.26668763")],
                        &[T("6"), T("16"), T("0.40003145")],
                        &[T("6"), T("32"), T("0.44446385")],
                        &[T("6"), T("63"), T("0.00444025")],
                    ],
                    tag: "SELECT 24",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank('{0.1, 0.2, 0.4, 1.0}', v, to_tsquery('english', 'fox & dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.09148999")],
                        &[T("2"), T("0.56060374")],
                        &[T("3"), T("1e-16")],
                        &[T("4"), T("1e-20")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.8000629")],
                        &[T("7"), T("0.13930126")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank('{1, 1, 1, 1}', v, to_tsquery('english', 'fox & dog'), 1) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.3258939")],
                        &[T("2"), T("0.30102992")],
                        &[T("3"), T("6.3092975e-16")],
                        &[T("4"), T("6.3092975e-21")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.30103")],
                        &[T("7"), T("0.38105333")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank('{-1, 0.5, -0.3, 0}', v, to_tsquery('english', 'fox | dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.06079271")],
                        &[T("2"), T("0.07176917")],
                        &[T("3"), T("0.06079271")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.08654518")],
                        &[T("7"), T("0.18237813")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank('{0.1, 0.2, 0.4, 1.0, 5}', v, 'fox'::tsquery) FROM docs WHERE id = 6;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("6"), T("0.08654518")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank('{{0.1, 0.2}, {0.4, 1.0}}', 'fox'::tsvector, 'fox'::tsquery);",
                expected: Expected::Error(Diagnostic { code: "2202E", message: "array of weight must be one-dimensional", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank('{0.1, 0.2, 0.4}', 'fox'::tsvector, 'fox'::tsquery);",
                expected: Expected::Error(Diagnostic { code: "2202E", message: "array of weight is too short", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank('{0.1, NULL, 0.4, 1}', 'fox'::tsvector, 'fox'::tsquery);",
                expected: Expected::Error(Diagnostic { code: "22004", message: "array of weight must not contain nulls", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank('{0.1, 0.2, 0.4, 1.5}', 'fox'::tsvector, 'fox'::tsquery);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "weight out of range", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank('{}', 'fox'::tsvector, 'fox'::tsquery);",
                expected: Expected::Error(Diagnostic { code: "2202E", message: "array of weight must be one-dimensional", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd(v, to_tsquery('english', 'fox & dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.02")],
                        &[T("2"), T("0.12727273")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.7")],
                        &[T("7"), T("0.06666667")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd(v, to_tsquery('english', 'fox | dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.2")],
                        &[T("2"), T("1.4")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.8")],
                        &[T("7"), T("0.3")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd(v, to_tsquery('english', 'fox <-> dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0")],
                        &[T("2"), T("0")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.4")],
                        &[T("7"), T("0")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd(v, to_tsquery('english', 'fox <2> dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0")],
                        &[T("2"), T("0")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0")],
                        &[T("7"), T("0.06666667")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd(v, to_tsquery('english', 'fox & !cat')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.1")],
                        &[T("2"), T("1.2")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.4")],
                        &[T("7"), T("0.2")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd(v, 'fox:* & dog:*'::tsquery) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.02")],
                        &[T("2"), T("0.12727273")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.7")],
                        &[T("7"), T("0.13333334")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd(v, 'fox:A | dog'::tsquery) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.1")],
                        &[T("2"), T("1.2")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.4")],
                        &[T("7"), T("0.1")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, n, ts_rank_cd(v, to_tsquery('english', 'fox & dog'), n) FROM docs, (VALUES (0), (1), (2), (4), (8), (16), (32), (63)) AS m(n) WHERE id IN (1, 2, 6) ORDER BY id, n;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("n", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0"), T("0.02")],
                        &[T("1"), T("1"), T("0.010277967")],
                        &[T("1"), T("2"), T("0.0033333334")],
                        &[T("1"), T("4"), T("0.02")],
                        &[T("1"), T("8"), T("0.0033333334")],
                        &[T("1"), T("16"), T("0.0071241437")],
                        &[T("1"), T("32"), T("0.019607844")],
                        &[T("1"), T("63"), T("0.000101686484")],
                        &[T("2"), T("0"), T("0.12727273")],
                        &[T("2"), T("1"), T("0.055273846")],
                        &[T("2"), T("2"), T("0.014141414")],
                        &[T("2"), T("4"), T("0.021212121")],
                        &[T("2"), T("8"), T("0.021212121")],
                        &[T("2"), T("16"), T("0.04533546")],
                        &[T("2"), T("32"), T("0.11290323")],
                        &[T("2"), T("63"), T("6.0764643e-05")],
                        &[T("6"), T("0"), T("0.7")],
                        &[T("6"), T("1"), T("0.30400613")],
                        &[T("6"), T("2"), T("0.07777778")],
                        &[T("6"), T("4"), T("0.6")],
                        &[T("6"), T("8"), T("0.23333333")],
                        &[T("6"), T("16"), T("0.35")],
                        &[T("6"), T("32"), T("0.4117647")],
                        &[T("6"), T("63"), T("0.0048023206")],
                    ],
                    tag: "SELECT 24",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd('{0.1, 0.2, 0.4, 1.0}', v, to_tsquery('english', 'fox & dog'), 4) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.02")],
                        &[T("2"), T("0.021212121")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("0.6")],
                        &[T("7"), T("0.06666667")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT id, ts_rank_cd('{1, 0, 0.5, 1}', v, to_tsquery('english', 'fox & dog')) FROM docs ORDER BY id;",
                expected: Expected::Rows {
                    columns: &[Column("id", INT4), Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("1"), T("0.2")],
                        &[T("2"), T("1")],
                        &[T("3"), T("0")],
                        &[T("4"), T("0")],
                        &[T("5"), T("0")],
                        &[T("6"), T("7")],
                        &[T("7"), T("0")],
                    ],
                    tag: "SELECT 7",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd('{0.1, 0.2}', 'fox:1'::tsvector, 'fox'::tsquery);",
                expected: Expected::Error(Diagnostic { code: "2202E", message: "array of weight is too short", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd('{0.1, 0.2, 0.4, 2}', 'fox:1'::tsvector, 'fox'::tsquery);",
                expected: Expected::Error(Diagnostic { code: "22023", message: "weight out of range", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd('a:1 b:2 c:3 a:4 b:5'::tsvector, 'a & b & c'::tsquery);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("0.3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd('a:1,4 b:2,5 c:3'::tsvector, 'a & b & c'::tsquery);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("0.3")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd('a:1,4 b:2,5 c:3'::tsvector, '(a | c) & b'::tsquery, 4);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("0.15")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank('a:1,4 b:2,5 c:3'::tsvector, '(a | c) & b'::tsquery);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("0.56335545")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd('a:1,4 b:2,5 c:3'::tsvector, 'a <-> b <-> c'::tsquery);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("0.1")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd('a:1,4 b:2,5 c:3'::tsvector, 'a <-> (b | c)'::tsquery);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("0.2")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank_cd(to_tsvector('english', repeat('fox dog ', 50)), to_tsquery('english', 'fox & dog'), 4);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank_cd", FLOAT4)],
                    rows: &[
                        &[T("9.8")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_rank(to_tsvector('english', repeat('fox dog ', 50)), to_tsquery('english', 'fox & dog'), 5);",
                expected: Expected::Rows {
                    columns: &[Column("ts_rank", FLOAT4)],
                    rows: &[
                        &[T("0.15019044")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_headlines() {
    run_scripts(&[ScriptTest {
        name: "ts_headline of text, json, and jsonb over AND, OR, phrase, NOT, prefix, and empty queries, with fragments, every option, tags and URLs, and the options' errors",
        set_up_script: &[
            "CREATE TABLE doc (t text);",
            "INSERT INTO doc VALUES ('Lorem ipsum urna.  Nullam nullam ullamcorper urna. Sed sit amet, consectetuer adipiscing elit. Curabitur ipsum dolor, ullamcorper ac urna nec, tincidunt molestie nunc. Etiam quis urna rhoncus, auctor eros quis, porta nunc. Sed vestibulum urna, at hendrerit tellus. Mauris ut metus ac dolor tincidunt aliquet.');",
        ],
        assertions: &[
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'fox & dog'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown <b>fox</b> jumps over the lazy <b>dog</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'fox | cat'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown <b>fox</b> jumps over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'quick <-> brown'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The <b>quick</b> <b>brown</b> fox jumps over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'quick <-> fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The <b>quick</b> brown <b>fox</b> jumps over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'jump:*'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown fox <b>jumps</b> over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'fox & !cat'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown <b>fox</b> jumps over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'cat'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown fox jumps over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', to_tsquery('english', 'the'));",
                notices: &[Diagnostic { code: "00000", message: "text-search query contains only stop words or doesn't contain lexemes, ignored", ..N }],
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown fox jumps over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The quick brown fox jumps over the lazy dog', ''::tsquery);",
                notices: &[Diagnostic { code: "00000", message: "text-search query doesn't contain lexemes: \"\"", position: 78, ..N }],
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown fox jumps over the lazy dog")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'ullamcorper & urna')) FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<b>urna</b>.  Nullam nullam <b>ullamcorper</b> <b>urna</b>. Sed sit amet, consectetuer adipiscing elit. Curabitur ipsum dolor, <b>ullamcorper</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'ullamcorper & urna'), 'MaxWords=10, MinWords=5') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<b>urna</b>.  Nullam nullam <b>ullamcorper</b> <b>urna</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'ullamcorper & urna'), 'MaxFragments=2, MaxWords=8, MinWords=3') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<b>urna</b>.  Nullam nullam <b>ullamcorper</b> <b>urna</b>. Sed sit amet ... ipsum dolor, <b>ullamcorper</b> ac <b>urna</b> nec, tincidunt molestie")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'urna'), 'MaxFragments=3, MaxWords=5, MinWords=2, FragmentDelimiter=\" | \"') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("Lorem ipsum <b>urna</b>.  Nullam nullam | ullamcorper <b>urna</b>. Sed sit amet | ullamcorper ac <b>urna</b> nec, tincidunt")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'nunc & tincidunt'), 'MaxFragments=1, MaxWords=12, MinWords=4, ShortWord=2') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("ullamcorper ac urna nec, <b>tincidunt</b> molestie <b>nunc</b>. Etiam quis urna rhoncus, auctor")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'missing'), 'MaxFragments=2, MinWords=4, MaxWords=8') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("Lorem ipsum urna.  Nullam")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'curabitur <-> ipsum'), 'StartSel=<<, StopSel=>>') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<<Curabitur>> <<ipsum>> dolor, ullamcorper ac urna nec, tincidunt molestie nunc. Etiam quis urna rhoncus, auctor")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'sed & (amet | vestibulum)'), 'HighlightAll=true') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("Lorem ipsum urna.  Nullam nullam ullamcorper urna. <b>Sed</b> sit <b>amet</b>, consectetuer adipiscing elit. Curabitur ipsum dolor, ullamcorper ac urna nec, tincidunt molestie nunc. Etiam quis urna rhoncus, auctor eros quis, porta nunc. <b>Sed</b> <b>vestibulum</b> urna, at hendrerit tellus. Mauris ut metus ac dolor tincidunt aliquet.")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', t, to_tsquery('english', 'etiam | mauris'), 'MaxWords=6, MinWords=2, ShortWord=0') FROM doc;",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<b>Etiam</b> quis")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '<html><body><b>bold fox</b> and a <a href=\"http://example.com/fox\">link</a> to foxes, see http://example.com/fox.html</body></html>', to_tsquery('english', 'fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("   bold <b>fox</b>  and a  link  to <b>foxes</b>, see http://example.com/fox.html  ")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '<html><body><b>bold fox</b> and a <a href=\"http://example.com/fox\">link</a> to foxes</body></html>', to_tsquery('english', 'fox'), 'HighlightAll=yes');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<html><body><b>bold <b>fox</b></b> and a <a href=\"http://example.com/fox\">link</a> to <b>foxes</b></body></html>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'state-of-the-art foxes and well-known dogs', to_tsquery('english', 'art & dog'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("state-of-the-<b>art</b> foxes and well-known <b>dogs</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('simple', 'one two three four five six seven eight nine ten eleven twelve', 'six & eleven'::tsquery, 'MaxWords=4, MinWords=2');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<b>six</b> seven eight nine")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('simple', 'one two three four five six seven eight nine ten eleven twelve', 'six & eleven'::tsquery, 'MaxWords=4, MinWords=2, MaxFragments=2');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("five <b>six</b> seven eight ... <b>eleven</b> twelve")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('simple', 'a b c a b c a b c', 'a <-> b'::tsquery, 'MaxFragments=5, MaxWords=3, MinWords=1');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<b>a</b> <b>b</b> ... <b>a</b> <b>b</b> ... <b>a</b> <b>b</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('simple', 'x y z', 'q'::tsquery, 'MaxFragments=1, MinWords=1, MaxWords=2');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("x")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'Foo=1');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized headline parameter: \"Foo\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'MinWords=10, MaxWords=5');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "MinWords must be less than MaxWords", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'MinWords=0');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "MinWords must be positive", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'ShortWord=-1');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "ShortWord must be >= 0", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'MaxFragments=-1');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "MaxFragments must be >= 0", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'MaxWords=abc');",
                expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"abc\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'MinWords=10, MaxWords=5, HighlightAll=on');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The <b>fox</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'The fox', 'fox'::tsquery, 'maxwords=4, MINWORDS=2, startsel=\"[\", stopsel=\"]\"');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The [fox]")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('The quick brown fox', to_tsquery('fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown <b>fox</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('The quick brown fox', to_tsquery('fox'), 'StartSel=*, StopSel=*');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("The quick brown *fox*")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '{\"a\": \"the quick fox\", \"b\": [\"lazy fox\", 1, true, null], \"fox\": {\"c\": \"no match here\"}}'::jsonb, to_tsquery('english', 'fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSONB)],
                    rows: &[
                        &[T("{\"a\": \"the quick <b>fox</b>\", \"b\": [\"lazy <b>fox</b>\", 1, true, null], \"fox\": {\"c\": \"no match here\"}}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '{\"a\": \"the quick fox\", \"b\": [\"lazy fox\", 1, true, null], \"fox\": {\"c\": \"no match here\"}}'::json, to_tsquery('english', 'fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSON)],
                    rows: &[
                        &[T("{\"a\":\"the quick <b>fox</b>\",\"b\":[\"lazy <b>fox</b>\",1,true,null],\"fox\":{\"c\":\"no match here\"}}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '{\"a\":  \"the quick fox\" , \"b\" : [ \"lazy fox\" , 1.50 ]}'::json, to_tsquery('english', 'fox'), 'StartSel=<, StopSel=>');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSON)],
                    rows: &[
                        &[T("{\"a\":\"the quick <fox>\",\"b\":[\"lazy <fox>\",1.50]}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '\"just a fox\"'::jsonb, to_tsquery('english', 'fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSONB)],
                    rows: &[
                        &[T("\"just a <b>fox</b>\"")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '\"just a fox\"'::json, to_tsquery('english', 'fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSON)],
                    rows: &[
                        &[T("\"just a <b>fox</b>\"")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '[]'::jsonb, to_tsquery('english', 'fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSONB)],
                    rows: &[
                        &[T("[]")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('{\"a\": \"fox den\"}'::jsonb, to_tsquery('fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSONB)],
                    rows: &[
                        &[T("{\"a\": \"<b>fox</b> den\"}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('{\"a\": \"fox den\"}'::jsonb, to_tsquery('fox'), 'StartSel=[, StopSel=]');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSONB)],
                    rows: &[
                        &[T("{\"a\": \"[fox] den\"}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('{\"a\": \"fox den\"}'::json, to_tsquery('fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSON)],
                    rows: &[
                        &[T("{\"a\":\"<b>fox</b> den\"}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('{\"a\": \"fox den\"}'::json, to_tsquery('fox'), 'StartSel=[, StopSel=]');",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSON)],
                    rows: &[
                        &[T("{\"a\":\"[fox] den\"}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '{\"a\": \"fox \\\"quoted\\\" den\\n\"}'::json, to_tsquery('english', 'fox'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", JSON)],
                    rows: &[
                        &[T("{\"a\":\"<b>fox</b> \\\"quoted\\\" den\\n\"}")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', '{\"a\": \"fox\"}'::jsonb, to_tsquery('english', 'fox'), 'Bad=1');",
                expected: Expected::Error(Diagnostic { code: "22023", message: "unrecognized headline parameter: \"Bad\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', 'foxes run, foxes hide', to_tsquery('english', 'fox & fox:*'));",
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T("<b>foxes</b> run, <b>foxes</b> hide")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_text_search_headline_error_order() {
    run_scripts(&[ScriptTest {
        name: "ts_headline reads its options after parsing a text, so a too-long word's NOTICE comes before a malformed option list's error, and before parsing json or jsonb",
        assertions: &[
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', repeat('x', 3000) || ' fox', 'fox'::tsquery, 'MaxWords=\"5');",
                notices: &[Diagnostic { code: "54000", message: "word is too long to be indexed", detail: "Words longer than 2047 characters are ignored.", ..N }],
                expected: Expected::Error(Diagnostic { code: "42601", message: "invalid parameter list format: \"MaxWords=\"5\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', repeat('x', 3000) || ' fox', 'fox'::tsquery, 'MaxWords=5, MinWords=2');",
                notices: &[Diagnostic { code: "54000", message: "word is too long to be indexed", detail: "Words longer than 2047 characters are ignored.", ..N }],
                expected: Expected::Rows {
                    columns: &[Column("ts_headline", TEXT)],
                    rows: &[
                        &[T(" <b>fox</b>")],
                    ],
                    tag: "SELECT 1",
                },
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', json_build_object('a', repeat('x', 3000) || ' fox'), 'fox'::tsquery, 'MaxWords=\"5');",
                expected: Expected::Error(Diagnostic { code: "42601", message: "invalid parameter list format: \"MaxWords=\"5\"", ..E }),
                ..A
            },
            ScriptTestAssertion {
                query: "SELECT ts_headline('english', jsonb_build_object('a', repeat('x', 3000) || ' fox'), 'fox'::tsquery, 'MaxWords=\"5');",
                expected: Expected::Error(Diagnostic { code: "42601", message: "invalid parameter list format: \"MaxWords=\"5\"", ..E }),
                ..A
            },
        ],
        ..S
    }]);
}

#[test]
fn test_ts_stat_reads_a_query() {
    run_scripts(&[
        ScriptTest {
            name: "ts_stat over a query's tsvectors, last lexeme first, with weights, more rows than one batch, NULLs, and the wrong columns",
            set_up_script: &[
                "CREATE TABLE docs (d TSVECTOR)",
                "INSERT INTO docs VALUES ('a:1A,2B b c:3 ab z'), ('a b:1C'), (NULL), ('')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM ts_stat('SELECT d FROM docs')",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4), Column("nentry", INT4)],
                        rows: &[
                            &[T("z"), T("1"), T("1")],
                            &[T("c"), T("1"), T("1")],
                            &[T("b"), T("2"), T("2")],
                            &[T("ab"), T("1"), T("1")],
                            &[T("a"), T("2"), T("3")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ts_stat('SELECT d FROM docs', 'ab')",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4), Column("nentry", INT4)],
                        rows: &[
                            &[T("a"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ts_stat('SELECT d FROM docs', 'xD')",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4), Column("nentry", INT4)],
                        rows: &[
                            &[T("c"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(ndoc), sum(nentry) FROM ts_stat('SELECT (''w'' || g % 7 || '' x'' || g)::tsvector FROM generate_series(1, 250) g')",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("257"), T("500"), T("500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT word, ndoc, nentry FROM ts_stat('SELECT (''w'' || g % 7)::tsvector FROM generate_series(1, 250) g') ORDER BY word",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4), Column("nentry", INT4)],
                        rows: &[
                            &[T("w0"), T("35"), T("35")],
                            &[T("w1"), T("36"), T("36")],
                            &[T("w2"), T("36"), T("36")],
                            &[T("w3"), T("36"), T("36")],
                            &[T("w4"), T("36"), T("36")],
                            &[T("w5"), T("36"), T("36")],
                            &[T("w6"), T("35"), T("35")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ts_stat('SELECT 1')",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "ts_stat query must return one tsvector column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ts_stat('SELECT d, d FROM docs')",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "ts_stat query must return one tsvector column", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ts_stat('SELECT d FROM docs WHERE false')",
                    expected: Expected::Rows {
                        columns: &[Column("word", TEXT), Column("ndoc", INT4), Column("nentry", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_ts_rewrite_reads_a_query() {
    run_scripts(&[
        ScriptTest {
            name: "ts_rewrite with the substitutions a query returns, row by row, skipping NULLs, and the wrong columns",
            set_up_script: &[
                "CREATE TABLE subs (q TSQUERY, s TSQUERY)",
                "INSERT INTO subs VALUES ('a', 'x|y'), (NULL, 'b'), ('b', NULL), ('c & d', 'e')",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a & b & (c & d)'::tsquery, 'SELECT q, s FROM subs')",
                    expected: Expected::Rows {
                        columns: &[Column("ts_rewrite", TSQUERY)],
                        rows: &[
                            &[T("'b' & 'e' & ( 'y' | 'x' )")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a'::tsquery, 'SELECT q FROM subs')",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "ts_rewrite query must return two tsquery columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a'::tsquery, 'SELECT q::text, s FROM subs')",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "ts_rewrite query must return two tsquery columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_rewrite('a'::tsquery, 'SELECT q, s FROM subs WHERE false')",
                    expected: Expected::Rows {
                        columns: &[Column("ts_rewrite", TSQUERY)],
                        rows: &[
                            &[T("'a'")],
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
fn test_tsvector_update_triggers() {
    run_scripts(&[
        ScriptTest {
            name: "tsvector_update_trigger and tsvector_update_trigger_column filling a tsvector column on INSERT and on an UPDATE of its text columns, with their errors",
            set_up_script: &[
                "CREATE TABLE tt (id INT, a TSVECTOR, t TEXT, u VARCHAR, cfg REGCONFIG, n INT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tt_byid BEFORE INSERT OR UPDATE ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.english', t, u)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id, t, u) VALUES (1, 'The quick foxes', 'jumped over'), (2, NULL, 'lazy dogs')",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a FROM tt ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TSVECTOR)],
                        rows: &[
                            &[T("1"), T("'fox':3 'jump':4 'quick':2")],
                            &[T("2"), T("'dog':2 'lazi':1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tt SET n = 5 WHERE id = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tt SET a = 'manual' WHERE id = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a FROM tt ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TSVECTOR)],
                        rows: &[
                            &[T("1"), T("'fox':3 'jump':4 'quick':2")],
                            &[T("2"), T("'manual'")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE tt SET u = NULL WHERE id = 2",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a FROM tt ORDER BY id",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TSVECTOR)],
                        rows: &[
                            &[T("1"), T("'fox':3 'jump':4 'quick':2")],
                            &[T("2"), T("")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tt_byid ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER tt_bycol BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger_column(a, cfg, t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id, t, cfg) VALUES (3, 'running quickly', 'simple')",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a FROM tt WHERE id = 3",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", TSVECTOR)],
                        rows: &[
                            &[T("3"), T("'quickly':2 'running':1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id, t) VALUES (4, 'x')",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "configuration column \"cfg\" must not be null", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER tt_bycol ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e1 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, english, t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "text search configuration name \"english\" must be schema-qualified", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e1 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e2 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(nope, 'pg_catalog.english', t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "tsvector column \"nope\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e2 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e3 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(t, 'pg_catalog.english', t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"t\" is not of tsvector type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e3 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e4 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.english', n)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"n\" is not of a character type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e4 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e5 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.english', nope)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"nope\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e5 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e6 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(ctid, 'pg_catalog.english', t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"ctid\" is not of tsvector type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e6 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e7 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger_column(a, nope, t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "configuration column \"nope\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e7 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e8 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger_column(a, t, t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"t\" is not of regconfig type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e8 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e9 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.english')",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tsvector_update_trigger: arguments must be tsvector_field, ts_config, text_field1, ...)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e9 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e10 AFTER INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.english', t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tsvector_update_trigger: must be fired BEFORE event", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e10 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e11 BEFORE INSERT ON tt FOR EACH STATEMENT EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.english', t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tsvector_update_trigger: must be fired for row", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e11 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e12 BEFORE DELETE ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.english', t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM tt WHERE id = 1",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tsvector_update_trigger: must be fired for INSERT or UPDATE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e12 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER e13 BEFORE INSERT ON tt FOR EACH ROW EXECUTE FUNCTION tsvector_update_trigger(a, 'pg_catalog.nope', t)",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tt (id) VALUES (5)",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "text search configuration \"pg_catalog.nope\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER e13 ON tt",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tsvector_update_trigger()",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "tsvector_update_trigger: not fired by trigger manager", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_built_in_text_search_objects() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER TEXT SEARCH DICTIONARY changes a built-in dictionary's options",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH DICTIONARY simple (Accept = false);",
                    expected: Expected::Tag("ALTER TEXT SEARCH DICTIONARY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dictinitoption FROM pg_ts_dict WHERE dictname = 'simple';",
                    expected: Expected::Rows {
                        columns: &[Column("dictinitoption", TEXT)],
                        rows: &[
                            &[T("accept = 'false'")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ts_lexize('simple', 'The'), ts_lexize('simple', 'Cats');",
                    expected: Expected::Rows {
                        columns: &[Column("ts_lexize", TEXT_ARRAY), Column("ts_lexize", TEXT_ARRAY)],
                        rows: &[
                            &[Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TEXT SEARCH CONFIGURATION english DROP MAPPING FOR word;",
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_mappings_of_built_in_configurations() {
    run_scripts(&[
        ScriptTest {
            name: "a superuser drops, adds, alters, and replaces the mappings of built-in configurations",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TEXT SEARCH CONFIGURATION english DROP MAPPING FOR asciiword;"#,
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT to_tsvector('english', 'The cats sat');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_tsvector", TSVECTOR)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TEXT SEARCH CONFIGURATION english DROP MAPPING FOR asciiword;"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"mapping for token type "asciiword" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TEXT SEARCH CONFIGURATION english DROP MAPPING IF EXISTS FOR asciiword;"#,
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    notices: &[Diagnostic { code: "00000", message: r#"mapping for token type "asciiword" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TEXT SEARCH CONFIGURATION english ADD MAPPING FOR asciiword WITH simple;"#,
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT to_tsvector('english', 'The cats sat');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_tsvector", TSVECTOR)],
                        rows: &[
                            &[T("'cats':2 'sat':3 'the':1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TEXT SEARCH CONFIGURATION simple ALTER MAPPING FOR word WITH english_stem, simple;"#,
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT m.maptokentype, m.mapseqno, d.dictname FROM pg_ts_config_map m JOIN pg_ts_dict d ON d.oid = m.mapdict WHERE m.mapcfg = 'simple'::regconfig AND m.maptokentype <= 3 ORDER BY 1, 2;"#,
                    expected: Expected::Rows {
                        columns: &[Column("maptokentype", INT4), Column("mapseqno", INT4), Column("dictname", NAME)],
                        rows: &[
                            &[T("1"), T("1"), T("simple")],
                            &[T("2"), T("1"), T("english_stem")],
                            &[T("2"), T("2"), T("simple")],
                            &[T("3"), T("1"), T("simple")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TEXT SEARCH CONFIGURATION french ALTER MAPPING REPLACE french_stem WITH simple;"#,
                    expected: Expected::Tag("ALTER TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT to_tsvector('french', 'chats mangés');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_tsvector", TSVECTOR)],
                        rows: &[
                            &[T("'chats':1 'mangés':2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TEXT SEARCH DICTIONARY english_stem CASCADE;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop text search configuration simple because it is required by the database system"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TEXT SEARCH CONFIGURATION german;"#,
                    expected: Expected::Tag("DROP TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_ts_config_map m LEFT JOIN pg_ts_config c ON c.oid = m.mapcfg WHERE c.oid IS NULL;"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
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
