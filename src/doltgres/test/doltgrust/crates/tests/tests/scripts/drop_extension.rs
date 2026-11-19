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
use harness::script::Cell::{Any, Null, Oid, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_drop_extension() {
    run_scripts(&[
        ScriptTest {
            name: "drop extension that does not exist",
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP EXTENSION IF EXISTS pg_graphql;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: r#"extension "pg_graphql" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION IF EXISTS pg_graphql, doltgrust_no_such_extension;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: r#"extension "pg_graphql" does not exist, skipping"#, ..N }, Diagnostic { code: "00000", message: r#"extension "doltgrust_no_such_extension" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION pg_graphql;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"extension "pg_graphql" does not exist"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension removes its functions",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT uuid_nil();",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_nil", UUID)],
                        rows: &[
                            &[T("00000000-0000-0000-0000-000000000000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_catalog.pg_extension WHERE extname = 'uuid-ossp';",
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
                    query: "SELECT count(*) FROM pg_catalog.pg_proc WHERE proname LIKE 'uuid_ns_%' OR proname LIKE 'uuid_generate_%' OR proname = 'uuid_nil';",
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
                    query: r#"CREATE EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_nil();",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_nil", UUID)],
                        rows: &[
                            &[T("00000000-0000-0000-0000-000000000000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension with if exists drops the extensions that exist",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION IF EXISTS pg_graphql, "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: r#"extension "pg_graphql" does not exist, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_catalog.pg_proc WHERE proname = 'uuid_nil';",
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
            name: "drop extension drops nothing when a name does not exist",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp", pg_graphql;"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"extension "pg_graphql" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_catalog.pg_proc WHERE proname = 'uuid_nil';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension created in a schema outside the search path",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp" WITH SCHEMA public;"#,
                "SELECT pg_catalog.set_config('search_path', '', false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp" CASCADE;"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_catalog.pg_proc WHERE proname = 'uuid_nil';",
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
            name: "drop extension removes its types, operators, casts, and aggregates",
            set_up_script: &[
                "CREATE EXTENSION vector;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector + '[3,4]'::vector, avg(v) FROM (VALUES ('[1,2]'::vector), ('[3,4]'::vector)) t (v);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", USER_DEFINED), Column("avg", USER_DEFINED)],
                        rows: &[
                            &[T("[4,6]"), T("[2,3]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_catalog.pg_type WHERE typname IN ('vector', '_vector', 'halfvec', '_halfvec', 'sparsevec', '_sparsevec');",
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
                    query: "SELECT '[1,2]'::vector;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "vector" does not exist"#, position: 17, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION vector;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector + '[3,4]'::vector, avg(v) FROM (VALUES ('[1,2]'::vector), ('[3,4]'::vector)) t (v);",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", USER_DEFINED), Column("avg", USER_DEFINED)],
                        rows: &[
                            &[T("[4,6]"), T("[2,3]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose types are used by a table",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
                "CREATE EXTENSION vector;",
                "CREATE TABLE items (id INT PRIMARY KEY, embedding vector(2));",
                "CREATE TABLE item_lists (id INT PRIMARY KEY, embeddings halfvec[]);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: r#"column embedding of table items depends on type vector
column embeddings of table item_lists depends on type halfvec[]"#, hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp", vector;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop desired object(s) because other objects depend on them", detail: r#"column embedding of table items depends on type vector
column embeddings of table item_lists depends on type halfvec[]"#, hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_nil();",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_nil", UUID)],
                        rows: &[
                            &[T("00000000-0000-0000-0000-000000000000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE items;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector RESTRICT;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "column embeddings of table item_lists depends on type halfvec[]", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector;",
                    expected: Expected::Rows {
                        columns: &[Column("vector", USER_DEFINED)],
                        rows: &[
                            &[T("[1,2]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE item_lists;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension cascades to the columns that use its types",
            set_up_script: &[
                "CREATE EXTENSION vector;",
                "CREATE TABLE items (id INT PRIMARY KEY, embedding vector(2));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector CASCADE;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to column embedding of table items", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name FROM information_schema.columns WHERE table_name = 'items';",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME)],
                        rows: &[
                            &[T("id")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose functions are used by a column default",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
                "CREATE TABLE goals (id uuid DEFAULT uuid_generate_v4(), other uuid DEFAULT gen_random_uuid());",
                "CREATE TABLE notes (id INT PRIMARY KEY, label text DEFAULT upper(uuid_nil()::text || 'x'));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: r#"default value for column label of table notes depends on function uuid_nil()
default value for column id of table goals depends on function uuid_generate_v4()"#, hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE goals ALTER COLUMN id DROP DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "default value for column label of table notes depends on function uuid_nil()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE notes ALTER COLUMN label DROP DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension cascades to the column defaults that use its functions",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
                "CREATE TABLE goals (id uuid DEFAULT uuid_generate_v4());",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp" CASCADE;"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to default value for column id of table goals", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_default FROM information_schema.columns WHERE table_name = 'goals';",
                    expected: Expected::Rows {
                        columns: &[Column("column_default", VARCHAR)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose functions are used by a generated column",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
                "CREATE TABLE gen (id INT PRIMARY KEY, nil_text text GENERATED ALWAYS AS (uuid_nil()::text) STORED);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "column nil_text of table gen depends on function uuid_nil()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE gen;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose functions are used by a view",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
                "CREATE VIEW plain_view AS SELECT gen_random_uuid() AS n;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW nil_view AS SELECT uuid_nil() AS n;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "view nil_view depends on function uuid_nil()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW nil_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW ns_view AS SELECT x.n FROM (SELECT uuid_ns_dns() AS n) x UNION SELECT gen_random_uuid();",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "view ns_view depends on function uuid_ns_dns()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW ns_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose functions are used by a CHECK constraint",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
                "CREATE TABLE checked (t text CHECK (t <> uuid_nil()::text));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "constraint checked_t_check on table checked depends on function uuid_nil()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE checked;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose types are used by casts",
            set_up_script: &[
                "CREATE EXTENSION vector;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE checked (t text CHECK (t::vector IS NOT NULL));",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "constraint checked_t_check on table checked depends on type vector", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE checked;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE defaulted (t text DEFAULT ('[1,2]'::vector)::text);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "default value for column t of table defaulted depends on type vector", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE defaulted;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW cast_view AS SELECT '[1,2]'::vector AS v;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "view cast_view depends on type vector", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW cast_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW array_view AS SELECT '{}'::halfvec[] AS v;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "view array_view depends on type halfvec[]", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW array_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose functions are used by a domain",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DOMAIN defaulted_uuid AS uuid DEFAULT uuid_nil();",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "type defaulted_uuid depends on function uuid_nil()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DOMAIN defaulted_uuid;",
                    expected: Expected::Tag("DROP DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN checked_uuid AS uuid CHECK (VALUE <> uuid_nil());",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "constraint checked_uuid_check depends on function uuid_nil()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DOMAIN checked_uuid;",
                    expected: Expected::Tag("DROP DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose types are used by a domain",
            set_up_script: &[
                "CREATE EXTENSION vector;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE DOMAIN based AS vector;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "type based depends on type vector", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DOMAIN based;",
                    expected: Expected::Tag("DROP DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN casted AS text DEFAULT ('[1,2]'::vector)::text;",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "type casted depends on type vector", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DOMAIN casted;",
                    expected: Expected::Tag("DROP DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DOMAIN casted_check AS text CHECK (VALUE::vector IS NOT NULL);",
                    expected: Expected::Tag("CREATE DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "constraint casted_check_check depends on type vector", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP DOMAIN casted_check;",
                    expected: Expected::Tag("DROP DOMAIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop extension whose functions are used by window functions and unions",
            set_up_script: &[
                r#"CREATE EXTENSION "uuid-ossp";"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE VIEW window_view AS SELECT row_number() OVER (ORDER BY uuid_nil()) AS n FROM (VALUES (1)) t (i);",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "view window_view depends on function uuid_nil()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW window_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW named_window_view AS SELECT row_number() OVER w AS n FROM (VALUES (1)) t (i) WINDOW w AS (PARTITION BY uuid_ns_url());",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "view named_window_view depends on function uuid_ns_url()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW named_window_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW union_view AS SELECT gen_random_uuid() AS n UNION ALL SELECT uuid_ns_oid();",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension uuid-ossp because other objects depend on it", detail: "view union_view depends on function uuid_ns_oid()", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW union_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_built_in_objects() {
    run_scripts(&[
        ScriptTest {
            name: "a superuser drops the built-in plpgsql extension and makes it again",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION f() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN RETURN 1; END $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION plpgsql;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop extension plpgsql because other objects depend on it"#, detail: r#"function f() depends on language plpgsql"#, hint: r#"Use DROP ... CASCADE to drop the dependent objects too."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION plpgsql RESTRICT;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop extension plpgsql because other objects depend on it"#, detail: r#"function f() depends on language plpgsql"#, hint: r#"Use DROP ... CASCADE to drop the dependent objects too."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP LANGUAGE plpgsql;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop language plpgsql because extension plpgsql requires it"#, hint: r#"You can drop extension plpgsql instead."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP FUNCTION plpgsql_validator(oid);"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop function plpgsql_validator(oid) because extension plpgsql requires it"#, hint: r#"You can drop extension plpgsql instead."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"BEGIN;"#,
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION plpgsql CASCADE;"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: r#"drop cascades to function f()"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_language WHERE lanname = 'plpgsql';"#,
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
                    query: r#"ROLLBACK;"#,
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT f();"#,
                    expected: Expected::Rows {
                        columns: &[Column("f", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT obj_description(oid, 'pg_language') FROM pg_language WHERE lanname = 'plpgsql';"#,
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("PL/pgSQL procedural language")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER EXTENSION plpgsql DROP FUNCTION plpgsql_inline_handler(internal);"#,
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pg_describe_object(classid, objid, objsubid) FROM pg_depend WHERE refclassid = 'pg_extension'::regclass ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("function plpgsql_call_handler()")],
                            &[T("function plpgsql_validator(oid)")],
                            &[T("language plpgsql")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION plpgsql CASCADE;"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: r#"drop cascades to function f()"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_language WHERE lanname = 'plpgsql';"#,
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
                    query: r#"SELECT proname FROM pg_proc WHERE proname LIKE 'plpgsql%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME)],
                        rows: &[
                            &[T("plpgsql_inline_handler")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DO $$ BEGIN END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "plpgsql" does not exist"#, hint: r#"Use CREATE EXTENSION to load the language into the database."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION g() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN RETURN 1; END $$;"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"language "plpgsql" does not exist"#, hint: r#"Use CREATE EXTENSION to load the language into the database."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP FUNCTION plpgsql_inline_handler(internal);"#,
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION plpgsql;"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT lanname, oid >= 16384 AS new FROM pg_language WHERE lanname = 'plpgsql';"#,
                    expected: Expected::Rows {
                        columns: &[Column("lanname", NAME), Column("new", BOOL)],
                        rows: &[
                            &[T("plpgsql"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT proname, oid >= 16384 AS new FROM pg_proc WHERE proname LIKE 'plpgsql%' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("new", BOOL)],
                        rows: &[
                            &[T("plpgsql_call_handler"), T("t")],
                            &[T("plpgsql_inline_handler"), T("t")],
                            &[T("plpgsql_validator"), T("t")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_depend WHERE refobjid = (SELECT oid FROM pg_extension WHERE extname = 'plpgsql');"#,
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
                    query: r#"DO $$ BEGIN RAISE NOTICE 'hi'; END $$;"#,
                    expected: Expected::Tag("DO"),
                    notices: &[Diagnostic { code: "00000", message: r#"hi"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE FUNCTION g() RETURNS INT LANGUAGE plpgsql AS $$ BEGIN RETURN 2; END $$;"#,
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT g();"#,
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4)],
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
        ScriptTest {
            name: "a superuser drops built-in schemas, text search objects, and collations, but not pinned objects",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP SCHEMA public;"#,
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT nspname FROM pg_namespace ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME)],
                        rows: &[
                            &[T("information_schema")],
                            &[T("pg_catalog")],
                            &[T("pg_toast")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE SCHEMA public;"#,
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT nspname, oid >= 16384 AS new FROM pg_namespace WHERE nspname = 'public';"#,
                    expected: Expected::Rows {
                        columns: &[Column("nspname", NAME), Column("new", BOOL)],
                        rows: &[
                            &[T("public"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TEXT SEARCH CONFIGURATION english;"#,
                    expected: Expected::Tag("DROP TEXT SEARCH CONFIGURATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT to_tsvector('english', 'cats');"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"text search configuration "english" does not exist"#, position: 20, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TEXT SEARCH DICTIONARY english_stem;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop text search dictionary english_stem because other objects depend on it"#, detail: r#"text search configuration hindi depends on text search dictionary english_stem
text search configuration russian depends on text search dictionary english_stem"#, hint: r#"Use DROP ... CASCADE to drop the dependent objects too."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TEXT SEARCH DICTIONARY french_stem CASCADE;"#,
                    expected: Expected::Tag("DROP TEXT SEARCH DICTIONARY"),
                    notices: &[Diagnostic { code: "00000", message: r#"drop cascades to text search configuration french"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_ts_config WHERE cfgname IN ('english', 'french');"#,
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
                    query: r#"DROP COLLATION "en-x-icu";"#,
                    expected: Expected::Tag("DROP COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_collation WHERE collname = 'en-x-icu';"#,
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
                    query: r#"CREATE COLLATION "en-x-icu" (provider = icu, locale = 'en');"#,
                    expected: Expected::Tag("CREATE COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP FUNCTION now();"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop function now() because it is required by the database system"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP COLLATION "C";"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop collation "C" because it is required by the database system"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "dropping a built-in object refuses while built-in objects depend on it",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP FUNCTION information_schema._pg_truetypid(pg_attribute, pg_type) RESTRICT;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop function information_schema._pg_truetypid(pg_attribute,pg_type) because other objects depend on it"#, detail: r#"view information_schema.attributes depends on function information_schema._pg_truetypid(pg_attribute,pg_type)
view information_schema.columns depends on function information_schema._pg_truetypid(pg_attribute,pg_type)
view information_schema.data_type_privileges depends on view information_schema.columns
view information_schema.element_types depends on view information_schema.data_type_privileges"#, hint: r#"Use DROP ... CASCADE to drop the dependent objects too."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP DOMAIN information_schema.cardinal_number;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop type information_schema.cardinal_number because other objects depend on it"#, detail: r#"view information_schema.attributes depends on type information_schema.cardinal_number
view information_schema.columns depends on type information_schema.cardinal_number
view information_schema.domains depends on type information_schema.cardinal_number
view information_schema.key_column_usage depends on type information_schema.cardinal_number
view information_schema.parameters depends on type information_schema.cardinal_number
view information_schema.routines depends on type information_schema.cardinal_number
view information_schema.data_type_privileges depends on view information_schema.routines
view information_schema.sequences depends on type information_schema.cardinal_number
column integer_value of table information_schema.sql_implementation_info depends on type information_schema.cardinal_number
column supported_value of table information_schema.sql_sizing depends on type information_schema.cardinal_number
column sizing_id of table information_schema.sql_sizing depends on type information_schema.cardinal_number
view information_schema.triggers depends on type information_schema.cardinal_number
view information_schema.user_defined_types depends on type information_schema.cardinal_number
view information_schema.element_types depends on type information_schema.cardinal_number"#, hint: r#"Use DROP ... CASCADE to drop the dependent objects too."#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_built_in_relations() {
    run_scripts(&[
        ScriptTest {
            name: "a superuser drops built-in views, which take their columns, rules, row types, comments, and privileges with them",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT 'pg_catalog.pg_tables'::regclass::oid;"#,
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID)],
                        rows: &[
                            &[T("12033")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE ROLE r1;"#,
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"GRANT SELECT (tablename) ON pg_catalog.pg_tables TO r1;"#,
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"COMMENT ON COLUMN pg_catalog.pg_tables.tablename IS 'tn';"#,
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP VIEW pg_catalog.pg_tables;"#,
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM pg_tables;"#,
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "pg_tables" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT (SELECT count(*) FROM pg_attribute WHERE attrelid = 12033) + (SELECT count(*) FROM pg_description WHERE objoid = 12033) + (SELECT count(*) FROM pg_init_privs WHERE objoid = 12033) + (SELECT count(*) FROM pg_depend WHERE objid = 12033 OR refobjid = 12033) + (SELECT count(*) FROM pg_type WHERE typrelid = 12033) + (SELECT count(*) FROM pg_rewrite WHERE ev_class = 12033) AS rows;"#,
                    expected: Expected::Rows {
                        columns: &[Column("rows", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE VIEW pg_catalog.pg_tables AS SELECT 1 AS x;"#,
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"permission denied to create "pg_catalog.pg_tables""#, detail: r#"System catalog modifications are currently disallowed."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE VIEW public.pg_tables AS SELECT 1 AS a;"#,
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM pg_tables;"#,
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP VIEW pg_roles;"#,
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a superuser drops information_schema's views, tables, domains, and the schema itself",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"DROP VIEW information_schema.tables RESTRICT;"#,
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP TABLE information_schema.sql_parts;"#,
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_class WHERE relname IN ('sql_parts', 'pg_toast_13892', 'pg_toast_13892_index');"#,
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
                    query: r#"DROP DOMAIN information_schema.yes_or_no CASCADE;"#,
                    expected: Expected::Tag("DROP DOMAIN"),
                    notices: &[Diagnostic { code: "00000", message: r#"drop cascades to 24 other objects"#, detail: r#"drop cascades to view information_schema.applicable_roles
drop cascades to view information_schema.administrable_role_authorizations
drop cascades to view information_schema.attributes
drop cascades to view information_schema.column_privileges
drop cascades to view information_schema.role_column_grants
drop cascades to view information_schema.columns
drop cascades to view information_schema.domain_constraints
drop cascades to view information_schema.parameters
drop cascades to view information_schema.routine_privileges
drop cascades to view information_schema.role_routine_grants
drop cascades to view information_schema.routines
drop cascades to view information_schema.data_type_privileges
drop cascades to view information_schema.element_types
drop cascades to view information_schema.sequences
drop cascades to column is_supported of table information_schema.sql_features
drop cascades to view information_schema.table_constraints
drop cascades to view information_schema.table_privileges
drop cascades to view information_schema.role_table_grants
drop cascades to view information_schema.udt_privileges
drop cascades to view information_schema.role_udt_grants
drop cascades to view information_schema.usage_privileges
drop cascades to view information_schema.role_usage_grants
drop cascades to view information_schema.user_defined_types
drop cascades to view information_schema.views"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT attname, attisdropped FROM pg_attribute WHERE attrelid = 'information_schema.sql_features'::regclass AND attnum > 0 ORDER BY attnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("attisdropped", BOOL)],
                        rows: &[
                            &[T("feature_id"), T("f")],
                            &[T("feature_name"), T("f")],
                            &[T("sub_feature_id"), T("f")],
                            &[T("sub_feature_name"), T("f")],
                            &[T("........pg.dropped.5........"), T("t")],
                            &[T("is_verified_by"), T("f")],
                            &[T("comments"), T("f")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT feature_id, sub_feature_id FROM information_schema.sql_features WHERE feature_id = 'B011';"#,
                    expected: Expected::Rows {
                        columns: &[Column("feature_id", VARCHAR), Column("sub_feature_id", VARCHAR)],
                        rows: &[
                            &[T("B011"), T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP SCHEMA information_schema;"#,
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"cannot drop schema information_schema because other objects depend on it"#, detail: r#"function information_schema._pg_expandarray(anyarray) depends on schema information_schema
function information_schema._pg_index_position(oid,smallint) depends on schema information_schema
function information_schema._pg_truetypid(pg_attribute,pg_type) depends on schema information_schema
function information_schema._pg_truetypmod(pg_attribute,pg_type) depends on schema information_schema
function information_schema._pg_char_max_length(oid,integer) depends on schema information_schema
function information_schema._pg_char_octet_length(oid,integer) depends on schema information_schema
function information_schema._pg_numeric_precision(oid,integer) depends on schema information_schema
function information_schema._pg_numeric_precision_radix(oid,integer) depends on schema information_schema
function information_schema._pg_numeric_scale(oid,integer) depends on schema information_schema
function information_schema._pg_datetime_precision(oid,integer) depends on schema information_schema
function information_schema._pg_interval_type(oid,integer) depends on schema information_schema
type information_schema.cardinal_number depends on schema information_schema
type information_schema.character_data depends on schema information_schema
type information_schema.sql_identifier depends on schema information_schema
view information_schema.information_schema_catalog_name depends on schema information_schema
type information_schema.time_stamp depends on schema information_schema
view information_schema.character_sets depends on schema information_schema
view information_schema.check_constraint_routine_usage depends on schema information_schema
view information_schema.check_constraints depends on schema information_schema
view information_schema.collations depends on schema information_schema
view information_schema.collation_character_set_applicability depends on schema information_schema
view information_schema.column_column_usage depends on schema information_schema
view information_schema.column_domain_usage depends on schema information_schema
view information_schema.column_udt_usage depends on schema information_schema
view information_schema.constraint_column_usage depends on schema information_schema
view information_schema.constraint_table_usage depends on schema information_schema
view information_schema.domain_udt_usage depends on schema information_schema
view information_schema.domains depends on schema information_schema
view information_schema.enabled_roles depends on schema information_schema
view information_schema.key_column_usage depends on schema information_schema
view information_schema.referential_constraints depends on schema information_schema
view information_schema.routine_column_usage depends on schema information_schema
view information_schema.routine_routine_usage depends on schema information_schema
view information_schema.routine_sequence_usage depends on schema information_schema
view information_schema.routine_table_usage depends on schema information_schema
view information_schema.schemata depends on schema information_schema
table information_schema.sql_features depends on schema information_schema
table information_schema.sql_implementation_info depends on schema information_schema
table information_schema.sql_sizing depends on schema information_schema
view information_schema.transforms depends on schema information_schema
view information_schema.triggered_update_columns depends on schema information_schema
view information_schema.triggers depends on schema information_schema
view information_schema.view_column_usage depends on schema information_schema
view information_schema.view_routine_usage depends on schema information_schema
view information_schema.view_table_usage depends on schema information_schema
view information_schema._pg_foreign_table_columns depends on schema information_schema
view information_schema.column_options depends on schema information_schema
view information_schema._pg_foreign_data_wrappers depends on schema information_schema
view information_schema.foreign_data_wrapper_options depends on schema information_schema
view information_schema.foreign_data_wrappers depends on schema information_schema
view information_schema._pg_foreign_servers depends on schema information_schema
view information_schema.foreign_server_options depends on schema information_schema
view information_schema.foreign_servers depends on schema information_schema
view information_schema._pg_foreign_tables depends on schema information_schema
view information_schema.foreign_table_options depends on schema information_schema
view information_schema.foreign_tables depends on schema information_schema
view information_schema._pg_user_mappings depends on schema information_schema
view information_schema.user_mapping_options depends on schema information_schema
view information_schema.user_mappings depends on schema information_schema"#, hint: r#"Use DROP ... CASCADE to drop the dependent objects too."#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP SCHEMA information_schema CASCADE;"#,
                    expected: Expected::Tag("DROP SCHEMA"),
                    notices: &[Diagnostic { code: "00000", message: r#"drop cascades to 59 other objects"#, detail: r#"drop cascades to function information_schema._pg_expandarray(anyarray)
drop cascades to function information_schema._pg_index_position(oid,smallint)
drop cascades to function information_schema._pg_truetypid(pg_attribute,pg_type)
drop cascades to function information_schema._pg_truetypmod(pg_attribute,pg_type)
drop cascades to function information_schema._pg_char_max_length(oid,integer)
drop cascades to function information_schema._pg_char_octet_length(oid,integer)
drop cascades to function information_schema._pg_numeric_precision(oid,integer)
drop cascades to function information_schema._pg_numeric_precision_radix(oid,integer)
drop cascades to function information_schema._pg_numeric_scale(oid,integer)
drop cascades to function information_schema._pg_datetime_precision(oid,integer)
drop cascades to function information_schema._pg_interval_type(oid,integer)
drop cascades to type information_schema.cardinal_number
drop cascades to type information_schema.character_data
drop cascades to type information_schema.sql_identifier
drop cascades to view information_schema.information_schema_catalog_name
drop cascades to type information_schema.time_stamp
drop cascades to view information_schema.character_sets
drop cascades to view information_schema.check_constraint_routine_usage
drop cascades to view information_schema.check_constraints
drop cascades to view information_schema.collations
drop cascades to view information_schema.collation_character_set_applicability
drop cascades to view information_schema.column_column_usage
drop cascades to view information_schema.column_domain_usage
drop cascades to view information_schema.column_udt_usage
drop cascades to view information_schema.constraint_column_usage
drop cascades to view information_schema.constraint_table_usage
drop cascades to view information_schema.domain_udt_usage
drop cascades to view information_schema.domains
drop cascades to view information_schema.enabled_roles
drop cascades to view information_schema.key_column_usage
drop cascades to view information_schema.referential_constraints
drop cascades to view information_schema.routine_column_usage
drop cascades to view information_schema.routine_routine_usage
drop cascades to view information_schema.routine_sequence_usage
drop cascades to view information_schema.routine_table_usage
drop cascades to view information_schema.schemata
drop cascades to table information_schema.sql_features
drop cascades to table information_schema.sql_implementation_info
drop cascades to table information_schema.sql_sizing
drop cascades to view information_schema.transforms
drop cascades to view information_schema.triggered_update_columns
drop cascades to view information_schema.triggers
drop cascades to view information_schema.view_column_usage
drop cascades to view information_schema.view_routine_usage
drop cascades to view information_schema.view_table_usage
drop cascades to view information_schema._pg_foreign_table_columns
drop cascades to view information_schema.column_options
drop cascades to view information_schema._pg_foreign_data_wrappers
drop cascades to view information_schema.foreign_data_wrapper_options
drop cascades to view information_schema.foreign_data_wrappers
drop cascades to view information_schema._pg_foreign_servers
drop cascades to view information_schema.foreign_server_options
drop cascades to view information_schema.foreign_servers
drop cascades to view information_schema._pg_foreign_tables
drop cascades to view information_schema.foreign_table_options
drop cascades to view information_schema.foreign_tables
drop cascades to view information_schema._pg_user_mappings
drop cascades to view information_schema.user_mapping_options
drop cascades to view information_schema.user_mappings"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE nspname = 'information_schema';"#,
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
                    query: r#"CREATE SCHEMA information_schema;"#,
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
