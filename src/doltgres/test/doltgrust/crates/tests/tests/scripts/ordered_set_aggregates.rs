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
fn test_ordered_set_aggregates() {
    run_scripts(&[
        ScriptTest {
            name: "ordered-set aggregates",
            set_up_script: &[
                "CREATE TABLE osa (g text, x integer, f float8, i interval, t text);",
                "INSERT INTO osa VALUES ('a', 1, 1.5, '1 day', 'p'), ('a', 3, 2.5, '3 days', 'q'), ('a', 3, NULL, NULL, 'q'), ('a', NULL, 4.5, '5 days', NULL), ('b', 10, 10.0, '10 hours', 'r'), ('b', 20, 20.0, '20 hours', 's');",
                "CREATE AGGREGATE my_percentile_disc(float8 ORDER BY anyelement) (STYPE = internal, SFUNC = ordered_set_transition, FINALFUNC = percentile_disc_final, FINALFUNC_EXTRA = true, FINALFUNC_MODIFY = read_write);",
                r#"CREATE AGGREGATE my_rank(VARIADIC "any" ORDER BY VARIADIC "any") (STYPE = internal, SFUNC = ordered_set_transition_multi, FINALFUNC = rank_final, FINALFUNC_EXTRA = true, HYPOTHETICAL);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY x), percentile_cont(0.5) WITHIN GROUP (ORDER BY f), percentile_cont(0.25) WITHIN GROUP (ORDER BY i), mode() WITHIN GROUP (ORDER BY t) FROM osa GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", TEXT), Column("percentile_disc", INT4), Column("percentile_cont", FLOAT8), Column("percentile_cont", INTERVAL), Column("mode", TEXT)],
                        rows: &[
                            &[T("a"), T("3"), T("2.5"), T("2 days"), T("q")],
                            &[T("b"), T("10"), T("15"), T("12:30:00"), T("r")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_disc(ARRAY[0, 0.25, NULL, 1]) WITHIN GROUP (ORDER BY x), percentile_cont(ARRAY[0.1, 0.9]) WITHIN GROUP (ORDER BY f), percentile_cont(ARRAY[0.5]) WITHIN GROUP (ORDER BY i) FROM osa;",
                    expected: Expected::Rows {
                        columns: &[Column("percentile_disc", INT4_ARRAY), Column("percentile_cont", FLOAT8_ARRAY), Column("percentile_cont", INTERVAL_ARRAY)],
                        rows: &[
                            &[T("{1,3,NULL,20}"), T("{1.9,16}"), T(r#"{"1 day"}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY x), percentile_disc(0.9) WITHIN GROUP (ORDER BY x), percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM osa GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", TEXT), Column("percentile_disc", INT4), Column("percentile_disc", INT4), Column("percentile_cont", FLOAT8)],
                        rows: &[
                            &[T("a"), T("3"), T("3"), T("3")],
                            &[T("b"), T("10"), T("20"), T("15")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY x), percentile_cont(0.5) WITHIN GROUP (ORDER BY f), mode() WITHIN GROUP (ORDER BY t), rank(1) WITHIN GROUP (ORDER BY x), cume_dist(1) WITHIN GROUP (ORDER BY x) FROM osa WHERE false;",
                    expected: Expected::Rows {
                        columns: &[Column("percentile_disc", INT4), Column("percentile_cont", FLOAT8), Column("mode", TEXT), Column("rank", INT8), Column("cume_dist", FLOAT8)],
                        rows: &[
                            &[Null, Null, Null, T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, percentile_disc(NULL::float8) WITHIN GROUP (ORDER BY x) FROM osa GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", TEXT), Column("percentile_disc", INT4)],
                        rows: &[
                            &[T("a"), Null],
                            &[T("b"), Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY x) FILTER (WHERE g = 'b'), rank(3) WITHIN GROUP (ORDER BY x) FILTER (WHERE x > 1) FROM osa;",
                    expected: Expected::Rows {
                        columns: &[Column("percentile_disc", INT4), Column("rank", INT8)],
                        rows: &[
                            &[T("10"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_disc(1.5) WITHIN GROUP (ORDER BY x) FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "percentile value 1.5 is not between 0 and 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_cont(ARRAY[0.5, -1]) WITHIN GROUP (ORDER BY f) FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "percentile value -1 is not between 0 and 1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY x) OVER () FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "OVER is not supported for ordered-set aggregate percentile_disc", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_disc(DISTINCT 0.5) WITHIN GROUP (ORDER BY x) FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "cannot use DISTINCT with WITHIN GROUP", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT percentile_disc(0.5) FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function percentile_disc(numeric) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rank(3) WITHIN GROUP (ORDER BY x), dense_rank(3) WITHIN GROUP (ORDER BY x), percent_rank(3) WITHIN GROUP (ORDER BY x), cume_dist(3) WITHIN GROUP (ORDER BY x) FROM osa;",
                    expected: Expected::Rows {
                        columns: &[Column("rank", INT8), Column("dense_rank", INT8), Column("percent_rank", FLOAT8), Column("cume_dist", FLOAT8)],
                        rows: &[
                            &[T("2"), T("2"), T("0.16666666666666666"), T("0.5714285714285714")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rank(NULL) WITHIN GROUP (ORDER BY x NULLS FIRST), rank(15, 'r') WITHIN GROUP (ORDER BY x DESC, t) FROM osa;",
                    expected: Expected::Rows {
                        columns: &[Column("rank", INT8), Column("rank", INT8)],
                        rows: &[
                            &[T("1"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rank(1, 2) WITHIN GROUP (ORDER BY x) FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function rank(integer, integer, integer) does not exist", hint: "To use the hypothetical-set aggregate rank, the number of hypothetical direct arguments (here 2) must match the number of ordering columns (here 1).", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rank('a') WITHIN GROUP (ORDER BY x) FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "a""#, position: 13, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rank(x) WITHIN GROUP (ORDER BY x) FROM osa;",
                    expected: Expected::Error(Diagnostic { code: "42803", message: r#"column "osa.x" must appear in the GROUP BY clause or be used in an aggregate function"#, detail: "Direct arguments of an ordered-set aggregate must use only grouped columns.", position: 13, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, my_percentile_disc(0.5) WITHIN GROUP (ORDER BY t), my_rank(15, 'r') WITHIN GROUP (ORDER BY x DESC, t) FROM osa GROUP BY g ORDER BY g;",
                    expected: Expected::Rows {
                        columns: &[Column("g", TEXT), Column("my_percentile_disc", TEXT), Column("my_rank", INT8)],
                        rows: &[
                            &[T("a"), T("q"), T("2")],
                            &[T("b"), T("r"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT my_percentile_disc(0.5) WITHIN GROUP (ORDER BY x), percentile_disc(0.5) WITHIN GROUP (ORDER BY x) FROM osa;",
                    expected: Expected::Rows {
                        columns: &[Column("my_percentile_disc", INT4), Column("percentile_disc", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
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
