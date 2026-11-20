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
fn test_btree_comparator_order() {
    run_scripts(&[
        ScriptTest {
            name: "btree indexes under an operator class whose SQL comparison function reverses the order",
            set_up_script: &[
                "CREATE FUNCTION rev_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 > $2 THEN -1 ELSE 1 END $$;",
                "CREATE OPERATOR ~<~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4gt);",
                "CREATE OPERATOR ~<=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4ge);",
                "CREATE OPERATOR ~>=~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4le);",
                "CREATE OPERATOR ~>~ (LEFTARG = INT4, RIGHTARG = INT4, FUNCTION = int4lt);",
                "CREATE OPERATOR CLASS rev_ops FOR TYPE INT4 USING btree AS OPERATOR 1 ~<~, OPERATOR 2 ~<=~, OPERATOR 3 =, OPERATOR 4 ~>=~, OPERATOR 5 ~>~, FUNCTION 1 rev_cmp(INT4, INT4);",
                "CREATE TABLE r (id INT PRIMARY KEY, a INT, b TEXT);",
                "INSERT INTO r SELECT i, i, 'v' || (i % 3) FROM generate_series(1, 10) i;",
                "INSERT INTO r VALUES (11, NULL, 'v1'), (12, NULL, NULL);",
                "CREATE INDEX r_a ON r (a rev_ops);",
                "CREATE INDEX r_ad ON r (a rev_ops DESC NULLS FIRST);",
                "CREATE INDEX r_ba ON r (b, a rev_ops);",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM r WHERE a ~<~ 5;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using r_ad on r")],
                            &[T("  Index Cond: (a ~<~ 5)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a ~<~ 5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                            &[T("10")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a ~>=~ 3 ORDER BY a USING ~<~;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("2")],
                            &[T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a ~>=~ 3 ORDER BY a USING ~>~;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a ~<=~ 7 AND a ~>~ 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r ORDER BY a USING ~<~ NULLS FIRST LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[Null],
                            &[Null],
                            &[T("10")],
                            &[T("9")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r ORDER BY a USING ~>~ NULLS LAST LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM r ORDER BY a USING ~>~ NULLS LAST LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: a USING ~>~ NULLS LAST")],
                            &[T("        ->  Index Only Scan using r_ba on r")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, a FROM r WHERE b = 'v1' ORDER BY b, a USING ~<~;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("a", INT4)],
                        rows: &[
                            &[T("v1"), T("10")],
                            &[T("v1"), T("7")],
                            &[T("v1"), T("4")],
                            &[T("v1"), T("1")],
                            &[T("v1"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, a FROM r WHERE b = 'v1' AND a ~<~ 7;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Only Scan using r_ba on r")],
                            &[T("  Index Cond: ((b = 'v1'::text) AND (a ~<~ 7))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, a FROM r WHERE b = 'v1' AND a ~<~ 7;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("a", INT4)],
                        rows: &[
                            &[T("v1"), T("10")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO r SELECT i, i, 'v' || (i % 3) FROM generate_series(13, 20) i;",
                    expected: Expected::Tag("INSERT 0 8"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM r WHERE id IN (2, 15);",
                    expected: Expected::Tag("DELETE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE r SET a = 30 WHERE id = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM r;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a ~<~ 12;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("13")],
                            &[T("14")],
                            &[T("16")],
                            &[T("17")],
                            &[T("18")],
                            &[T("19")],
                            &[T("20")],
                            &[T("30")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM r WHERE a ~>~ 12 ORDER BY a USING ~>~;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("4")],
                            &[T("5")],
                            &[T("6")],
                            &[T("7")],
                            &[T("8")],
                            &[T("9")],
                            &[T("10")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b, a FROM r WHERE b = 'v0' ORDER BY b, a USING ~<~;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT), Column("a", INT4)],
                        rows: &[
                            &[T("v0"), T("30")],
                            &[T("v0"), T("18")],
                            &[T("v0"), T("9")],
                            &[T("v0"), T("6")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_btree_comparator_uniqueness() {
    run_scripts(&[
        ScriptTest {
            name: "a unique index under a case-insensitive operator class treats keys its comparison function calls equal as duplicates",
            set_up_script: &[
                "CREATE FUNCTION ci_cmp(TEXT, TEXT) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN lower($1) < lower($2) THEN -1 WHEN lower($1) > lower($2) THEN 1 ELSE 0 END $$;",
                "CREATE FUNCTION ci_lt(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) < lower($2) $$;",
                "CREATE FUNCTION ci_le(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) <= lower($2) $$;",
                "CREATE FUNCTION ci_eq(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) = lower($2) $$;",
                "CREATE FUNCTION ci_ge(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) >= lower($2) $$;",
                "CREATE FUNCTION ci_gt(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) > lower($2) $$;",
                "CREATE OPERATOR <~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_lt);",
                "CREATE OPERATOR <=~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_le);",
                "CREATE OPERATOR =~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_eq);",
                "CREATE OPERATOR >=~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_ge);",
                "CREATE OPERATOR >~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_gt);",
                "CREATE OPERATOR CLASS ci_ops FOR TYPE TEXT USING btree AS OPERATOR 1 <~, OPERATOR 2 <=~, OPERATOR 3 =~, OPERATOR 4 >=~, OPERATOR 5 >~, FUNCTION 1 ci_cmp(TEXT, TEXT);",
                "CREATE TABLE u (id INT PRIMARY KEY, name TEXT);",
                "CREATE UNIQUE INDEX u_name ON u (name ci_ops);",
                "CREATE TABLE n (id INT PRIMARY KEY, name TEXT);",
                "CREATE UNIQUE INDEX n_name ON n (name ci_ops) NULLS NOT DISTINCT;",
                "CREATE TABLE d (id INT PRIMARY KEY, name TEXT);",
                "INSERT INTO d VALUES (1, 'a'), (2, 'A');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (1, 'Apple'), (2, 'banana'), (3, NULL), (4, NULL);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (5, 'APPLE');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"u_name\"", detail: "Key (name)=(APPLE) already exists.", schema: "public", table: "u", constraint: "u_name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (5, 'Cherry'), (6, 'cherry');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"u_name\"", detail: "Key (name)=(cherry) already exists.", schema: "public", table: "u", constraint: "u_name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (6, 'CHERRY') ON CONFLICT (name) DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (7, 'BaNaNa') ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (8, 'apple') ON CONFLICT (name) DO UPDATE SET name = excluded.name;",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE u SET name = 'BANANA' WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"u_name\"", detail: "Key (name)=(BANANA) already exists.", schema: "public", table: "u", constraint: "u_name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE u SET name = 'BANANA' WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE u SET name = 'Date' WHERE id = 3;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, name FROM u ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("apple")],
                            &[T("2"), T("BANANA")],
                            &[T("3"), T("Date")],
                            &[T("4"), Null],
                            &[T("6"), T("CHERRY")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM u WHERE name =~ 'cherry';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, name FROM u WHERE name >~ 'b' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("2"), T("BANANA")],
                            &[T("3"), T("Date")],
                            &[T("6"), T("CHERRY")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM u;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO u VALUES (9, 'DATE');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"u_name\"", detail: "Key (name)=(DATE) already exists.", schema: "public", table: "u", constraint: "u_name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM u WHERE name =~ 'DATE';",
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
                    query: "INSERT INTO n VALUES (1, NULL), (2, 'x');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO n VALUES (3, NULL);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"n_name\"", detail: "Key (name)=(null) already exists.", schema: "public", table: "n", constraint: "n_name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO n VALUES (4, 'X');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"n_name\"", detail: "Key (name)=(X) already exists.", schema: "public", table: "n", constraint: "n_name", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX d_name ON d (name ci_ops);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"d_name\"", detail: "Key (name)=(a) is duplicated.", schema: "public", table: "d", constraint: "d_name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "an insert of a key a running transaction's key is equal to under a case-insensitive operator class waits for it, and an insert of an unequal key does not",
            set_up_script: &[
                "CREATE FUNCTION ci_cmp(TEXT, TEXT) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN lower($1) < lower($2) THEN -1 WHEN lower($1) > lower($2) THEN 1 ELSE 0 END $$;",
                "CREATE FUNCTION ci_lt(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) < lower($2) $$;",
                "CREATE FUNCTION ci_le(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) <= lower($2) $$;",
                "CREATE FUNCTION ci_eq(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) = lower($2) $$;",
                "CREATE FUNCTION ci_ge(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) >= lower($2) $$;",
                "CREATE FUNCTION ci_gt(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) > lower($2) $$;",
                "CREATE OPERATOR <~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_lt);",
                "CREATE OPERATOR <=~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_le);",
                "CREATE OPERATOR =~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_eq);",
                "CREATE OPERATOR >=~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_ge);",
                "CREATE OPERATOR >~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_gt);",
                "CREATE OPERATOR CLASS ci_ops FOR TYPE TEXT USING btree AS OPERATOR 1 <~, OPERATOR 2 <=~, OPERATOR 3 =~, OPERATOR 4 >=~, OPERATOR 5 >~, FUNCTION 1 ci_cmp(TEXT, TEXT);",
                "CREATE TABLE w (id INT PRIMARY KEY, name TEXT);",
                "CREATE UNIQUE INDEX w_name ON w (name ci_ops);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO w VALUES (1, 'Apple');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO w VALUES (2, 'APPLE');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"w_name\"", detail: "Key (name)=(APPLE) already exists.", schema: "public", table: "w", constraint: "w_name", ..E }),
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ INSERT INTO w VALUES (3, 'Cherry');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ INSERT INTO w VALUES (4, 'banana');",
                    expected: Expected::Tag("INSERT 0 1"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT id, name FROM w ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT)],
                        rows: &[
                            &[T("1"), T("Apple")],
                            &[T("4"), T("banana")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_btree_comparison_function_errors() {
    run_scripts(&[
        ScriptTest {
            name: "an operator class's comparison function that raises an error fails the statement that compares by it",
            set_up_script: &[
                "CREATE FUNCTION bad_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = 13 OR $2 = 13 THEN $1 / ($2 - $2) WHEN $1 = $2 THEN 0 WHEN $1 < $2 THEN -1 ELSE 1 END $$;",
                "CREATE OPERATOR CLASS bad_ops FOR TYPE INT4 USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 bad_cmp(INT4, INT4);",
                "CREATE TABLE b (id INT PRIMARY KEY, a INT);",
                "INSERT INTO b SELECT i, i FROM generate_series(1, 20) i WHERE i <> 13;",
                "CREATE TABLE c (id INT PRIMARY KEY, a INT);",
                "INSERT INTO c SELECT i, i FROM generate_series(1, 20) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX c_a ON c (a bad_ops);",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX b_a ON b (a bad_ops);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (13, 13);",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO b VALUES (21, 21);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM b;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("20")],
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
fn test_btree_comparator_primary_keys() {
    run_scripts(&[
        ScriptTest {
            name: "primary keys over types whose default btree operator class has no key encoding",
            set_up_script: &[
                "CREATE FUNCTION ci_cmp(TEXT, TEXT) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN lower($1) < lower($2) THEN -1 WHEN lower($1) > lower($2) THEN 1 ELSE 0 END $$;",
                "CREATE FUNCTION ci_lt(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) < lower($2) $$;",
                "CREATE FUNCTION ci_le(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) <= lower($2) $$;",
                "CREATE FUNCTION ci_eq(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) = lower($2) $$;",
                "CREATE FUNCTION ci_ge(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) >= lower($2) $$;",
                "CREATE FUNCTION ci_gt(TEXT, TEXT) RETURNS BOOL LANGUAGE sql IMMUTABLE AS $$ SELECT lower($1) > lower($2) $$;",
                "CREATE OPERATOR <~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_lt);",
                "CREATE OPERATOR <=~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_le);",
                "CREATE OPERATOR =~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_eq);",
                "CREATE OPERATOR >=~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_ge);",
                "CREATE OPERATOR >~ (LEFTARG = TEXT, RIGHTARG = TEXT, FUNCTION = ci_gt);",
                "CREATE OPERATOR CLASS ci_ops FOR TYPE TEXT USING btree AS OPERATOR 1 <~, OPERATOR 2 <=~, OPERATOR 3 =~, OPERATOR 4 >=~, OPERATOR 5 >~, FUNCTION 1 ci_cmp(TEXT, TEXT);",
                "CREATE TABLE q (name TEXT NOT NULL);",
                "CREATE UNIQUE INDEX q_name ON q (name ci_ops);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE q ADD CONSTRAINT q_pk PRIMARY KEY USING INDEX q_name;",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "index \"q_name\" column number 1 does not have default sorting behavior", detail: "Cannot create a primary key or unique constraint using such an index.", position: 19, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tv (v TSVECTOR PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tv VALUES ('b a'), ('c');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT v FROM tv ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", TSVECTOR)],
                        rows: &[
                            &[T("'c'")],
                            &[T("'a' 'b'")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE tq (id INT PRIMARY KEY, v TSVECTOR, w TSQUERY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX tq_v ON tq (v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX tq_w ON tq (w);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO tq VALUES (1, 'b a', 'a & b'), (2, 'c', 'c | d');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM tq WHERE v = 'a b';",
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
                    query: "SELECT id FROM tq WHERE w = 'c | d';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
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
fn test_btree_replaced_comparison_functions() {
    run_scripts(&[
        ScriptTest {
            name: "an index calls its operator class's comparison function as replaced from the statement after the replacement commits",
            set_up_script: &[
                "CREATE FUNCTION my_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 < $2 THEN -1 ELSE 1 END $$;",
                "CREATE OPERATOR CLASS my_ops FOR TYPE INT4 USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 my_cmp(INT4, INT4);",
                "CREATE TABLE k (id INT PRIMARY KEY, a INT);",
                "INSERT INTO k SELECT i, i FROM generate_series(1, 10) i;",
                "CREATE UNIQUE INDEX k_a ON k (a my_ops);",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a FROM k WHERE a = 5;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION my_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT $1 / ($2 - $2) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM k WHERE a = 5;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    skip: Some("an index compares by its comparison function as its branch committed it, so the transaction that replaces the function still compares by the old one"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION my_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT $1 / ($2 - $2) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM k WHERE a = 5;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (11, 11);",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM k;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION my_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 < $2 THEN -1 ELSE 1 END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (11, 11);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM k WHERE a = 11;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "each branch's index calls its own branch's version of its operator class's comparison function",
            set_up_script: &[
                "CREATE FUNCTION my_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT CASE WHEN $1 = $2 THEN 0 WHEN $1 < $2 THEN -1 ELSE 1 END $$;",
                "CREATE OPERATOR CLASS my_ops FOR TYPE INT4 USING btree AS OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >, FUNCTION 1 my_cmp(INT4, INT4);",
                "CREATE TABLE k (id INT PRIMARY KEY, a INT);",
                "INSERT INTO k SELECT i, i FROM generate_series(1, 10) i;",
                "CREATE UNIQUE INDEX k_a ON k (a my_ops);",
                "SET enable_seqscan = off;",
                "SET enable_bitmapscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT length(dolt_commit('-Am', 'k')::text) > 0;",
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
                    query: "SELECT dolt_checkout('-b', 'other');",
                    expected: Expected::Rows { columns: &[Column("dolt_checkout", RECORD)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION my_cmp(INT4, INT4) RETURNS INT LANGUAGE sql IMMUTABLE AS $$ SELECT $1 / ($2 - $2) $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (11, 11);",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Rows { columns: &[Column("dolt_checkout", RECORD)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO k VALUES (11, 11);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM k WHERE a = 11;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("11")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('other');",
                    expected: Expected::Rows { columns: &[Column("dolt_checkout", RECORD)], rows: &[&[Any]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM k WHERE a = 5;",
                    expected: Expected::Error(Diagnostic { code: "22012", message: "division by zero", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_amvalidate() {
    run_scripts(&[
        ScriptTest {
            name: "amvalidate of incomplete operator classes of each index access method",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY av_bt USING btree;",
                    expected: Expected::Tag("CREATE OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS av_bt_int FOR TYPE int4 USING btree FAMILY av_bt AS OPERATOR 1 <, OPERATOR 3 =, FUNCTION 1 btint4cmp(int4, int4);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_bt_int';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing operator(s) for types integer and integer", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR FAMILY av_bt USING btree ADD OPERATOR 1 < (int4, int8);",
                    expected: Expected::Tag("ALTER OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_bt_int';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing operator(s) for types integer and bigint", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing support function for types integer and bigint", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing operator(s) for types integer and integer", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing cross-type operator(s)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS av_bt_int2 FOR TYPE int2 USING btree FAMILY av_bt AS OPERATOR 1 <;",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_bt_int2';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing operator(s) for types smallint and smallint", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing support function for types smallint and smallint", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing operator(s) for types integer and bigint", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing support function for types integer and bigint", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing operator(s) for types integer and integer", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_bt\" of access method btree is missing cross-type operator(s)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY av_h USING hash;",
                    expected: Expected::Tag("CREATE OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS av_h_int FOR TYPE int4 USING hash FAMILY av_h AS OPERATOR 1 =;",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_h_int';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_h\" of access method hash lacks support function for operator =(integer,integer)", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_h\" of access method hash is missing cross-type operator(s)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER OPERATOR FAMILY av_h USING hash ADD FUNCTION 1 hashint4(int4), OPERATOR 1 = (int4, int8);",
                    expected: Expected::Tag("ALTER OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_h_int';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_h\" of access method hash lacks support function for operator =(integer,bigint)", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_h\" of access method hash is missing cross-type operator(s)", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY av_g USING gist;",
                    expected: Expected::Tag("CREATE OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS av_g_box FOR TYPE box USING gist FAMILY av_g AS OPERATOR 3 &&, FUNCTION 1 gist_box_consistent(internal, box, smallint, oid, internal);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_g_box';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_g_box\" of access method gist is missing support function 2", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_g_box\" of access method gist is missing support function 5", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_g_box\" of access method gist is missing support function 6", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_g_box\" of access method gist is missing support function 7", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY av_gin USING gin;",
                    expected: Expected::Tag("CREATE OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS av_gin_arr FOR TYPE int4[] USING gin FAMILY av_gin AS OPERATOR 1 &&(anyarray, anyarray), FUNCTION 2 ginarrayextract(anyarray, internal, internal);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_gin_arr';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_gin_arr\" of access method gin is missing support function 3", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_gin_arr\" of access method gin is missing support function 4 or 6", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY av_br USING brin;",
                    expected: Expected::Tag("CREATE OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS av_br_int FOR TYPE int4 USING brin FAMILY av_br AS OPERATOR 1 <, FUNCTION 1 brin_minmax_opcinfo(internal);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_br_int';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_br_int\" of access method brin is missing support function 2", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_br_int\" of access method brin is missing support function 3", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator class \"av_br_int\" of access method brin is missing support function 4", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR FAMILY av_sp USING spgist;",
                    expected: Expected::Tag("CREATE OPERATOR FAMILY"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR CLASS av_sp_pt FOR TYPE point USING spgist FAMILY av_sp AS OPERATOR 1 <<, FUNCTION 1 spg_quad_config(internal, internal);",
                    expected: Expected::Tag("CREATE OPERATOR CLASS"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'av_sp_pt';",
                    expected: Expected::Rows {
                        columns: &[Column("amvalidate", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_sp\" of access method spgist is missing support function 2 for type point", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_sp\" of access method spgist is missing support function 3 for type point", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_sp\" of access method spgist is missing support function 4 for type point", ..N }, Diagnostic { severity: "INFO", code: "42P17", message: "operator family \"av_sp\" of access method spgist is missing support function 5 for type point", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amvalidate(0);",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "cache lookup failed for operator class 0", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "amvalidate finds every built-in operator class complete",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT oid, opcname FROM pg_opclass WHERE NOT amvalidate(oid);",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID), Column("opcname", NAME)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
