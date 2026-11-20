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

//! The soft input errors of PG18's regress modules: `pg_input_is_valid` and `pg_input_error_info` over each type's
//! input function.

use harness::oid::*;
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};


#[test]
fn test_regress_input_errors() {
    run_scripts(&[
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's arrays regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('{1,2,3}', 'integer[]');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('{1,2', 'integer[]');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('{1,zed}', 'integer[]');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('{1,zed}', 'integer[]');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type integer: \"zed\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's bit regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('01010001', 'bit(10)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('01010001', 'bit(10)');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("bit string length 8 does not match type bit(10)"), Null, Null, T("22026")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('01010Z01', 'bit(8)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('01010Z01', 'bit(8)');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"Z\" is not a valid binary digit"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('x01010Z01', 'bit(32)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('x01010Z01', 'bit(32)');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"Z\" is not a valid hexadecimal digit"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('01010Z01', 'varbit');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('01010Z01', 'varbit');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"Z\" is not a valid binary digit"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('x01010Z01', 'varbit');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('x01010Z01', 'varbit');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"Z\" is not a valid hexadecimal digit"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's boolean regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('true', 'bool');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('asdf', 'bool');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('junk', 'bool');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type boolean: \"junk\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's box regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('200', 'box');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('200', 'box');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type box: \"200\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('((200,300),(500, xyz))', 'box');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('((200,300),(500, xyz))', 'box');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type box: \"((200,300),(500, xyz))\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's char regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('abcd  ', 'char(4)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('abcde', 'char(4)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('abcde', 'char(4)');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value too long for type character(4)"), Null, Null, T("22001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's date regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('now', 'date');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('garbage', 'date');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('6874898-01-01', 'date');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('garbage', 'date');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type date: \"garbage\""), Null, Null, T("22007")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('6874898-01-01', 'date');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("date out of range: \"6874898-01-01\""), Null, Null, T("22008")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's domain regress module",
            set_up_script: &[
                "CREATE DOMAIN positiveint int4 CHECK (value > 0);",
                "CREATE DOMAIN weirdfloat float8 CHECK ((1 / value) < 10);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('1', 'positiveint');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('junk', 'positiveint');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('-1', 'positiveint');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('junk', 'positiveint');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type integer: \"junk\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('-1', 'positiveint');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value for domain positiveint violates check constraint \"positiveint_check\""), Null, Null, T("23514")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('junk', 'weirdfloat');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type double precision: \"junk\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('0.01', 'weirdfloat');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value for domain weirdfloat violates check constraint \"weirdfloat_check\""), Null, Null, T("23514")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('0', 'weirdfloat');",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's enum regress module",
            set_up_script: &[
                "CREATE TYPE rainbow AS ENUM ('red', 'orange', 'yellow', 'green', 'blue', 'purple');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('red', 'rainbow');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('mauve', 'rainbow');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('mauve', 'rainbow');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input value for enum rainbow: \"mauve\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info(repeat('too_long', 32), 'rainbow');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input value for enum rainbow: \"too_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_longtoo_long\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's float4 regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('34.5', 'float4');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('xyz', 'float4');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1e400', 'float4');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1e400', 'float4');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"1e400\" is out of range for type real"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's float8 regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('34.5', 'float8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('xyz', 'float8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1e4000', 'float8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1e4000', 'float8');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"1e4000\" is out of range for type double precision"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's geometry regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(1', 'circle');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1,', 'circle');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type circle: \"1,\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(1,2),-1', 'circle');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(1,2),-1', 'circle');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type circle: \"(1,2),-1\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's inet regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1234', 'cidr');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1234', 'cidr');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type cidr: \"1234\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('192.168.198.200/24', 'cidr');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('192.168.198.200/24', 'cidr');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid cidr value: \"192.168.198.200/24\""), T("Value has bits set to right of mask."), Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1234', 'inet');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1234', 'inet');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type inet: \"1234\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's int2 regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('34', 'int2');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('asdf', 'int2');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('50000', 'int2');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('50000', 'int2');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"50000\" is out of range for type smallint"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid(' 1 3  5 ', 'int2vector');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1 asdf', 'int2vector');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type smallint: \"asdf\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('50000', 'int2vector');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"50000\" is out of range for type smallint"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's int4 regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('34', 'int4');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('asdf', 'int4');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1000000000000', 'int4');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1000000000000', 'int4');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"1000000000000\" is out of range for type integer"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's int8 regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('34', 'int8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('asdf', 'int8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('10000000000000000000', 'int8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('10000000000000000000', 'int8');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"10000000000000000000\" is out of range for type bigint"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's interval regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1.5 weeks', 'interval');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('garbage', 'interval');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('@ 30 eons ago', 'interval');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('garbage', 'interval');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type interval: \"garbage\""), Null, Null, T("22007")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('@ 30 eons ago', 'interval');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type interval: \"@ 30 eons ago\""), Null, Null, T("22007")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's json regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('{\"a\":true}', 'json');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('{\"a\":true', 'json');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('{\"a\":true', 'json');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type json"), T("The input string ended unexpectedly."), Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's json_encoding regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('{ \"a\":  \"\\ud83d\\ude04\\ud83d\\udc36\" }', 'jsonb');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's jsonb regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('{\"a\":true}', 'jsonb');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('{\"a\":true', 'jsonb');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('{\"a\":true', 'jsonb');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type json"), T("The input string ended unexpectedly."), Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('{\"a\":1e1000000}', 'jsonb');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value overflows numeric format"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's line regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('{1, 1}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('{1, 1}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type line: \"{1, 1}\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('{0, 0, 0}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('{0, 0, 0}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid line specification: A and B cannot both be zero"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('{1, 1, a}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('{1, 1, a}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type line: \"{1, 1, a}\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('{1, 1, 1e400}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('{1, 1, 1e400}', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"1e400\" is out of range for type double precision"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(1, 1), (1, 1e400)', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(1, 1), (1, 1e400)', 'line');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"1e400\" is out of range for type double precision"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's lseg regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('[(1,2),(3)]', 'lseg');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('[(1,2),(3)]', 'lseg');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type lseg: \"[(1,2),(3)]\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's macaddr regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('08:00:2b:01:02:ZZ', 'macaddr');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('08:00:2b:01:02:ZZ', 'macaddr');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type macaddr: \"08:00:2b:01:02:ZZ\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('08:00:2b:01:02:', 'macaddr');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('08:00:2b:01:02:', 'macaddr');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type macaddr: \"08:00:2b:01:02:\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's macaddr8 regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('08:00:2b:01:02:03:04:ZZ', 'macaddr8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('08:00:2b:01:02:03:04:ZZ', 'macaddr8');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type macaddr8: \"08:00:2b:01:02:03:04:ZZ\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('08:00:2b:01:02:03:04:', 'macaddr8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('08:00:2b:01:02:03:04:', 'macaddr8');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type macaddr8: \"08:00:2b:01:02:03:04:\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's money regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('\\x0001', 'money');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('\\x0001', 'money');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type money: \"\\x0001\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('192233720368547758.07', 'money');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('192233720368547758.07', 'money');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"192233720368547758.07\" is out of range for type money"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's multirangetypes regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('{[1,2], [4,5]}', 'int4multirange');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('{[1,2], [4,5]', 'int4multirange');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('{[1,2], [4,5]', 'int4multirange');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("malformed multirange literal: \"{[1,2], [4,5]\""), T("Unexpected end of input."), Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('{[1,2], [4,zed]}', 'int4multirange');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('{[1,2], [4,zed]}', 'int4multirange');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type integer: \"zed\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's numeric regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('34.5', 'numeric');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('34xyz', 'numeric');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1e400000', 'numeric');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1e400000', 'numeric');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value overflows numeric format"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1234.567', 'numeric(8,4)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1234.567', 'numeric(7,4)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1234.567', 'numeric(7,4)');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("numeric field overflow"), T("A field with precision 7, scale 4 must round to an absolute value less than 10^3."), Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('0x1234.567', 'numeric');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type numeric: \"0x1234.567\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's oid regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1234', 'oid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('01XYZ', 'oid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('01XYZ', 'oid');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type oid: \"01XYZ\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('9999999999', 'oid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('9999999999', 'oid');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"9999999999\" is out of range for type oid"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid(' 1 2  4 ', 'oidvector');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('01 01XYZ', 'oidvector');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('01 01XYZ', 'oidvector');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type oid: \"XYZ\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('01 9999999999', 'oidvector');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('01 9999999999', 'oidvector');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"9999999999\" is out of range for type oid"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's path regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('[(1,2),(3)]', 'path');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('[(1,2),(3)]', 'path');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type path: \"[(1,2),(3)]\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('[(1,2,6),(3,4,6)]', 'path');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('[(1,2,6),(3,4,6)]', 'path');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type path: \"[(1,2,6),(3,4,6)]\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's pg_lsn regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('16AE7F7', 'pg_lsn');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('16AE7F7', 'pg_lsn');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type pg_lsn: \"16AE7F7\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's point regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('1,y', 'point');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('1,y', 'point');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type point: \"1,y\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's polygon regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(2.0,0.8,0.1)', 'polygon');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(2.0,0.8,0.1)', 'polygon');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type polygon: \"(2.0,0.8,0.1)\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(2.0,xyz)', 'polygon');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(2.0,xyz)', 'polygon');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type polygon: \"(2.0,xyz)\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's privileges regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('regress_priv_user1=r/regress_priv_user2', 'aclitem');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('regress_priv_user1=r/', 'aclitem');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('regress_priv_user1=r/', 'aclitem');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("role \"regress_priv_user1\" does not exist"), Null, Null, T("42704")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('regress_priv_user1=r/regress_no_such_user', 'aclitem');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('regress_priv_user1=r/regress_no_such_user', 'aclitem');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("role \"regress_priv_user1\" does not exist"), Null, Null, T("42704")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('regress_priv_user1=rY', 'aclitem');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('regress_priv_user1=rY', 'aclitem');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid mode character: must be one of \"arwdDxtXUCTcsAm\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's rangetypes regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('(1,4)', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('(1,4', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('(1,4', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("malformed range literal: \"(1,4\""), T("Unexpected end of input."), Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('(4,1)', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('(4,1)', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("range lower bound must be less than or equal to range upper bound"), Null, Null, T("22000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('(4,zed)', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('(4,zed)', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type integer: \"zed\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('[1,2147483647]', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('[1,2147483647]', 'int4range');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("integer out of range"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('[2000-01-01,5874897-12-31]', 'daterange');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('[2000-01-01,5874897-12-31]', 'daterange');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("date out of range"), Null, Null, T("22008")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's regproc regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('ng_catalog.pg_class', 'regclass');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("relation \"ng_catalog.pg_class\" does not exist"), Null, Null, T("42P01")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('ng_catalog.\"POSIX\"', 'regcollation');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('no_such_config', 'regconfig');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("text search configuration \"no_such_config\" does not exist"), Null, Null, T("42704")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('no_such_dictionary', 'regdictionary');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("text search dictionary \"no_such_dictionary\" does not exist"), Null, Null, T("42704")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('Nonexistent', 'regnamespace');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("schema \"nonexistent\" does not exist"), Null, Null, T("3F000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('ng_catalog.||/', 'regoper');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("operator does not exist: ng_catalog.||/"), Null, Null, T("42883")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('-', 'regoper');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("more than one operator named -"), Null, Null, T("42725")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('ng_catalog.+(int4,int4)', 'regoperator');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("operator does not exist: ng_catalog.+(int4,int4)"), Null, Null, T("42883")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('-', 'regoperator');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("expected a left parenthesis"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('ng_catalog.now', 'regproc');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("function \"ng_catalog.now\" does not exist"), Null, Null, T("42883")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('ng_catalog.abs(numeric)', 'regprocedure');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("function \"ng_catalog.abs(numeric)\" does not exist"), Null, Null, T("42883")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('ng_catalog.abs(numeric', 'regprocedure');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("expected a right parenthesis"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('regress_regrole_test', 'regrole');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("role \"regress_regrole_test\" does not exist"), Null, Null, T("42704")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('no_such_type', 'regtype');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("type \"no_such_type\" does not exist"), Null, Null, T("42704")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('incorrect type name syntax', 'regtype');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"type\"", position: 11, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('way.too.many.names', 'regtype');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "improper qualified name (too many dotted names): way.too.many.names", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('no_such_catalog.schema.name', 'regtype');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cross-database references are not implemented: no_such_catalog.schema.name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's rowtypes regress module",
            set_up_script: &[
                "CREATE TYPE complex AS (r float8, i float8);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(1,2)', 'complex');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(1,2', 'complex');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(1,zed)', 'complex');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(1,zed)', 'complex');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type double precision: \"zed\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(1,1e400)', 'complex');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("\"1e400\" is out of range for type double precision"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's strings regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid(E'\\\\xDeAdBeE', 'bytea');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info(E'\\\\xDeAdBeE', 'bytea');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid hexadecimal data: odd number of digits"), Null, Null, T("22023")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info(E'\\\\xDeAdBeEx', 'bytea');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid hexadecimal digit: \"x\""), Null, Null, T("22023")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info(E'foo\\\\99bar', 'bytea');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type bytea"), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's tid regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(0)', 'tid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(0)', 'tid');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type tid: \"(0)\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('(0,-1)', 'tid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('(0,-1)', 'tid');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type tid: \"(0,-1)\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's time regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('12:00:00', 'time');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('25:00:00', 'time');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('15:36:39 America/New_York', 'time');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('25:00:00', 'time');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("date/time field value out of range: \"25:00:00\""), Null, Null, T("22008")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('15:36:39 America/New_York', 'time');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type time: \"15:36:39 America/New_York\""), Null, Null, T("22007")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's timestamp regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('now', 'timestamp');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('garbage', 'timestamp');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('2001-01-01 00:00 Nehwon/Lankhmar', 'timestamp');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('garbage', 'timestamp');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type timestamp: \"garbage\""), Null, Null, T("22007")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('2001-01-01 00:00 Nehwon/Lankhmar', 'timestamp');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("time zone \"nehwon/lankhmar\" not recognized"), Null, Null, T("22023")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's timestamptz regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('now', 'timestamptz');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('garbage', 'timestamptz');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('2001-01-01 00:00 Nehwon/Lankhmar', 'timestamptz');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('garbage', 'timestamptz');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type timestamp with time zone: \"garbage\""), Null, Null, T("22007")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('2001-01-01 00:00 Nehwon/Lankhmar', 'timestamptz');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("time zone \"nehwon/lankhmar\" not recognized"), Null, Null, T("22023")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's timetz regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('12:00:00 PDT', 'timetz');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('25:00:00 PDT', 'timetz');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('15:36:39 America/New_York', 'timetz');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('25:00:00 PDT', 'timetz');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("date/time field value out of range: \"25:00:00 PDT\""), Null, Null, T("22008")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('15:36:39 America/New_York', 'timetz');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type time with time zone: \"15:36:39 America/New_York\""), Null, Null, T("22007")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's tstypes regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('foo', 'tsvector');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid($$''$$, 'tsvector');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info($$''$$, 'tsvector');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("syntax error in tsvector: \"''\""), Null, Null, T("42601")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('foo', 'tsquery');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('foo!', 'tsquery');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('foo!', 'tsquery');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("syntax error in tsquery: \"foo!\""), Null, Null, T("42601")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('a <100000> b', 'tsquery');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("distance in phrase operator must be an integer value between zero and 16384 inclusive"), Null, Null, T("22023")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's uuid regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('11', 'uuid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('11', 'uuid');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type uuid: \"11\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's varchar regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('abcd  ', 'varchar(4)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('abcde', 'varchar(4)');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('abcde', 'varchar(4)');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value too long for type character varying(4)"), Null, Null, T("22001")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's xid regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('42', 'xid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('asdf', 'xid');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('0xffffffffff', 'xid');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"0xffffffffff\" is out of range for type xid"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('42', 'xid8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('asdf', 'xid8');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('0xffffffffffffffffffff', 'xid8');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("value \"0xffffffffffffffffffff\" is out of range for type xid8"), Null, Null, T("22003")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('12:13:', 'pg_snapshot');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('31:12:', 'pg_snapshot');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('31:12:', 'pg_snapshot');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type pg_snapshot: \"31:12:\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select pg_input_is_valid('12:16:14,13', 'pg_snapshot');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "select * from pg_input_error_info('12:16:14,13', 'pg_snapshot');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT), Column("detail", TEXT), Column("hint", TEXT), Column("sql_error_code", TEXT)],
                        rows: &[
                            &[T("invalid input syntax for type pg_snapshot: \"12:16:14,13\""), Null, Null, T("22P02")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "pg_input_is_valid and pg_input_error_info in PG18's xml regress module",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('<value>one</value>', 'xml');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('<value>one</', 'xml');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM pg_input_error_info('<value>one</', 'xml');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("invalid XML content")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_input_is_valid('<?xml version=\"1.0\" standalone=\"y\"?><foo/>', 'xml');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_input_is_valid", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT message FROM pg_input_error_info('<?xml version=\"1.0\" standalone=\"y\"?><foo/>', 'xml');",
                    expected: Expected::Rows {
                        columns: &[Column("message", TEXT)],
                        rows: &[
                            &[T("invalid XML content: invalid XML declaration")],
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
fn test_type_string_errors_have_no_position() {
    run_scripts(&[
        ScriptTest {
            name: "a type modifier error in a type name read from a string has no position in the statement",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_input_error_info('numeric(1,2,3)', 'regtype');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid NUMERIC type modifier", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_regtype('numeric(1,2,3)');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid NUMERIC type modifier", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'numeric(1,2,3)'::regtype;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid NUMERIC type modifier", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_an_error_late_in_a_long_statement_points_at_its_character_position() {
    run_scripts(&[
        ScriptTest {
            name: "an error late in a long multi-row INSERT with multibyte text points at its character position",
            set_up_script: &["CREATE TABLE pos_t (a INT, b TEXT);"],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO pos_t VALUES (1, 'ééé'), (2, 'ünïcödé ☃'), (3, '日本語'), (4, 'x4'), (5, 'x5'), (6, 'x6'), (7, 'x7'), (8, 'x8'), (9, 'x9'), (10, 'x10'), (11, 'x11'), (12, 'x12'), (13, 'x13'), (14, 'x14'), (15, 'x15'), (16, 'x16'), (17, 'x17'), (18, 'x18'), (19, 'x19'), (20, 'x20'), (21, 'x21'), (22, 'x22'), (23, 'x23'), (24, 'x24'), (25, 'x25'), (26, 'x26'), (27, 'x27'), (28, 'x28'), (29, 'x29'), (30, 'é'), ('nope', 'end');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type integer: \"nope\"", position: 406, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
