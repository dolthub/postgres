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
fn test_create_operator() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE OPERATOR with LEFTARG, RIGHTARG, and FUNCTION",
            set_up_script: &[
                r#"CREATE FUNCTION op_int_dist(a int4, b int4) RETURNS int4
					AS $$ SELECT abs(a - b) $$ LANGUAGE SQL;"#,
                "CREATE TABLE op_points (pk int4 PRIMARY KEY, v int4);",
                "INSERT INTO op_points VALUES (1, 4), (2, 9), (3, 15);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_int_dist);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 3 <-> 10;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 10 <-> 3;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (NULL::int4 <-> 3) IS NULL;",
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
                    query: "SELECT pk, v <-> 10 FROM op_points ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("1"), T("6")],
                            &[T("2"), T("1")],
                            &[T("3"), T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pk FROM op_points WHERE v <-> 10 < 3 ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT oprname, oprkind, oprcanhash, oprcanmerge, oprleft::regtype::text, oprright::regtype::text, oprresult::regtype::text
						FROM pg_operator WHERE oprcode::text = 'op_int_dist';"#,
                    expected: Expected::Rows {
                        columns: &[Column("oprname", NAME), Column("oprkind", CHAR), Column("oprcanhash", BOOL), Column("oprcanmerge", BOOL), Column("oprleft", TEXT), Column("oprright", TEXT), Column("oprresult", TEXT)],
                        rows: &[
                            &[T("<->"), T("b"), T("f"), T("f"), T("integer"), T("integer"), T("integer")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n.nspname FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace WHERE o.oprcode::text = 'op_int_dist';",
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME)],
                        rows: &[
                            &[T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OPERATOR with COMMUTATOR and NEGATOR",
            set_up_script: &[
                r#"CREATE FUNCTION op_len_eq(a text, b text) RETURNS boolean
					AS $$ SELECT length(a) = length(b) $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION op_len_ne(a text, b text) RETURNS boolean
					AS $$ SELECT length(a) <> length(b) $$ LANGUAGE SQL;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <=> (LEFTARG = text, RIGHTARG = text, FUNCTION = op_len_eq, COMMUTATOR = <=>);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <~> (LEFTARG = text, RIGHTARG = text, FUNCTION = op_len_ne, COMMUTATOR = <~>, NEGATOR = <=>);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'abc' <=> 'xyz';",
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
                    query: "SELECT 'abc' <=> 'wxyz';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'abc' <~> 'wxyz';",
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
                    query: "SELECT c.oprname FROM pg_operator o JOIN pg_operator c ON c.oid = o.oprcom WHERE o.oprcode::text = 'op_len_eq';",
                    expected: Expected::Rows {
                        columns: &[Column("oprname", NAME)],
                        rows: &[
                            &[T("<=>")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n.oprname FROM pg_operator o JOIN pg_operator n ON n.oid = o.oprnegate WHERE o.oprcode::text = 'op_len_ne';",
                    expected: Expected::Rows {
                        columns: &[Column("oprname", NAME)],
                        rows: &[
                            &[T("<=>")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT n.oprname FROM pg_operator o JOIN pg_operator n ON n.oid = o.oprnegate WHERE o.oprcode::text = 'op_len_eq';",
                    expected: Expected::Rows {
                        columns: &[Column("oprname", NAME)],
                        rows: &[
                            &[T("<~>")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OPERATOR with HASHES and MERGES",
            set_up_script: &[
                r#"CREATE FUNCTION op_ci_eq(a text, b text) RETURNS boolean
					AS $$ SELECT lower(a) = lower(b) $$ LANGUAGE SQL;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <%> (LEFTARG = text, RIGHTARG = text, FUNCTION = op_ci_eq, COMMUTATOR = <%>, HASHES, MERGES);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'ABC' <%> 'abc';",
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
                    query: "SELECT oprcanhash, oprcanmerge FROM pg_operator WHERE oprcode::text = 'op_ci_eq';",
                    expected: Expected::Rows {
                        columns: &[Column("oprcanhash", BOOL), Column("oprcanmerge", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OPERATOR with composite operands",
            set_up_script: &[
                "CREATE TABLE op_pair (x int4, y int4);",
                r#"CREATE FUNCTION op_pair_add(a op_pair, b op_pair) RETURNS op_pair
					AS $$ SELECT ROW((a).x + (b).x, (a).y + (b).y)::op_pair $$ LANGUAGE SQL;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <+> (LEFTARG = op_pair, RIGHTARG = op_pair, FUNCTION = op_pair_add);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ROW(1, 2)::op_pair <+> ROW(3, 4)::op_pair;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", USER_DEFINED)],
                        rows: &[
                            &[T("(4,6)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT oprleft::regtype::text, oprright::regtype::text, oprresult::regtype::text
						FROM pg_operator WHERE oprcode::text = 'op_pair_add';"#,
                    expected: Expected::Rows {
                        columns: &[Column("oprleft", TEXT), Column("oprright", TEXT), Column("oprresult", TEXT)],
                        rows: &[
                            &[T("op_pair"), T("op_pair"), T("op_pair")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OPERATOR with composite operands on table rows in aggregate",
            set_up_script: &[
                "CREATE TABLE op_pair (x int4, y int4);",
                r#"CREATE FUNCTION op_pair_add(a op_pair, b op_pair) RETURNS op_pair
			AS $$ SELECT ROW((a).x + (b).x, (a).y + (b).y)::op_pair $$ LANGUAGE SQL;"#,
                "CREATE OPERATOR <+> (LEFTARG = op_pair, RIGHTARG = op_pair, FUNCTION = op_pair_add);",
                "INSERT INTO op_pair VALUES (1, 2), (3, 4), (5, 6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT sum(((p <+> ROW(10, 20)::op_pair)).x), sum(((p <+> ROW(10, 20)::op_pair)).y)
			       FROM op_pair p;"#,
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("39"), T("72")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OPERATOR with mixed operand types",
            set_up_script: &[
                r#"CREATE FUNCTION op_repeat(a text, b int4) RETURNS text
					AS $$ SELECT repeat(a, b) $$ LANGUAGE SQL;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <#> (LEFTARG = text, RIGHTARG = int4, FUNCTION = op_repeat);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'ab' <#> 3;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("ababab")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 3 <#> 'ab';",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer <#> unknown", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 10, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DROP OPERATOR smoke test",
            set_up_script: &[
                r#"CREATE FUNCTION op_drop_dist(a int4, b int4) RETURNS int4
					AS $$ SELECT abs(a - b) $$ LANGUAGE SQL;"#,
                "CREATE OPERATOR <-> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_drop_dist);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT 3 <-> 10;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR <-> (int4, int4);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 3 <-> 10;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer <-> integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 10, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT EXISTS (SELECT 1 FROM pg_operator WHERE oprcode::text = 'op_drop_dist');",
                    expected: Expected::Rows {
                        columns: &[Column("exists", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_drop_dist);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 3 <-> 10;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OPERATOR validation",
            set_up_script: &[
                r#"CREATE FUNCTION op_valid_dist(a int4, b int4) RETURNS int4
					AS $$ SELECT abs(a - b) $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION op_one_arg(a int4) RETURNS int4
					AS $$ SELECT a $$ LANGUAGE SQL;"#,
                "CREATE OPERATOR <-> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_valid_dist);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <=> (LEFTARG = int4, RIGHTARG = int4);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "operator function must be specified", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <=> (FUNCTION = op_valid_dist);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "operator argument types must be specified", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <=> (LEFTARG = int4, FUNCTION = op_valid_dist);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "operator right argument type must be specified", detail: "Postfix operators are not supported.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <=> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_missing_fn);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function op_missing_fn(integer, integer) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <=> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_one_arg);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function op_one_arg(integer, integer) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_valid_dist);",
                    expected: Expected::Error(Diagnostic { code: "42723", message: "operator <-> already exists", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "CREATE OPERATOR prefix operators, options, and DROP OPERATOR errors",
            set_up_script: &[
                r#"CREATE FUNCTION op_prefix_neg(b int4) RETURNS int4
					AS $$ SELECT -b $$ LANGUAGE SQL;"#,
                r#"CREATE FUNCTION op_prefix_sub(a int4, b int4) RETURNS int4
					AS $$ SELECT a - b $$ LANGUAGE SQL;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP OPERATOR <-> (int4, int4);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: integer <-> integer", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR IF EXISTS <-> (int4, int4);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    notices: &[Diagnostic { code: "00000", message: "operator <-> does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (RIGHTARG = int4, FUNCTION = op_prefix_neg, COMMUTATOR = <->);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "only binary operators can have commutators", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (RIGHTARG = int4, FUNCTION = op_prefix_neg, HASHES);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "only binary operators can hash", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = op_prefix_sub, NEGATOR = <>);",
                    expected: Expected::Error(Diagnostic { code: "42P13", message: "only boolean operators can have negators", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (LEFTARG = int4, RIGHTARG = int4, PROCEDURE = op_prefix_sub, BOGUS);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    notices: &[Diagnostic { severity: "WARNING", code: "42601", message: r#"operator attribute "bogus" not recognized"#, ..E }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR <-> (RIGHTARG = int4, FUNCTION = op_prefix_neg);",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT <-> 5, 7 <-> 5;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4), Column("?column?", INT4)],
                        rows: &[
                            &[T("-5"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oprkind, oprleft, oprright, oprresult, oprowner, oprcode::text, oprrest::text, oprjoin::text FROM pg_operator WHERE oprname = '<->' AND oprnamespace = 'public'::regnamespace ORDER BY oprkind;",
                    expected: Expected::Rows {
                        columns: &[Column("oprkind", CHAR), Column("oprleft", OID), Column("oprright", OID), Column("oprresult", OID), Column("oprowner", OID), Column("oprcode", TEXT), Column("oprrest", TEXT), Column("oprjoin", TEXT)],
                        rows: &[
                            &[T("b"), T("23"), T("23"), T("23"), T("10"), T("op_prefix_sub"), T("-"), T("-")],
                            &[T("l"), T("0"), T("23"), T("23"), T("10"), T("op_prefix_neg"), T("-"), T("-")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR <-> (none, int4);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR <-> (int4, int4);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_operator_classes_with_sql_support_functions() {
    run_scripts(&[
        ScriptTest {
            name: "sorts, merge joins, hash joins, hashed ANY, and ordered aggregates by operator classes whose support functions are SQL functions",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 > $2 THEN -1 ELSE 1 END $$;",
                "CREATE FUNCTION sql_hash(INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT $1 % 7 $$;",
                "CREATE OPERATOR ~<~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4gt);",
                "CREATE OPERATOR ~<=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4ge);",
                "CREATE OPERATOR ~=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4eq, HASHES, MERGES);",
                "CREATE OPERATOR ~>=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4le);",
                "CREATE OPERATOR ~>~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4lt);",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 ~<~, OPERATOR 2 ~<=~, OPERATOR 3 ~=~, OPERATOR 4 ~>=~, OPERATOR 5 ~>~, FUNCTION 1 rev_cmp(INT4, INT4);",
                "CREATE OPERATOR CLASS sql_hash_ops FOR TYPE INT4 USING hash AS OPERATOR 1 ~=~, FUNCTION 1 sql_hash(INT4);",
                "CREATE TABLE r (id INT PRIMARY KEY, a INT);",
                "INSERT INTO r SELECT i, i FROM generate_series(1, 10) i;",
                "INSERT INTO r VALUES (11, NULL);",
                "CREATE TABLE s (b INT);",
                "INSERT INTO s VALUES (2), (4), (4), (6), (20);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a FROM r ORDER BY a USING ~<~ NULLS FIRST LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[Null],
                            &[T("10")],
                            &[T("9")],
                            &[T("8")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r ORDER BY a USING ~>~;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[Null],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                            &[T("10")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r ORDER BY a USING ~<~ LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("10")],
                            &[T("9")],
                            &[T("8")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashjoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_nestloop = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM r JOIN s ON a ~=~ b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Merge Join")],
                            &[T("  Merge Cond: (r.a ~=~ s.b)")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: r.a USING ~<~")],
                            &[T("        ->  Seq Scan on r")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: s.b USING ~<~")],
                            &[T("        ->  Seq Scan on s")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM r JOIN s ON a ~=~ b ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("4"), T("4")],
                            &[T("4"), T("4")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashjoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_mergejoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM r JOIN s ON a ~=~ b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Hash Join")],
                            &[T("  Hash Cond: (r.a ~=~ s.b)")],
                            &[T("  ->  Seq Scan on r")],
                            &[T("  ->  Hash")],
                            &[T("        ->  Seq Scan on s")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM r JOIN s ON a ~=~ b ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("4"), T("4")],
                            &[T("4"), T("4")],
                            &[T("6"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_nestloop;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_mergejoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a ~=~ ANY (ARRAY[1, 3, 5, 7, 9, 11, 13, 15, 17]) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                            &[T("5")],
                            &[T("7")],
                            &[T("9")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT array_agg(a ORDER BY a USING ~<~) FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("array_agg", INT4_ARRAY)],
                        rows: &[
                            &[T("{10,9,8,7,6,5,4,3,2,1,NULL}")],
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
fn test_drop_operator_if_exists_names_the_missing_type_or_schema() {
    run_scripts(&[
        ScriptTest {
            name: "DROP OPERATOR IF EXISTS skips naming a missing argument type or schema",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP OPERATOR IF EXISTS + (no_such_type, no_such_type);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    notices: &[Diagnostic { code: "00000", message: r#"type "no_such_type" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR IF EXISTS + (no_such_schema.no_such_type, no_such_schema.no_such_type);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    notices: &[Diagnostic { code: "00000", message: r#"schema "no_such_schema" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR IF EXISTS # (NONE, no_such_schema.no_such_type);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    notices: &[Diagnostic { code: "00000", message: r#"schema "no_such_schema" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_alter_operator_and_create_operator_change_built_in_operators() {
    run_scripts(&[
        ScriptTest {
            name: "ALTER OPERATOR and a new operator's commutator change a built-in operator's row",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER OPERATOR pg_catalog.= (int4, int4) SET (RESTRICT = scalarltsel, JOIN = scalarltjoinsel);",
                    expected: Expected::Tag("ALTER OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oprrest, oprjoin FROM pg_operator WHERE oid = '=(int4,int4)'::regoperator;",
                    expected: Expected::Rows {
                        columns: &[Column("oprrest", REGPROC), Column("oprjoin", REGPROC)],
                        rows: &[
                            &[T("scalarltsel"), T("scalarltjoinsel")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR #-# (LEFTARG = int4, RIGHTARG = int4, FUNCTION = int4mi, COMMUTATOR = OPERATOR(pg_catalog.-));",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oprcom::regoperator FROM pg_operator WHERE oid = '-(int4,int4)'::regoperator;",
                    expected: Expected::Rows {
                        columns: &[Column("oprcom", REGOPERATOR)],
                        rows: &[
                            &[T("#-#(integer,integer)")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE r2;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE r2;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR pg_catalog.= (int4, int4) SET (RESTRICT = eqsel);",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of operator =", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
