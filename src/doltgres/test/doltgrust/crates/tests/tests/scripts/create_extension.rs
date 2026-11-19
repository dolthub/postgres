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
fn test_create_extension() {
    run_scripts(&[
        ScriptTest {
            name: "create extension uuid-ossp after setting search_path to empty",
            set_up_script: &[
                "SELECT pg_catalog.set_config('search_path', '', false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_nil();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function uuid_nil() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.uuid_nil();",
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
            name: "alter table with default expr using extension function when search_path is empty",
            set_up_script: &[
                "SELECT pg_catalog.set_config('search_path', '', false);",
                r#"CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;"#,
                r#"CREATE TABLE public.goals (
    id uuid DEFAULT public.uuid_generate_v4() NOT NULL,
    note_id uuid,
    completion_timestamp timestamp without time zone,
    due_date timestamp without time zone
);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE ONLY public.goals ADD CONSTRAINT goals_pkey PRIMARY KEY (id);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "uuid-ossp is not available before it is created",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_catalog.pg_extension;",
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
                    query: "SELECT name, installed_version FROM pg_catalog.pg_available_extensions WHERE name = 'uuid-ossp';",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME), Column("installed_version", TEXT)],
                        rows: &[
                            &[T("uuid-ossp"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_nil();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function uuid_nil() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
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
            name: "only emulated extensions may be created",
            assertions: &[
                // Doltgrust extension: Postgres also names the missing control file, which Doltgrust does not have.
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "doltgrust_no_such_extension";"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"extension "doltgrust_no_such_extension" is not available"#, ..E }),
                    ..A
                },
                // Doltgrust extension: Postgres also names the missing control file, which Doltgrust does not have.
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION IF NOT EXISTS "doltgrust_no_such_extension";"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"extension "doltgrust_no_such_extension" is not available"#, ..E }),
                    ..A
                },
                // Changed from the recording: a case-insensitive file system let Postgres open uuid-ossp's control file for
                // "UUID-OSSP", which extension names otherwise never match.
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "UUID-OSSP";"#,
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"extension "UUID-OSSP" is not available"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    flow: Flow::Exec,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"extension "uuid-ossp" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION IF NOT EXISTS "uuid-ossp";"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    notices: &[Diagnostic { code: "42710", message: r#"extension "uuid-ossp" already exists, skipping"#, ..N }],
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
            name: "uuid-ossp options that are not yet supported",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "uuid-ossp" VERSION oldversion;"#,
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"extension "uuid-ossp" has no installation script nor update path for version "oldversion""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "uuid-ossp" CASCADE;"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    flow: Flow::Exec,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "uuid-ossp installation participates in branches",
            set_up_script: &[
                "SELECT dolt_commit('--allow-empty', '-m', 'initial commit');",
                "SELECT dolt_checkout('-b', 'ext');",
                r#"CREATE EXTENSION "uuid-ossp";"#,
                "SELECT dolt_commit('-Am', 'create the extension');",
            ],
            assertions: &[
                // Doltgrust extension: dolt_checkout and dolt_merge change which extensions exist, and Postgres always lists plpgsql.
                ScriptTestAssertion {
                    query: "SELECT extname FROM pg_catalog.pg_extension;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME)],
                        rows: &[
                            &[T("plpgsql")],
                            &[T("uuid-ossp")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_checkout('main');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust extension: dolt_checkout and dolt_merge change which extensions exist, and Postgres always lists plpgsql.
                ScriptTestAssertion {
                    query: "SELECT extname FROM pg_catalog.pg_extension;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME)],
                        rows: &[
                            &[T("plpgsql")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust extension: dolt_checkout changed which functions exist, and Postgres reports the missing function.
                ScriptTestAssertion {
                    query: "SELECT uuid_nil();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function uuid_nil() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT dolt_merge('ext');",
                    expected: Expected::Tag("SELECT 1"),
                    flow: Flow::Exec,
                    ..A
                },
                // Doltgrust extension: dolt_checkout and dolt_merge change which extensions exist, and Postgres always lists plpgsql.
                ScriptTestAssertion {
                    query: "SELECT extname, extversion FROM pg_catalog.pg_extension;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("extversion", TEXT)],
                        rows: &[
                            &[T("plpgsql"), T("1.0")],
                            &[T("uuid-ossp"), T("1.1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
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
            name: "uuid-ossp in a non-public schema",
            set_up_script: &[
                "CREATE SCHEMA extensions;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "uuid-ossp" WITH SCHEMA nosuchschema;"#,
                    expected: Expected::Error(Diagnostic { code: "3F000", message: r#"schema "nosuchschema" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA extensions;"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    notices: &[Diagnostic { code: "42710", message: r#"extension "uuid-ossp" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extensions.uuid_nil();",
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
                    query: "SELECT uuid_nil();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function uuid_nil() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT public.uuid_nil();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.uuid_nil() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.extname, n.nspname FROM pg_catalog.pg_extension e JOIN pg_catalog.pg_namespace n ON e.extnamespace = n.oid;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("nspname", NAME)],
                        rows: &[
                            &[T("plpgsql"), T("pg_catalog")],
                            &[T("uuid-ossp"), T("extensions")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE goals (id UUID DEFAULT extensions.uuid_generate_v4() PRIMARY KEY, note TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO goals (note) VALUES ('first');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA extensions;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop schema extensions because other objects depend on it", detail: r#"extension uuid-ossp depends on schema extensions
default value for column id of table goals depends on function extensions.uuid_generate_v4()"#, hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = public, extensions;",
                    expected: Expected::Tag("SET"),
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
                    query: "INSERT INTO goals (note) VALUES ('second');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT note, length(id::text) FROM goals;",
                    expected: Expected::Rows {
                        columns: &[Column("note", TEXT), Column("length", INT4)],
                        rows: &[
                            &[T("first"), T("36")],
                            &[T("second"), T("36")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE goals;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extensions.uuid_nil();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function extensions.uuid_nil() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA extensions;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "vector in a non-public schema",
            set_up_script: &[
                "CREATE SCHEMA extensions;",
                "CREATE EXTENSION vector WITH SCHEMA extensions;",
                "CREATE TABLE items (id INT PRIMARY KEY, embedding extensions.vector(3));",
                "INSERT INTO items VALUES (1, '[1,2,3]'), (2, '[4,5,6]');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT '[1,2,3]'::vector;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"type "vector" does not exist"#, position: 19, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2,3]'::extensions.vector <-> '[1,2,4]'::extensions.vector;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: extensions.vector <-> extensions.vector", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 37, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2,3]'::extensions.vector OPERATOR(extensions.<->) '[1,2,4]'::extensions.vector;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", FLOAT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extensions.l2_distance('[1,2,3]'::extensions.vector, '[1,2,4]'::extensions.vector);",
                    expected: Expected::Rows {
                        columns: &[Column("l2_distance", FLOAT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path = public, extensions;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM items ORDER BY embedding <-> '[3,1,2]' LIMIT 1;",
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
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: "column embedding of table items depends on type vector", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_extension_rules() {
    run_scripts(&[
        ScriptTest {
            name: "uuid-ossp functions",
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_nil(), uuid_ns_dns(), uuid_ns_url(), uuid_ns_oid(), uuid_ns_x500();",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_nil", UUID), Column("uuid_ns_dns", UUID), Column("uuid_ns_url", UUID), Column("uuid_ns_oid", UUID), Column("uuid_ns_x500", UUID)],
                        rows: &[
                            &[T("00000000-0000-0000-0000-000000000000"), T("6ba7b810-9dad-11d1-80b4-00c04fd430c8"), T("6ba7b811-9dad-11d1-80b4-00c04fd430c8"), T("6ba7b812-9dad-11d1-80b4-00c04fd430c8"), T("6ba7b814-9dad-11d1-80b4-00c04fd430c8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_generate_v3(uuid_ns_dns(), 'www.example.com'), uuid_generate_v5(uuid_ns_url(), 'http://www.postgresql.org');",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_generate_v3", UUID), Column("uuid_generate_v5", UUID)],
                        rows: &[
                            &[T("5df41881-3aed-3515-88a7-2f4a814cf09e"), T("e1ee1ad4-cd4e-5889-962a-4f605a68d94e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT substring(uuid_generate_v4()::text, 15, 1), substring(uuid_generate_v1()::text, 15, 1), substring(uuid_generate_v1mc()::text, 26, 1) IN ('3', '7', 'b', 'f');",
                    expected: Expected::Rows {
                        columns: &[Column("substring", TEXT), Column("substring", TEXT), Column("?column?", BOOL)],
                        rows: &[
                            &[T("4"), T("1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_generate_v3(NULL, 'x') IS NULL;",
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
                    query: r#"CREATE EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"extension "uuid-ossp" already exists"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION IF NOT EXISTS "uuid-ossp";"#,
                    expected: Expected::Tag("CREATE EXTENSION"),
                    notices: &[Diagnostic { code: "42710", message: r#"extension "uuid-ossp" already exists, skipping"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE EXTENSION "uuid-ossp" VERSION '9.9';"#,
                    expected: Expected::Error(Diagnostic { code: "42710", message: r#"extension "uuid-ossp" already exists"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                // Doltgrust extension: Postgres also names the missing control file, which Doltgrust does not have.
                ScriptTestAssertion {
                    query: "CREATE EXTENSION no_such_extension_here;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: r#"extension "no_such_extension_here" is not available"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extname, extversion, extrelocatable FROM pg_extension ORDER BY extname;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("extversion", TEXT), Column("extrelocatable", BOOL)],
                        rows: &[
                            &[T("plpgsql"), T("1.0"), T("f")],
                            &[T("uuid-ossp"), T("1.1"), T("t")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_nil();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function uuid_nil() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION "uuid-ossp";"#,
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"extension "uuid-ossp" does not exist"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP EXTENSION IF EXISTS "uuid-ossp";"#,
                    expected: Expected::Tag("DROP EXTENSION"),
                    notices: &[Diagnostic { code: "00000", message: r#"extension "uuid-ossp" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "vector input and output",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION vector;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1000000, 0.0000001, 1.5e20, -0, 1e-45]'::vector, '[1e-50]'::vector, '[ 1 , 2 ]'::vector;",
                    expected: Expected::Rows {
                        columns: &[Column("vector", USER_DEFINED), Column("vector", USER_DEFINED), Column("vector", USER_DEFINED)],
                        rows: &[
                            &[T("[1e+06,1e-07,1.5e+20,-0,1e-45]"), T("[0]"), T("[1,2]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1e39]'::vector;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: r#""1e39" is out of range for type vector"#, position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[nan]'::vector;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "NaN not allowed in vector", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2'::vector;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type vector: "[1,2""#, position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '1,2'::vector;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type vector: "1,2""#, detail: r#"Vector contents must start with "["."#, position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2] x'::vector;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type vector: "[1,2] x""#, detail: "Junk after closing right brace.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[]'::vector;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "vector must have at least 1 dimension", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::halfvec, '[65519]'::halfvec, '[1.0001, 0.00001]'::halfvec;",
                    expected: Expected::Rows {
                        columns: &[Column("halfvec", USER_DEFINED), Column("halfvec", USER_DEFINED), Column("halfvec", USER_DEFINED)],
                        rows: &[
                            &[T("[1,2]"), T("[65504]"), T("[1,1.001358e-05]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[65520]'::halfvec;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: r#""65520" is out of range for type halfvec"#, position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1:0,3:2}/3'::sparsevec, '{}/2'::sparsevec, ' { 3 : 1 , 1:2 } / 4 '::sparsevec;",
                    expected: Expected::Rows {
                        columns: &[Column("sparsevec", USER_DEFINED), Column("sparsevec", USER_DEFINED), Column("sparsevec", USER_DEFINED)],
                        rows: &[
                            &[T("{3:2}/3"), T("{}/2"), T("{1:2,3:1}/4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1:1}/3 x'::sparsevec;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type sparsevec: "{1:1}/3 x""#, detail: "Junk after closing.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{4:1}/3'::sparsevec;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "sparsevec index out of bounds", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1:1,1:2}/3'::sparsevec;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "sparsevec indices must not contain duplicates", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1:1}/0'::sparsevec;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "sparsevec must have at least 1 dimension", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1:1e-50}/3'::sparsevec;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: r#""1e-50" is out of range for type sparsevec"#, position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{}'::sparsevec;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type sparsevec: "{}""#, detail: "Unexpected end of input.", position: 8, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector(3);",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "expected 3 dimensions, not 2", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector(2)::vector(3);",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "expected 3 dimensions, not 2", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector(0);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "dimensions for type vector must be at least 1", position: 17, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector(1,2);",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid type modifier", position: 17, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector('a');",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: r#"invalid input syntax for type integer: "a""#, position: 17, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof('[1,2]'::vector), format_type('vector'::regtype, 3), format_type('_vector'::regtype, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE), Column("format_type", TEXT), Column("format_type", TEXT)],
                        rows: &[
                            &[T("vector"), T("vector(3)"), T("vector(3)[]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "vector functions and operators",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION vector;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT vector_dims('[1,2,3]'::vector), vector_norm('[3,4]'), l2_normalize('[3,4]'::vector), l2_normalize('[0,0]'::vector);",
                    expected: Expected::Rows {
                        columns: &[Column("vector_dims", INT4), Column("vector_norm", FLOAT8), Column("l2_normalize", USER_DEFINED), Column("l2_normalize", USER_DEFINED)],
                        rows: &[
                            &[T("3"), T("5"), T("[0.6,0.8]"), T("[0,0]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector <-> '[3,4]', '[1,2]'::vector <#> '[3,4]', '[1,2]'::vector <=> '[3,4]', '[1,2]'::vector <+> '[3,4]';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", FLOAT8), Column("?column?", FLOAT8), Column("?column?", FLOAT8), Column("?column?", FLOAT8)],
                        rows: &[
                            &[T("2.8284271247461903"), T("-11"), T("0.01613008990009257"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector + '[3,4]', '[1,2]'::vector - '[3,4]', '[1,2]'::vector * '[3,4]', '[1,2]'::vector || '[3]';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", USER_DEFINED), Column("?column?", USER_DEFINED), Column("?column?", USER_DEFINED), Column("?column?", USER_DEFINED)],
                        rows: &[
                            &[T("[4,6]"), T("[-2,-2]"), T("[3,8]"), T("[1,2,3]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector + '[3]';",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "different vector dimensions 2 and 1", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1e38]'::vector * '[10]';",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "value out of range: overflow", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1e-30]'::vector * '[1e-30]';",
                    expected: Expected::Error(Diagnostic { code: "22003", message: "value out of range: underflow", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector < '[1,2,3]', '[1,2]'::vector = '[1,2]', '[2]'::vector > '[1,9]', '{1:1}/2'::sparsevec < '{2:1}/2', '{1:-1}/2'::sparsevec < '{2:1}/2';",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL), Column("?column?", BOOL)],
                        rows: &[
                            &[T("t"), T("t"), T("t"), T("f"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT subvector('[1,2,3,4]'::vector, 2, 2), subvector('[1,2,3]'::vector, 0, 2), subvector('[1,2,3]'::vector, 3, 5);",
                    expected: Expected::Rows {
                        columns: &[Column("subvector", USER_DEFINED), Column("subvector", USER_DEFINED), Column("subvector", USER_DEFINED)],
                        rows: &[
                            &[T("[2,3]"), T("[1]"), T("[3]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT subvector('[1,2,3]'::vector, 4, 1);",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "vector must have at least 1 dimension", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT binary_quantize('[1,-1,0,2]'::vector), hamming_distance(B'101', B'011'), jaccard_distance(B'101', B'011'), jaccard_distance(B'000', B'000');",
                    expected: Expected::Rows {
                        columns: &[Column("binary_quantize", BIT), Column("hamming_distance", FLOAT8), Column("jaccard_distance", FLOAT8), Column("jaccard_distance", FLOAT8)],
                        rows: &[
                            &[T("1001"), T("2"), T("0.6666666666666667"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT hamming_distance(B'101', B'01');",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "different bit lengths 3 and 2", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT l2_distance('[1,2]'::halfvec, '[3,4]'), inner_product('{1:2,3:4}/3'::sparsevec, '{3:1}/3'), cosine_distance('{1:1}/2'::sparsevec, '{2:1}/2'), l2_norm('{}/2'::sparsevec);",
                    expected: Expected::Rows {
                        columns: &[Column("l2_distance", FLOAT8), Column("inner_product", FLOAT8), Column("cosine_distance", FLOAT8), Column("l2_norm", FLOAT8)],
                        rows: &[
                            &[T("2.8284271247461903"), T("4"), T("1"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT cosine_distance('[0,0]'::vector, '[1,1]');",
                    expected: Expected::Rows {
                        columns: &[Column("cosine_distance", FLOAT8)],
                        rows: &[
                            &[T("NaN")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT vector_accum('{0}', '[1,2]'), vector_avg('{2,4,6}'), vector_combine('{0,1}', '{1,2,3}');",
                    expected: Expected::Rows {
                        columns: &[Column("vector_accum", FLOAT8_ARRAY), Column("vector_avg", USER_DEFINED), Column("vector_combine", FLOAT8_ARRAY)],
                        rows: &[
                            &[T("{1,1,2}"), T("[2,3]"), T("{1,2,3}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT vector_combine('{0}', '{0}');",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "vector must have at least 1 dimension", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT avg(v), sum(v) FROM (VALUES ('[1,2]'::vector), ('[3,5]'), (NULL)) t(v);",
                    expected: Expected::Rows {
                        columns: &[Column("avg", USER_DEFINED), Column("sum", USER_DEFINED)],
                        rows: &[
                            &[T("[2,3.5]"), T("[4,7]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT avg(v), sum(v) FROM (VALUES ('[1,2]'::vector)) t(v) WHERE false;",
                    expected: Expected::Rows {
                        columns: &[Column("avg", USER_DEFINED), Column("sum", USER_DEFINED)],
                        rows: &[
                            &[Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT avg(v) FROM (VALUES ('[1,2]'::vector), ('[3]')) t(v);",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "expected 2 dimensions, not 1", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT unnest('{"[1,2,3]", "[4,5,6]"}'::vector[]);"#,
                    expected: Expected::Rows {
                        columns: &[Column("unnest", USER_DEFINED)],
                        rows: &[
                            &[T("[1,2,3]")],
                            &[T("[4,5,6]")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "vector casts",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION vector;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[1,2,3]::vector, '{1.5,2}'::real[]::vector, ARRAY[1,2]::numeric[]::halfvec, '{1,0,2}'::float8[]::sparsevec;",
                    expected: Expected::Rows {
                        columns: &[Column("array", USER_DEFINED), Column("vector", USER_DEFINED), Column("array", USER_DEFINED), Column("sparsevec", USER_DEFINED)],
                        rows: &[
                            &[T("[1,2,3]"), T("[1.5,2]"), T("[1,2]"), T("{1:1,3:2}/3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2]'::vector::real[], '[1,2]'::vector::halfvec::sparsevec, '{1:1,3:2}/3'::sparsevec::vector, '[1.5,2]'::halfvec::vector;",
                    expected: Expected::Rows {
                        columns: &[Column("float4", FLOAT4_ARRAY), Column("sparsevec", USER_DEFINED), Column("vector", USER_DEFINED), Column("vector", USER_DEFINED)],
                        rows: &[
                            &[T("{1,2}"), T("{1:1,2:2}/2"), T("[1,0,2]"), T("[1.5,2]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1,2,3}'::real[]::vector(2);",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "expected 2 dimensions, not 3", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[[1,2],[3,4]]::vector;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "array must be 1-D", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ARRAY[1,NULL]::vector;",
                    expected: Expected::Error(Diagnostic { code: "22004", message: "array must not contain nulls", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{}'::real[]::vector;",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "vector must have at least 1 dimension", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[65520]'::vector::halfvec;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: r#""65520" is out of range for type halfvec"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1:65520}/1'::sparsevec::halfvec;",
                    expected: Expected::Error(Diagnostic { code: "22003", message: r#""65520" is out of range for type halfvec"#, ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '[1,2,3]'::vector::halfvec(2);",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "expected 2 dimensions, not 3", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'abc'::bytea::vector;",
                    expected: Expected::Error(Diagnostic { code: "42846", message: "cannot cast type bytea to vector", position: 20, ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "vector columns",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION vector;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE items (id INT PRIMARY KEY, v vector(3), h halfvec(2), s sparsevec(5), vs vector[]);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO items VALUES (1, '[1,2,3]', '[1.5,2]', '{1:1,3:2}/5', ARRAY['[1,2]'::vector]), (2, '[4,5,6]', '[0,1]', '{}/5', NULL);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO items (id, v) VALUES (3, '[1,2]');",
                    expected: Expected::Error(Diagnostic { code: "22000", message: "expected 3 dimensions, not 2", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO items (id, v) VALUES (3, ARRAY[1,2,3]);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO items (id, h) VALUES (4, '[1,2]'::vector);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM items ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", USER_DEFINED), Column("h", USER_DEFINED), Column("s", USER_DEFINED), Column("vs", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("[1,2,3]"), T("[1.5,2]"), T("{1:1,3:2}/5"), T(r#"{"[1,2]"}"#)],
                            &[T("2"), T("[4,5,6]"), T("[0,1]"), T("{}/5"), Null],
                            &[T("3"), T("[1,2,3]"), Null, Null, Null],
                            &[T("4"), Null, T("[1,2]"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v <-> '[1,1,1]' AS d FROM items ORDER BY d LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("d", FLOAT8)],
                        rows: &[
                            &[T("1"), T("2.23606797749979")],
                            &[T("3"), T("2.23606797749979")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM items ORDER BY v DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("2")],
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT avg(v), sum(h) FROM items;",
                    expected: Expected::Rows {
                        columns: &[Column("avg", USER_DEFINED), Column("sum", USER_DEFINED)],
                        rows: &[
                            &[T("[2,3,4]"), T("[2.5,5]")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE vk (v vector(2) PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vk VALUES ('[2,1]'), ('[1,2]');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO vk VALUES ('[1,2]');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "vk_pkey""#, detail: "Key (v)=([1,2]) already exists.", schema: "public", table: "vk", constraint: "vk_pkey", ..E }),
                    flow: Flow::Query,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM vk ORDER BY v;",
                    expected: Expected::Rows {
                        columns: &[Column("v", USER_DEFINED)],
                        rows: &[
                            &[T("[1,2]")],
                            &[T("[2,1]")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, atttypmod, format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'items'::regclass AND attnum > 0 ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("atttypmod", INT4), Column("format_type", TEXT)],
                        rows: &[
                            &[T("id"), T("-1"), T("integer")],
                            &[T("v"), T("3"), T("vector(3)")],
                            &[T("h"), T("2"), T("halfvec(2)")],
                            &[T("s"), T("5"), T("sparsevec(5)")],
                            &[T("vs"), T("-1"), T("vector[]")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION vector;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop extension vector because other objects depend on it", detail: r#"column vs of table items depends on type vector[]
column v of table items depends on type vector
column v of table vk depends on type vector
column h of table items depends on type halfvec
column s of table items depends on type sparsevec"#, hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    flow: Flow::Query,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "extension catalogs",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_available_extension_versions WHERE name IN ('plpgsql', 'uuid-ossp', 'vector') ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME), Column("version", TEXT), Column("installed", BOOL), Column("superuser", BOOL), Column("trusted", BOOL), Column("relocatable", BOOL), Column("schema", NAME), Column("requires", NAME_ARRAY), Column("comment", TEXT)],
                        rows: &[
                            &[T("plpgsql"), T("1.0"), T("t"), T("t"), T("t"), T("f"), T("pg_catalog"), Null, T("PL/pgSQL procedural language")],
                            &[T("uuid-ossp"), T("1.1"), T("f"), T("t"), T("t"), T("t"), Null, Null, T("generate universally unique identifiers (UUIDs)")],
                            &[T("vector"), T("0.8.6"), T("f"), T("t"), T("f"), T("t"), Null, Null, T("vector data type and ivfflat and hnsw access methods")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION vector;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, installed_version FROM pg_available_extensions WHERE name IN ('plpgsql', 'uuid-ossp', 'vector') ORDER BY name;",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME), Column("installed_version", TEXT)],
                        rows: &[
                            &[T("plpgsql"), T("1.0")],
                            &[T("uuid-ossp"), Null],
                            &[T("vector"), T("0.8.6")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT amname, amtype, amhandler::text FROM pg_am ORDER BY amname;",
                    expected: Expected::Rows {
                        columns: &[Column("amname", NAME), Column("amtype", CHAR), Column("amhandler", TEXT)],
                        rows: &[
                            &[T("brin"), T("i"), T("brinhandler")],
                            &[T("btree"), T("i"), T("bthandler")],
                            &[T("gin"), T("i"), T("ginhandler")],
                            &[T("gist"), T("i"), T("gisthandler")],
                            &[T("hash"), T("i"), T("hashhandler")],
                            &[T("heap"), T("t"), T("heap_tableam_handler")],
                            &[T("hnsw"), T("i"), T("hnswhandler")],
                            &[T("ivfflat"), T("i"), T("ivfflathandler")],
                            &[T("spgist"), T("i"), T("spghandler")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT o.opcname, a.amname, o.opcdefault FROM pg_opclass o JOIN pg_am a ON a.oid = o.opcmethod WHERE a.amname IN ('hnsw', 'ivfflat') ORDER BY a.amname, o.opcname;",
                    expected: Expected::Rows {
                        columns: &[Column("opcname", NAME), Column("amname", NAME), Column("opcdefault", BOOL)],
                        rows: &[
                            &[T("bit_hamming_ops"), T("hnsw"), T("f")],
                            &[T("bit_jaccard_ops"), T("hnsw"), T("f")],
                            &[T("halfvec_cosine_ops"), T("hnsw"), T("f")],
                            &[T("halfvec_ip_ops"), T("hnsw"), T("f")],
                            &[T("halfvec_l1_ops"), T("hnsw"), T("f")],
                            &[T("halfvec_l2_ops"), T("hnsw"), T("f")],
                            &[T("sparsevec_cosine_ops"), T("hnsw"), T("f")],
                            &[T("sparsevec_ip_ops"), T("hnsw"), T("f")],
                            &[T("sparsevec_l1_ops"), T("hnsw"), T("f")],
                            &[T("sparsevec_l2_ops"), T("hnsw"), T("f")],
                            &[T("vector_cosine_ops"), T("hnsw"), T("f")],
                            &[T("vector_ip_ops"), T("hnsw"), T("f")],
                            &[T("vector_l1_ops"), T("hnsw"), T("f")],
                            &[T("vector_l2_ops"), T("hnsw"), T("f")],
                            &[T("bit_hamming_ops"), T("ivfflat"), T("f")],
                            &[T("halfvec_cosine_ops"), T("ivfflat"), T("f")],
                            &[T("halfvec_ip_ops"), T("ivfflat"), T("f")],
                            &[T("halfvec_l2_ops"), T("ivfflat"), T("f")],
                            &[T("vector_cosine_ops"), T("ivfflat"), T("f")],
                            &[T("vector_ip_ops"), T("ivfflat"), T("f")],
                            &[T("vector_l2_ops"), T("ivfflat"), T("t")],
                        ],
                        tag: "SELECT 21",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typname, typtype, typcategory, typlen, typstorage FROM pg_type WHERE typname IN ('vector', '_vector', 'halfvec', 'sparsevec') ORDER BY typname;",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("typtype", CHAR), Column("typcategory", CHAR), Column("typlen", INT2), Column("typstorage", CHAR)],
                        rows: &[
                            &[T("_vector"), T("b"), T("A"), T("-1"), T("x")],
                            &[T("halfvec"), T("b"), T("U"), T("-1"), T("e")],
                            &[T("sparsevec"), T("b"), T("U"), T("-1"), T("e")],
                            &[T("vector"), T("b"), T("U"), T("-1"), T("e")],
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
fn test_extension_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE EXTENSION installs required extensions, schemas, and members, and ALTER EXTENSION UPDATE runs update scripts",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SCHEMA has$dollar;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext1;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "required extension \"test_ext2\" is not installed", hint: "Use CREATE EXTENSION ... CASCADE to install required extensions too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext1 SCHEMA test_ext1;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: "schema \"test_ext1\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext1 SCHEMA test_ext;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: "schema \"test_ext\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext1 SCHEMA has$dollar;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "extension \"test_ext1\" must be installed in schema \"test_ext1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext1 SCHEMA has$dollar CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "installing required extension \"test_ext2\"", ..N }, Diagnostic { code: "00000", message: "installing required extension \"test_ext3\"", ..N }, Diagnostic { code: "00000", message: "installing required extension \"test_ext5\"", ..N }, Diagnostic { code: "00000", message: "installing required extension \"test_ext4\"", ..N }],
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extname, nspname, extversion, extrelocatable FROM pg_extension e, pg_namespace n WHERE extname LIKE 'test_ext%' AND e.extnamespace = n.oid ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("nspname", NAME), Column("extversion", TEXT), Column("extrelocatable", BOOL)],
                        rows: &[
                            &[T("test_ext1"), T("test_ext1"), T("1.0"), T("f")],
                            &[T("test_ext2"), T("has$dollar"), T("1.0"), T("t")],
                            &[T("test_ext3"), T("has$dollar"), T("1.0"), T("t")],
                            &[T("test_ext4"), T("has$dollar"), T("1.0"), T("t")],
                            &[T("test_ext5"), T("has$dollar"), T("1.0"), T("t")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.extname, r.extname AS requires FROM pg_depend d JOIN pg_extension e ON e.oid = d.objid JOIN pg_extension r ON r.oid = d.refobjid WHERE d.classid = 'pg_extension'::regclass AND d.refclassid = 'pg_extension'::regclass ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("requires", NAME)],
                        rows: &[
                            &[T("test_ext1"), T("test_ext2")],
                            &[T("test_ext1"), T("test_ext4")],
                            &[T("test_ext2"), T("test_ext3")],
                            &[T("test_ext2"), T("test_ext5")],
                            &[T("test_ext4"), T("test_ext5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext1;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "extension \"test_ext1\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION IF NOT EXISTS test_ext1;",
                    notices: &[Diagnostic { code: "42710", message: "extension \"test_ext1\" already exists, skipping", ..N }],
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cyclic1 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "installing required extension \"test_ext_cyclic2\"", ..N }],
                    expected: Expected::Error(Diagnostic { code: "42P19", message: "cyclic dependency detected between extensions \"test_ext_cyclic1\" and \"test_ext_cyclic2\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA has$dollar CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 5 other objects", detail: "drop cascades to extension test_ext3\ndrop cascades to extension test_ext5\ndrop cascades to extension test_ext2\ndrop cascades to extension test_ext4\ndrop cascades to extension test_ext1", ..N }],
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA has$dollar;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext6;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext6;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext6;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE old_table1 (col1 serial PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext7;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext7') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("sequence ext7_table1_col1_seq")],
                            &[T("sequence ext7_table2_col2_seq")],
                            &[T("sequence old_table1_col1_seq")],
                            &[T("table ext7_table1")],
                            &[T("table ext7_table2")],
                            &[T("table old_table1")],
                            &[T("type ext7_table1")],
                            &[T("type ext7_table1[]")],
                            &[T("type ext7_table2")],
                            &[T("type ext7_table2[]")],
                            &[T("type old_table1")],
                            &[T("type old_table1[]")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext7 UPDATE TO '2.0';",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext7') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("sequence ext7_table2_col2_seq")],
                            &[T("table ext7_table2")],
                            &[T("type ext7_table2")],
                            &[T("type ext7_table2[]")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext7 UPDATE TO '2.0';",
                    notices: &[Diagnostic { code: "00000", message: "version \"2.0\" of extension \"test_ext7\" is already installed", ..N }],
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext7 UPDATE TO '2.1bad';",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"FUNCTIN\"", context: Some("extension script file \"test_ext7--2.0--2.1bad.sql\", near line 10"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext7 UPDATE TO '2.2bad';",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \",\"", context: Some("SQL statement \"CREATE FUNCTION my_erroneous_func(int) RETURNS int LANGUAGE SQL\nAS $$ SELECT $1 + , 1 $$\"\nextension script file \"test_ext7--2.0--2.2bad.sql\", near line 9"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext7 UPDATE TO '3.0';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "extension \"test_ext7\" has no update path from version \"2.0\" to version \"3.0\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extversion FROM pg_extension WHERE extname = 'test_ext7';",
                    expected: Expected::Rows {
                        columns: &[Column("extversion", TEXT)],
                        rows: &[
                            &[T("2.0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext8;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT regexp_replace(pg_describe_object(classid, objid, objsubid), 'pg_temp_\\d+', 'pg_temp', 'g') COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext8') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("function ext8_even(posint)")],
                            &[T("function pg_temp.ext8_temp_even(posint)")],
                            &[T("table ext8_table1")],
                            &[T("table ext8_temp_table1")],
                            &[T("type ext8_table1")],
                            &[T("type ext8_table1[]")],
                            &[T("type ext8_temp_table1")],
                            &[T("type ext8_temp_table1[]")],
                            &[T("type posint")],
                            &[T("type posint[]")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext8;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext8;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext8;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext9;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext9') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("cast from varbitrange to varbitmultirange")],
                            &[T("function varbitmultirange()")],
                            &[T("function varbitmultirange(varbitrange)")],
                            &[T("function varbitmultirange(varbitrange[])")],
                            &[T("function varbitrange(bit varying,bit varying)")],
                            &[T("function varbitrange(bit varying,bit varying,text)")],
                            &[T("table sometable")],
                            &[T("type somecomposite")],
                            &[T("type somecomposite[]")],
                            &[T("type sometable")],
                            &[T("type sometable[]")],
                            &[T("type varbitmultirange")],
                            &[T("type varbitmultirange[]")],
                            &[T("type varbitrange")],
                            &[T("type varbitrange[]")],
                        ],
                        tag: "SELECT 15",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext9 DROP TYPE varbitrange;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext9') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("cast from varbitrange to varbitmultirange")],
                            &[T("function varbitmultirange()")],
                            &[T("function varbitmultirange(varbitrange)")],
                            &[T("function varbitmultirange(varbitrange[])")],
                            &[T("function varbitrange(bit varying,bit varying)")],
                            &[T("function varbitrange(bit varying,bit varying,text)")],
                            &[T("table sometable")],
                            &[T("type somecomposite")],
                            &[T("type somecomposite[]")],
                            &[T("type sometable")],
                            &[T("type sometable[]")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext9 ADD TYPE varbitrange;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext9 DROP TABLE sometable;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext9') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("cast from varbitrange to varbitmultirange")],
                            &[T("function varbitmultirange()")],
                            &[T("function varbitmultirange(varbitrange)")],
                            &[T("function varbitmultirange(varbitrange[])")],
                            &[T("function varbitrange(bit varying,bit varying)")],
                            &[T("function varbitrange(bit varying,bit varying,text)")],
                            &[T("type somecomposite")],
                            &[T("type somecomposite[]")],
                            &[T("type varbitmultirange")],
                            &[T("type varbitmultirange[]")],
                            &[T("type varbitrange")],
                            &[T("type varbitrange[]")],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext9 ADD TABLE sometable;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext9 ADD TABLE sometable;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "table sometable is already a member of extension \"test_ext9\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext9 DROP TABLE old_table1;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"old_table1\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext9 ADD INDEX old_table1_pkey;",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "cannot add an object of this type to an extension", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE sometable;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop table sometable because extension test_ext9 requires it", hint: "You can drop extension test_ext9 instead.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext9;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext7, test_ext6;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION IF EXISTS test_ext7, no_such_extension;",
                    notices: &[Diagnostic { code: "00000", message: "extension \"test_ext7\" does not exist, skipping", ..N }, Diagnostic { code: "00000", message: "extension \"no_such_extension\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_extension WHERE extname LIKE 'test_ext%';",
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
            name: "ALTER EXTENSION SET SCHEMA and the schemas of required extensions",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SCHEMA has$dollar;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA \"has space\";",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_extschema SCHEMA has$dollar;",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid character in extension \"test_ext_extschema\" schema: must not contain any of \"\"$'\\\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_extschema SCHEMA \"has space\";",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extnamespace::regnamespace FROM pg_extension WHERE extname = 'test_ext_extschema';",
                    expected: Expected::Rows {
                        columns: &[Column("extnamespace", REGNAMESPACE)],
                        rows: &[
                            &[T("\"has space\"")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA s1;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA s2;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_set_schema SCHEMA s1;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_set_schema SET SCHEMA s2;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_set_schema') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("cast from s2.ess_range_type to s2.ess_multirange_type")],
                            &[T("function s2.ess_func(integer)")],
                            &[T("function s2.ess_multirange_type()")],
                            &[T("function s2.ess_multirange_type(s2.ess_range_type)")],
                            &[T("function s2.ess_multirange_type(s2.ess_range_type[])")],
                            &[T("function s2.ess_range_type(text,text)")],
                            &[T("function s2.ess_range_type(text,text,text)")],
                            &[T("table s2.ess_table")],
                            &[T("type s2.ess_composite_type")],
                            &[T("type s2.ess_composite_type[]")],
                            &[T("type s2.ess_multirange_type")],
                            &[T("type s2.ess_multirange_type[]")],
                            &[T("type s2.ess_range_type")],
                            &[T("type s2.ess_range_type[]")],
                            &[T("type s2.ess_table")],
                            &[T("type s2.ess_table[]")],
                        ],
                        tag: "SELECT 16",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_functiondef('s2.ess_func(int)'::regprocedure);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_functiondef", TEXT)],
                        rows: &[
                            &[T("CREATE OR REPLACE FUNCTION s2.ess_func(integer)\n RETURNS text\n LANGUAGE sql\nBEGIN ATOMIC\n SELECT ess_table.f3\n    FROM s2.ess_table\n   WHERE (ess_table.f1 = $1);\nEND\n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_set_schema SET SCHEMA s2;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_set_schema SET SCHEMA no_such_schema;",
                    expected: Expected::Error(Diagnostic { code: "3F000", message: "schema \"no_such_schema\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_extschema SET SCHEMA s1;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "extension \"test_ext_extschema\" does not support SET SCHEMA", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA test_func_dep1;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA test_func_dep2;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA test_func_dep3;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_req_schema1 SCHEMA test_func_dep1;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION test_func_dep1.dep_req1() SET SCHEMA test_func_dep2;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS obj, pg_describe_object(refclassid, refobjid, refobjsubid) COLLATE \"C\" AS objref, deptype FROM pg_depend WHERE classid = 'pg_extension'::regclass AND objid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_req_schema1') ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("obj", TEXT), Column("objref", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("extension test_ext_req_schema1"), T("schema test_func_dep1"), T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_func_dep3;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "extension \"test_ext_req_schema1\" does not support SET SCHEMA", detail: "function test_func_dep2.dep_req1() is not in the extension's schema \"test_func_dep1\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION test_func_dep2.dep_req1() SET SCHEMA test_func_dep1;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_func_dep3;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS obj, pg_describe_object(refclassid, refobjid, refobjsubid) COLLATE \"C\" AS objref, deptype FROM pg_depend WHERE classid = 'pg_extension'::regclass AND objid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_req_schema1') ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("obj", TEXT), Column("objref", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("extension test_ext_req_schema1"), T("schema test_func_dep3"), T("n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext_req_schema1 CASCADE;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA test_func_dep1;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA test_func_dep2;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA test_func_dep3;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_req_schema1 SCHEMA has$dollar;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_req_schema3 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "installing required extension \"test_ext_req_schema2\"", ..N }],
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid character in extension \"test_ext_req_schema1\" schema: must not contain any of \"\"$'\\\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext_req_schema1;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA test_s_dep;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_req_schema1 SCHEMA test_s_dep;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_req_schema3 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "installing required extension \"test_ext_req_schema2\"", ..N }],
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT test_s_dep.dep_req1();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req1", TEXT)],
                        rows: &[
                            &[T("req1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dep_req2();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req2", TEXT)],
                        rows: &[
                            &[T("req1 req2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dep_req3();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req3", TEXT)],
                        rows: &[
                            &[T("req1 req3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dep_req3b();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req3b", TEXT)],
                        rows: &[
                            &[T("req1 req2 req3b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA test_s_dep2;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_s_dep2;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "cannot SET SCHEMA of extension \"test_ext_req_schema1\" because other extensions prevent it", detail: "Extension \"test_ext_req_schema3\" requests no relocation of extension \"test_ext_req_schema1\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_req_schema2 SET SCHEMA test_s_dep;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT test_s_dep.dep_req1();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req1", TEXT)],
                        rows: &[
                            &[T("req1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT test_s_dep.dep_req2();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req2", TEXT)],
                        rows: &[
                            &[T("req1 req2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dep_req3();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req3", TEXT)],
                        rows: &[
                            &[T("req1 req3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dep_req3b();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function public.dep_req2() does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", context: Some("SQL function \"dep_req3b\" statement 1"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext_req_schema3;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_s_dep2;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT test_s_dep2.dep_req1();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req1", TEXT)],
                        rows: &[
                            &[T("req1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT test_s_dep.dep_req2();",
                    expected: Expected::Rows {
                        columns: &[Column("dep_req2", TEXT)],
                        rows: &[
                            &[T("req1 req2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext_req_schema1 CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to extension test_ext_req_schema2", ..N }],
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Extension scripts replace or skip only objects their extension owns",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ext_cor_func() RETURNS text AS $$ SELECT 'ext_cor_func: original'::text $$ LANGUAGE sql;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cor;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "function ext_cor_func() is not a member of extension \"test_ext_cor\"", detail: "An extension is not allowed to replace an object that it does not own.", context: Some("SQL statement \"CREATE OR REPLACE FUNCTION ext_cor_func() RETURNS text\n  AS $$ SELECT 'ext_cor_func: from extension'::text $$ LANGUAGE sql\"\nextension script file \"test_ext_cor--1.0.sql\", near line 8"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ext_cor_func();",
                    expected: Expected::Rows {
                        columns: &[Column("ext_cor_func", TEXT)],
                        rows: &[
                            &[T("ext_cor_func: original")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION ext_cor_func();",
                    expected: Expected::Tag("DROP FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE VIEW ext_cor_view AS SELECT 'ext_cor_view: original'::text AS col;",
                    expected: Expected::Tag("CREATE VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cor;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "view ext_cor_view is not a member of extension \"test_ext_cor\"", detail: "An extension is not allowed to replace an object that it does not own.", context: Some("SQL statement \"CREATE OR REPLACE VIEW ext_cor_view AS\n  SELECT 'ext_cor_view: from extension'::text AS col\"\nextension script file \"test_ext_cor--1.0.sql\", near line 11"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ext_cor_func();",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function ext_cor_func() does not exist", position: 8, hint: "No function matches the given name and argument types. You might need to add explicit type casts.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ext_cor_view;",
                    expected: Expected::Rows {
                        columns: &[Column("col", TEXT)],
                        rows: &[
                            &[T("ext_cor_view: original")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP VIEW ext_cor_view;",
                    expected: Expected::Tag("DROP VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TYPE test_ext_type;",
                    expected: Expected::Tag("CREATE TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cor;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "type test_ext_type is not a member of extension \"test_ext_cor\"", detail: "An extension is not allowed to replace an object that it does not own.", context: Some("SQL statement \"CREATE TYPE test_ext_type AS ENUM('x', 'y')\"\nextension script file \"test_ext_cor--1.0.sql\", near line 17"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TYPE test_ext_type;",
                    expected: Expected::Tag("DROP TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OPERATOR @@>> ( PROCEDURE = poly_contain_pt, LEFTARG = polygon, RIGHTARG = point, COMMUTATOR = <<@@ );",
                    expected: Expected::Tag("CREATE OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cor;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "operator <<@@(point,polygon) is not a member of extension \"test_ext_cor\"", detail: "An extension is not allowed to replace an object that it does not own.", context: Some("SQL statement \"CREATE OPERATOR <<@@ ( PROCEDURE = pt_contained_poly,\n  LEFTARG = point, RIGHTARG = polygon )\"\nextension script file \"test_ext_cor--1.0.sql\", near line 19"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OPERATOR <<@@ (point, polygon);",
                    expected: Expected::Tag("DROP OPERATOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cor;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ext_cor_func();",
                    expected: Expected::Rows {
                        columns: &[Column("ext_cor_func", TEXT)],
                        rows: &[
                            &[T("ext_cor_func: from extension")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ext_cor_view;",
                    expected: Expected::Rows {
                        columns: &[Column("col", TEXT)],
                        rows: &[
                            &[T("ext_cor_view: from extension")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'x'::test_ext_type;",
                    expected: Expected::Rows {
                        columns: &[Column("test_ext_type", USER_DEFINED)],
                        rows: &[
                            &[T("x")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT point(0,0) <<@@ polygon(circle(point(0,0),1));",
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
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_cor') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("function ext_cor_func()")],
                            &[T("operator <<@@(point,polygon)")],
                            &[T("type ext_cor_view")],
                            &[T("type ext_cor_view[]")],
                            &[T("type test_ext_type")],
                            &[T("type test_ext_type[]")],
                            &[T("view ext_cor_view")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE COLLATION ext_cine_coll ( LC_COLLATE = \"C\", LC_CTYPE = \"C\" );",
                    expected: Expected::Tag("CREATE COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cine;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "collation ext_cine_coll is not a member of extension \"test_ext_cine\"", detail: "An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.", context: Some("SQL statement \"CREATE COLLATION IF NOT EXISTS ext_cine_coll\n  ( LC_COLLATE = \"POSIX\", LC_CTYPE = \"POSIX\" )\"\nextension script file \"test_ext_cine--1.0.sql\", near line 10"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP COLLATION ext_cine_coll;",
                    expected: Expected::Tag("DROP COLLATION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW ext_cine_mv AS SELECT 11 AS f1;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cine;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "materialized view ext_cine_mv is not a member of extension \"test_ext_cine\"", detail: "An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.", context: Some("SQL statement \"CREATE MATERIALIZED VIEW IF NOT EXISTS ext_cine_mv AS SELECT 42 AS f1\"\nextension script file \"test_ext_cine--1.0.sql\", near line 13"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP MATERIALIZED VIEW ext_cine_mv;",
                    expected: Expected::Tag("DROP MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SCHEMA ext_cine_schema;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cine;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "schema ext_cine_schema is not a member of extension \"test_ext_cine\"", detail: "An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.", context: Some("SQL statement \"CREATE SCHEMA IF NOT EXISTS ext_cine_schema\"\nextension script file \"test_ext_cine--1.0.sql\", near line 19"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SCHEMA ext_cine_schema;",
                    expected: Expected::Tag("DROP SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SEQUENCE ext_cine_seq;",
                    expected: Expected::Tag("CREATE SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cine;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "sequence ext_cine_seq is not a member of extension \"test_ext_cine\"", detail: "An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.", context: Some("SQL statement \"CREATE SEQUENCE IF NOT EXISTS ext_cine_seq\"\nextension script file \"test_ext_cine--1.0.sql\", near line 21"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SEQUENCE ext_cine_seq;",
                    expected: Expected::Tag("DROP SEQUENCE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ext_cine_tab1 (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cine;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "table ext_cine_tab1 is not a member of extension \"test_ext_cine\"", detail: "An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.", context: Some("SQL statement \"CREATE TABLE IF NOT EXISTS ext_cine_tab1 (x int)\"\nextension script file \"test_ext_cine--1.0.sql\", near line 23"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ext_cine_tab1;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ext_cine_tab2 AS SELECT 42 AS y;",
                    expected: Expected::Tag("SELECT 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cine;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "table ext_cine_tab2 is not a member of extension \"test_ext_cine\"", detail: "An extension may only use CREATE ... IF NOT EXISTS to skip object creation if the conflicting object is one that it already owns.", context: Some("SQL statement \"CREATE TABLE IF NOT EXISTS ext_cine_tab2 AS SELECT 42 AS y\"\nextension script file \"test_ext_cine--1.0.sql\", near line 25"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TABLE ext_cine_tab2;",
                    expected: Expected::Tag("DROP TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_cine;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_cine') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("collation ext_cine_coll")],
                            &[T("foreign-data wrapper ext_cine_fdw")],
                            &[T("materialized view ext_cine_mv")],
                            &[T("schema ext_cine_schema")],
                            &[T("sequence ext_cine_seq")],
                            &[T("server ext_cine_srv")],
                            &[T("table ext_cine_tab1")],
                            &[T("table ext_cine_tab2")],
                            &[T("type ext_cine_mv")],
                            &[T("type ext_cine_mv[]")],
                            &[T("type ext_cine_tab1")],
                            &[T("type ext_cine_tab1[]")],
                            &[T("type ext_cine_tab2")],
                            &[T("type ext_cine_tab2[]")],
                        ],
                        tag: "SELECT 14",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_cine UPDATE TO '1.1';",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) COLLATE \"C\" AS member FROM pg_depend WHERE refclassid = 'pg_extension'::regclass AND deptype = 'e' AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_cine') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("member", TEXT)],
                        rows: &[
                            &[T("collation ext_cine_coll")],
                            &[T("foreign-data wrapper ext_cine_fdw")],
                            &[T("materialized view ext_cine_mv")],
                            &[T("schema ext_cine_schema")],
                            &[T("sequence ext_cine_seq")],
                            &[T("server ext_cine_srv")],
                            &[T("table ext_cine_tab1")],
                            &[T("table ext_cine_tab2")],
                            &[T("table ext_cine_tab3")],
                            &[T("type ext_cine_mv")],
                            &[T("type ext_cine_mv[]")],
                            &[T("type ext_cine_tab1")],
                            &[T("type ext_cine_tab1[]")],
                            &[T("type ext_cine_tab2")],
                            &[T("type ext_cine_tab2[]")],
                            &[T("type ext_cine_tab3")],
                            &[T("type ext_cine_tab3[]")],
                        ],
                        tag: "SELECT 17",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "DEPENDS ON EXTENSION makes objects go with their extension",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SCHEMA test_ext;",
                    expected: Expected::Tag("CREATE SCHEMA"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext5 SCHEMA test_ext;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path TO test_ext;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE a (a1 int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION b() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN NEW.a1 := NEW.a1 + 42; RETURN NEW; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION b() DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER c BEFORE INSERT ON a FOR EACH ROW EXECUTE PROCEDURE b();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TRIGGER c ON a DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE MATERIALIZED VIEW d AS SELECT * FROM a;",
                    expected: Expected::Tag("SELECT 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER MATERIALIZED VIEW d DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER MATERIALIZED VIEW"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX e ON a (a1);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX e DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER INDEX e DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET search_path;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT deptype, i.* FROM pg_catalog.pg_depend, LATERAL (SELECT * FROM pg_identify_object(classid, objid, objsubid)) i WHERE refclassid = 'pg_extension'::regclass AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext5') ORDER BY i.type COLLATE \"C\", i.identity COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("deptype", CHAR), Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("x"), T("function"), T("test_ext"), Null, T("test_ext.b()")],
                            &[T("x"), T("index"), T("test_ext"), T("e"), T("test_ext.e")],
                            &[T("x"), T("materialized view"), T("test_ext"), T("d"), T("test_ext.d")],
                            &[T("x"), T("trigger"), Null, Null, T("c on test_ext.a")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext5;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_class WHERE relname IN ('a', 'd', 'e') AND relnamespace = 'test_ext'::regnamespace;",
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
                    query: "SELECT count(*) FROM pg_proc WHERE proname = 'b';",
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
                    query: "CREATE EXTENSION test_ext5 SCHEMA test_ext;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET search_path TO test_ext;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION b() RETURNS TRIGGER LANGUAGE plpgsql AS $$ BEGIN NEW.a1 := NEW.a1 + 42; RETURN NEW; END; $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION b() DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION b() NO DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION b() NO DEPENDS ON EXTENSION test_ext5;",
                    expected: Expected::Tag("ALTER FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER FUNCTION b() DEPENDS ON EXTENSION no_such_extension;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "extension \"no_such_extension\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET search_path;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT deptype, i.* FROM pg_catalog.pg_depend, LATERAL (SELECT * FROM pg_identify_object(classid, objid, objsubid)) i WHERE refclassid = 'pg_extension'::regclass AND refobjid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext5') ORDER BY i.type COLLATE \"C\", i.identity COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("deptype", CHAR), Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext5;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_proc WHERE proname = 'b';",
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
            name: "Extension listings, versions, and errors",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, default_version, installed_version, comment FROM pg_available_extensions WHERE name IN ('test_ext1', 'test_ext7', 'test_ext_cine', 'dg_config') ORDER BY name COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME), Column("default_version", TEXT), Column("installed_version", TEXT), Column("comment", TEXT)],
                        rows: &[
                            &[T("dg_config"), T("1.0"), Null, T("Doltgrust test extension with configuration tables")],
                            &[T("test_ext1"), T("1.0"), Null, T("Test extension 1")],
                            &[T("test_ext7"), T("1.0"), Null, T("Test extension 7")],
                            &[T("test_ext_cine"), T("1.0"), Null, T("Test extension using CREATE IF NOT EXISTS")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name, version, installed, superuser, trusted, relocatable, schema, requires, comment FROM pg_available_extension_versions WHERE name IN ('test_ext1', 'test_ext7', 'test_ext_cine') ORDER BY name COLLATE \"C\", version COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME), Column("version", TEXT), Column("installed", BOOL), Column("superuser", BOOL), Column("trusted", BOOL), Column("relocatable", BOOL), Column("schema", NAME), Column("requires", NAME_ARRAY), Column("comment", TEXT)],
                        rows: &[
                            &[T("test_ext1"), T("1.0"), T("f"), T("t"), T("f"), T("f"), T("test_ext1"), T("{test_ext2,test_ext4}"), T("Test extension 1")],
                            &[T("test_ext7"), T("1.0"), T("f"), T("t"), T("f"), T("f"), T("public"), Null, T("Test extension 7")],
                            &[T("test_ext7"), T("2.0"), T("f"), T("t"), T("f"), T("f"), T("public"), Null, T("Test extension 7")],
                            &[T("test_ext7"), T("2.1bad"), T("f"), T("t"), T("f"), T("f"), T("public"), Null, T("Test extension 7")],
                            &[T("test_ext7"), T("2.2bad"), T("f"), T("t"), T("f"), T("f"), T("public"), Null, T("Test extension 7")],
                            &[T("test_ext_cine"), T("1.0"), T("f"), T("t"), T("f"), T("t"), Null, Null, T("Test extension using CREATE IF NOT EXISTS")],
                            &[T("test_ext_cine"), T("1.1"), T("f"), T("t"), T("f"), T("t"), Null, Null, T("Test extension using CREATE IF NOT EXISTS")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT source, target, path FROM pg_extension_update_paths('test_ext7') ORDER BY source COLLATE \"C\", target COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("source", TEXT), Column("target", TEXT), Column("path", TEXT)],
                        rows: &[
                            &[T("1.0"), T("2.0"), T("1.0--2.0")],
                            &[T("1.0"), T("2.1bad"), T("1.0--2.0--2.1bad")],
                            &[T("1.0"), T("2.2bad"), T("1.0--2.0--2.2bad")],
                            &[T("2.0"), T("1.0"), Null],
                            &[T("2.0"), T("2.1bad"), T("2.0--2.1bad")],
                            &[T("2.0"), T("2.2bad"), T("2.0--2.2bad")],
                            &[T("2.1bad"), T("1.0"), Null],
                            &[T("2.1bad"), T("2.0"), Null],
                            &[T("2.1bad"), T("2.2bad"), Null],
                            &[T("2.2bad"), T("1.0"), Null],
                            &[T("2.2bad"), T("2.0"), Null],
                            &[T("2.2bad"), T("2.1bad"), Null],
                        ],
                        tag: "SELECT 12",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT source, target, path FROM pg_extension_update_paths('no_such_extension');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "extension \"no_such_extension\" is not available", hint: "The extension must first be installed on the system where PostgreSQL is running.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT source, target, path FROM pg_extension_update_paths('bad--name');",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid extension name: \"bad--name\"", detail: "Extension names must not contain \"--\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) > 0 AS listed FROM pg_available_extensions WHERE name = 'plpgsql';",
                    expected: Expected::Rows {
                        columns: &[Column("listed", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION no_such_extension;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "extension \"no_such_extension\" is not available", hint: "The extension must first be installed on the system where PostgreSQL is running.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION \"bad--name\";",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid extension name: \"bad--name\"", detail: "Extension names must not contain \"--\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION \"-bad\";",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid extension name: \"-bad\"", detail: "Extension names must not begin or end with \"-\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext7 VERSION '1.0--2.0';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "invalid extension version name: \"1.0--2.0\"", detail: "Version names must not contain \"--\".", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext7 VERSION '9.9';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "extension \"test_ext7\" has no installation script nor update path for version \"9.9\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext7 VERSION '2.0';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"old_table1\" does not exist", context: Some("SQL statement \"alter extension test_ext7 add table old_table1\"\nextension script file \"test_ext7--1.0.sql\", near line 7"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extversion FROM pg_extension WHERE extname = 'test_ext7';",
                    expected: Expected::Rows {
                        columns: &[Column("extversion", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext3 SCHEMA public SCHEMA public;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "conflicting or redundant options", position: 42, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext7 UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "extension \"test_ext7\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION no_such_extension UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "extension \"no_such_extension\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext7;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "extension \"test_ext7\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET extension_control_path = 'relative/path';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext3;",
                    expected: Expected::Error(Diagnostic { code: "42602", message: "component in parameter \"extension_control_path\" is not an absolute path", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET extension_control_path = '';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext3;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "extension \"test_ext3\" is not available", hint: "The extension must first be installed on the system where PostgreSQL is running.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT name FROM pg_available_extensions WHERE name LIKE 'test_ext%';",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_extension_config_dump('pg_class', '');",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "pg_extension_config_dump() can only be called from an SQL script executed by CREATE EXTENSION", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Extension configuration tables and event triggers in extension scripts",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_config;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extconfig::regclass[]::text[], extcondition FROM pg_extension WHERE extname = 'dg_config';",
                    expected: Expected::Rows {
                        columns: &[Column("extconfig", TEXT_ARRAY), Column("extcondition", TEXT_ARRAY)],
                        rows: &[
                            &[T("{dg_config_settings,dg_config_log}"), T("{\"WHERE name <> 'builtin'\",\"WHERE false\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_config DROP TABLE dg_config_settings;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extconfig::regclass[]::text[], extcondition FROM pg_extension WHERE extname = 'dg_config';",
                    expected: Expected::Rows {
                        columns: &[Column("extconfig", TEXT_ARRAY), Column("extcondition", TEXT_ARRAY)],
                        rows: &[
                            &[T("{dg_config_log}"), T("{\"WHERE false\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_config ADD TABLE dg_config_settings;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extconfig::regclass[]::text[], extcondition FROM pg_extension WHERE extname = 'dg_config';",
                    expected: Expected::Rows {
                        columns: &[Column("extconfig", TEXT_ARRAY), Column("extcondition", TEXT_ARRAY)],
                        rows: &[
                            &[T("{dg_config_log}"), T("{\"WHERE false\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_config UPDATE TO '1.1';",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extconfig::regclass[]::text[], extcondition FROM pg_extension WHERE extname = 'dg_config';",
                    expected: Expected::Rows {
                        columns: &[Column("extconfig", TEXT_ARRAY), Column("extcondition", TEXT_ARRAY)],
                        rows: &[
                            &[T("{dg_config_log,dg_config_other}"), T("{\"WHERE false\",\"\"}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_config DROP TABLE dg_config_log;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_config DROP TABLE dg_config_other;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extconfig, extcondition FROM pg_extension WHERE extname = 'dg_config';",
                    expected: Expected::Rows {
                        columns: &[Column("extconfig", OID_ARRAY), Column("extcondition", TEXT_ARRAY)],
                        rows: &[
                            &[Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_config;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE dg_outside (x int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_badconfig;",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "table \"dg_outside\" is not a member of the extension being created", context: Some("SQL statement \"SELECT pg_catalog.pg_extension_config_dump('dg_outside', '')\"\nextension script file \"dg_badconfig--1.0.sql\", near line 1"), ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext_evttrig;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext_evttrig UPDATE TO '2.0';",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext_evttrig;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Trusted extensions install as the bootstrap superuser for a user with CREATE on the database",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE ROLE ext_user LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE ext_other LOGIN PASSWORD 'password';",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT CREATE ON SCHEMA public TO ext_user;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ext_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_untrusted;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create extension \"dg_untrusted\"", hint: "Must be superuser to create this extension.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_trusted;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create extension \"dg_trusted\"", hint: "Must have CREATE privilege on current database to create this extension.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_trusted;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ext_other;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_trusted;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of extension dg_trusted", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_trusted UPDATE TO '1.0';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of extension dg_trusted", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_trusted SET SCHEMA public;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for schema public", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_trusted;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_untrusted;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dg_untrusted_func();",
                    expected: Expected::Rows {
                        columns: &[Column("dg_untrusted_func", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_untrusted;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('GRANT CREATE ON DATABASE %I TO ext_user', current_database()); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ext_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_untrusted;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create extension \"dg_untrusted\"", hint: "Must be superuser to create this extension.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_trusted;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dg_trusted_owner;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for table dg_trusted_owner", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dg_trusted_func();",
                    expected: Expected::Rows {
                        columns: &[Column("dg_trusted_func", INT4)],
                        rows: &[
                            &[T("42")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extname, extowner::regrole FROM pg_extension WHERE extname = 'dg_trusted';",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("extowner", REGROLE)],
                        rows: &[
                            &[T("dg_trusted"), T("ext_user")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT runner = (SELECT rolname FROM pg_roles WHERE oid = 10) AS ran_as_bootstrap, owner FROM dg_trusted_owner;",
                    expected: Expected::Rows {
                        columns: &[Column("ran_as_bootstrap", BOOL), Column("owner", TEXT)],
                        rows: &[
                            &[T("t"), T("ext_user")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proowner = 10 AS owned_by_bootstrap FROM pg_proc WHERE proname = 'dg_trusted_func';",
                    expected: Expected::Rows {
                        columns: &[Column("owned_by_bootstrap", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relowner = 10 AS owned_by_bootstrap FROM pg_class WHERE relname = 'dg_trusted_owner';",
                    expected: Expected::Rows {
                        columns: &[Column("owned_by_bootstrap", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ext_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_trusted;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN EXECUTE format('REVOKE CREATE ON DATABASE %I FROM ext_user', current_database()); END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE ext_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_trusted;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied to create extension \"dg_trusted\"", hint: "Must have CREATE privilege on current database to create this extension.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE CREATE ON SCHEMA public FROM ext_user;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE ext_user;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE ext_other;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "plpgsql is a built-in extension",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, default_version, installed_version, comment FROM pg_available_extensions WHERE name = 'plpgsql';",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME), Column("default_version", TEXT), Column("installed_version", TEXT), Column("comment", TEXT)],
                        rows: &[
                            &[T("plpgsql"), T("1.0"), T("1.0"), T("PL/pgSQL procedural language")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION plpgsql;",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "extension \"plpgsql\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION IF NOT EXISTS plpgsql;",
                    notices: &[Diagnostic { code: "42710", message: "extension \"plpgsql\" already exists, skipping", ..N }],
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION plpgsql;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_language WHERE lanname = 'plpgsql';",
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
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Event triggers see the commands of extension scripts as in an extension",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE FUNCTION report_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $$ DECLARE r record; BEGIN FOR r IN SELECT * FROM pg_event_trigger_ddl_commands() LOOP RAISE WARNING '% % %', r.command_tag, r.object_identity, r.in_extension; END LOOP; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EVENT TRIGGER report_ddl ON ddl_command_end EXECUTE FUNCTION report_ddl();",
                    expected: Expected::Tag("CREATE EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION test_ext3;",
                    notices: &[
                        Diagnostic { severity: "WARNING", code: "01000", message: "CREATE TABLE public.test_ext3_table t", context: Some("PL/pgSQL function report_ddl() line 1 at RAISE\nSQL statement \"CREATE TABLE test_ext3_table (col_old INT)\"\nextension script file \"test_ext3--1.0.sql\", near line 5"), ..N },
                        Diagnostic { severity: "WARNING", code: "01000", message: "ALTER TABLE public.test_ext3_table.col_new t", context: Some("PL/pgSQL function report_ddl() line 1 at RAISE\nSQL statement \"ALTER TABLE test_ext3_table RENAME col_old TO col_new\"\nextension script file \"test_ext3--1.0.sql\", near line 7"), ..N },
                        Diagnostic { severity: "WARNING", code: "01000", message: "CREATE EXTENSION test_ext3 f", context: Some("PL/pgSQL function report_ddl() line 1 at RAISE"), ..N },
                    ],
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION test_ext3 DROP TABLE test_ext3_table;",
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "ALTER EXTENSION test_ext3 f", context: Some("PL/pgSQL function report_ddl() line 1 at RAISE"), ..N }],
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE plain_table (x int);",
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "CREATE TABLE public.plain_table f", context: Some("PL/pgSQL function report_ddl() line 1 at RAISE"), ..N }],
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EVENT TRIGGER report_ddl;",
                    expected: Expected::Tag("DROP EVENT TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION test_ext3;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "uuid-ossp is a built-in extension whose functions are in LANGUAGE C",
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION \"uuid-ossp\";",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT extname, extversion, extrelocatable FROM pg_extension WHERE extname = 'uuid-ossp';",
                    expected: Expected::Rows {
                        columns: &[Column("extname", NAME), Column("extversion", TEXT), Column("extrelocatable", BOOL)],
                        rows: &[
                            &[T("uuid-ossp"), T("1.1"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_nil(), uuid_ns_dns(), uuid_ns_url(), uuid_ns_oid(), uuid_ns_x500();",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_nil", UUID), Column("uuid_ns_dns", UUID), Column("uuid_ns_url", UUID), Column("uuid_ns_oid", UUID), Column("uuid_ns_x500", UUID)],
                        rows: &[
                            &[T("00000000-0000-0000-0000-000000000000"), T("6ba7b810-9dad-11d1-80b4-00c04fd430c8"), T("6ba7b811-9dad-11d1-80b4-00c04fd430c8"), T("6ba7b812-9dad-11d1-80b4-00c04fd430c8"), T("6ba7b814-9dad-11d1-80b4-00c04fd430c8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_generate_v3(uuid_ns_dns(), 'www.widgets.com'), uuid_generate_v5(uuid_ns_dns(), 'www.widgets.com');",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_generate_v3", UUID), Column("uuid_generate_v5", UUID)],
                        rows: &[
                            &[T("3d813cbb-47fb-32ba-91df-831e1593ac29"), T("21f7f8de-8051-5b89-8680-0195ef798b6a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_generate_v5(uuid_ns_url(), 'http://www.postgresql.org');",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_generate_v5", UUID)],
                        rows: &[
                            &[T("e1ee1ad4-cd4e-5889-962a-4f605a68d94e")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_extract_version(uuid_generate_v1()) AS v1, uuid_extract_version(uuid_generate_v1mc()) AS v1mc, uuid_extract_version(uuid_generate_v4()) AS v4;",
                    expected: Expected::Rows {
                        columns: &[Column("v1", INT2), Column("v1mc", INT2), Column("v4", INT2)],
                        rows: &[
                            &[T("1"), T("1"), T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_generate_v1() <> uuid_generate_v1() AS differ;",
                    expected: Expected::Rows {
                        columns: &[Column("differ", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (get_byte(uuid_send(uuid_generate_v1mc()), 10) & 3) = 3 AS mc;",
                    expected: Expected::Rows {
                        columns: &[Column("mc", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_extract_timestamp(uuid_generate_v1()) BETWEEN now() - interval '1 minute' AND now() + interval '1 minute' AS recent;",
                    expected: Expected::Rows {
                        columns: &[Column("recent", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION my_v4() RETURNS uuid AS '$libdir/uuid-ossp', 'uuid_generate_v4' LANGUAGE C;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT uuid_extract_version(my_v4());",
                    expected: Expected::Rows {
                        columns: &[Column("uuid_extract_version", INT2)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION uuid_nil() RETURNS uuid AS '$libdir/uuid-ossp' LANGUAGE C;",
                    expected: Expected::Error(Diagnostic { code: "42723", message: "function \"uuid_nil\" already exists with same argument types", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad1() RETURNS uuid AS '$libdir/no_such_lib', 'f' LANGUAGE C;",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: "could not access file \"no_such_lib\": No such file or directory", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad2() RETURNS uuid AS '$libdir/uuid-ossp', 'no_such_symbol' LANGUAGE C;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "could not find function \"no_such_symbol\" in file ", message_contains: true, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION bad3() RETURNS uuid AS 'no_such_lib', 'f' LANGUAGE C;",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: "could not access file \"no_such_lib\": No such file or directory", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION \"uuid-ossp\";",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION \"uuid-ossp\" VERSION '1.0';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "extension \"uuid-ossp\" has no installation script nor update path for version \"1.0\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, prolang::regproc, probin, prosrc FROM pg_proc WHERE proname IN ('uuid_generate_v4', 'my_v4') ORDER BY proname COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("prolang", REGPROC), Column("probin", TEXT), Column("prosrc", TEXT)],
                        rows: &[
                            &[T("my_v4"), T("13"), T("$libdir/uuid-ossp"), T("uuid_generate_v4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Extension scripts in other encodings",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_latin1;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dg_latin1_word(), octet_length(dg_latin1_word());",
                    expected: Expected::Rows {
                        columns: &[Column("dg_latin1_word", TEXT), Column("octet_length", INT4)],
                        rows: &[
                            &[T("café ½"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_badenc;",
                    expected: Expected::Error(Diagnostic { code: "22021", message: "invalid byte sequence for encoding \"EUC_JP\": 0xff 0x27", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_extension WHERE extname = 'dg_badenc';",
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
            name: "Initial privileges of extension members",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
                "CREATE ROLE ip_role;",
                "CREATE ROLE ip_role2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_initprivs;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE objoid >= 16384 ORDER BY pg_describe_object(classoid, objoid, objsubid) COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("column b of table dg_ip_tab"), T("e"), T("{ip_role=w/postgres}")],
                            &[T("function dg_ip_func()"), T("e"), T("{postgres=X/postgres,ip_role=X/postgres}")],
                            &[T("large object 4242424"), T("e"), T("{postgres=rw/postgres,=r/postgres}")],
                            &[T("schema dg_ip_schema"), T("e"), T("{postgres=UC/postgres,=U/postgres}")],
                            &[T("table dg_ip_tab"), T("e"), T("{postgres=arwdDxtm/postgres,=r/postgres}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) AS object, deptype FROM pg_shdepend WHERE refobjid = 'ip_role'::regrole ORDER BY pg_describe_object(classid, objid, objsubid) COLLATE \"C\", deptype;",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("column b of table dg_ip_tab"), T("a")],
                            &[T("column b of table dg_ip_tab"), T("i")],
                            &[T("function dg_ip_func()"), T("a")],
                            &[T("function dg_ip_func()"), T("i")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT INSERT ON dg_ip_tab TO ip_role;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_initprivs DROP TABLE dg_ip_tab;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE objoid >= 16384 ORDER BY pg_describe_object(classoid, objoid, objsubid) COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("function dg_ip_func()"), T("e"), T("{postgres=X/postgres,ip_role=X/postgres}")],
                            &[T("large object 4242424"), T("e"), T("{postgres=rw/postgres,=r/postgres}")],
                            &[T("schema dg_ip_schema"), T("e"), T("{postgres=UC/postgres,=U/postgres}")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER EXTENSION dg_initprivs ADD TABLE dg_ip_tab;",
                    expected: Expected::Tag("ALTER EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE objoid >= 16384 ORDER BY pg_describe_object(classoid, objoid, objsubid) COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("column b of table dg_ip_tab"), T("e"), T("{ip_role=w/postgres}")],
                            &[T("function dg_ip_func()"), T("e"), T("{postgres=X/postgres,ip_role=X/postgres}")],
                            &[T("large object 4242424"), T("e"), T("{postgres=rw/postgres,=r/postgres}")],
                            &[T("schema dg_ip_schema"), T("e"), T("{postgres=UC/postgres,=U/postgres}")],
                            &[T("table dg_ip_tab"), T("e"), T("{postgres=arwdDxtm/postgres,=r/postgres,ip_role=a/postgres}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE ip_role;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: r#"role "ip_role" cannot be dropped because some objects depend on it"#, detail: "privileges for table dg_ip_tab\ninitial privileges for table dg_ip_tab\nprivileges for column b of table dg_ip_tab\ninitial privileges for column b of table dg_ip_tab\nprivileges for function dg_ip_func()\ninitial privileges for function dg_ip_func()", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY ip_role TO ip_role2;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE objoid >= 16384 ORDER BY pg_describe_object(classoid, objoid, objsubid) COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("column b of table dg_ip_tab"), T("e"), T("{ip_role2=w/postgres}")],
                            &[T("function dg_ip_func()"), T("e"), T("{postgres=X/postgres,ip_role2=X/postgres}")],
                            &[T("large object 4242424"), T("e"), T("{postgres=rw/postgres,=r/postgres}")],
                            &[T("schema dg_ip_schema"), T("e"), T("{postgres=UC/postgres,=U/postgres}")],
                            &[T("table dg_ip_tab"), T("e"), T("{postgres=arwdDxtm/postgres,=r/postgres,ip_role2=a/postgres}")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY ip_role2;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE objoid >= 16384 ORDER BY pg_describe_object(classoid, objoid, objsubid) COLLATE \"C\";",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("function dg_ip_func()"), T("e"), T("{postgres=X/postgres}")],
                            &[T("large object 4242424"), T("e"), T("{postgres=rw/postgres,=r/postgres}")],
                            &[T("schema dg_ip_schema"), T("e"), T("{postgres=UC/postgres,=U/postgres}")],
                            &[T("table dg_ip_tab"), T("e"), T("{postgres=arwdDxtm/postgres,=r/postgres}")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_initprivs;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_init_privs WHERE objoid >= 16384;",
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
            name: "Include directives in extension control files",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT name, version, superuser, relocatable, comment FROM pg_available_extension_versions WHERE name = 'dg_include';",
                    expected: Expected::Rows {
                        columns: &[Column("name", NAME), Column("version", TEXT), Column("superuser", BOOL), Column("relocatable", BOOL), Column("comment", TEXT)],
                        rows: &[
                            &[T("dg_include"), T("1.0"), T("f"), T("t"), T("Doltgrust test extension whose control file includes others")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_include;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dg_include_func();",
                    expected: Expected::Rows {
                        columns: &[Column("dg_include_func", INT4)],
                        rows: &[
                            &[T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET extension_control_path = '{TESTDATA}/extensions_broken';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_include_absent;",
                    expected: Expected::Error(Diagnostic { code: "58P01", message: "could not open configuration file \"{TESTDATA}/extensions_broken/extension/dg_no_such.conf\": No such file or directory", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_include_loop;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "configuration file recursion in \"{TESTDATA}/extensions_broken/extension/dg_include_loop.control\"", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_include_deep;",
                    expected: Expected::Error(Diagnostic { code: "54000", message: "could not open configuration file \"dg_include_ping.conf\": maximum nesting depth exceeded", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Initial privileges an extension's script sets on its database and a setting",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
                "CREATE ROLE ip_db_role;",
                "CREATE ROLE ip_db_role2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_initprivs_db;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_database'::regclass AND objoid = (SELECT oid FROM pg_database WHERE datname = current_database());",
                    expected: Expected::Rows {
                        columns: &[Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("e"), T("{=Tc/postgres,postgres=CTc/postgres,ip_db_role=C/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_parameter_acl'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("parameter log_lock_waits"), T("e"), T("{postgres=sA/postgres,ip_db_role=s/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass AS catalog, deptype FROM pg_shdepend WHERE refobjid = 'ip_db_role'::regrole ORDER BY classid::regclass::text COLLATE \"C\", deptype;",
                    expected: Expected::Rows {
                        columns: &[Column("catalog", REGCLASS), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_database"), T("a")],
                            &[T("pg_database"), T("i")],
                            &[T("pg_parameter_acl"), T("a")],
                            &[T("pg_parameter_acl"), T("i")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_initprivs_db;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_database'::regclass AND objoid = (SELECT oid FROM pg_database WHERE datname = current_database());",
                    expected: Expected::Rows {
                        columns: &[Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("e"), T("{=Tc/postgres,postgres=CTc/postgres,ip_db_role=C/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_parameter_acl'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("parameter log_lock_waits"), T("e"), T("{postgres=sA/postgres,ip_db_role=s/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY ip_db_role TO ip_db_role2;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_database'::regclass AND objoid = (SELECT oid FROM pg_database WHERE datname = current_database());",
                    expected: Expected::Rows {
                        columns: &[Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("e"), T("{=Tc/postgres,postgres=CTc/postgres,ip_db_role2=C/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classoid, objoid, objsubid) AS object, privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_parameter_acl'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("object", TEXT), Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("parameter log_lock_waits"), T("e"), T("{postgres=sA/postgres,ip_db_role2=s/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY ip_db_role2;",
                    expected: Expected::Error(Diagnostic { code: "XX000", message: "unrecognized class ID: 6243", ..E }),
                    skip: Some("DROP OWNED removes the role from a setting's initial privileges, where Postgres' object properties have no parameter ACL (Appendix C); server_wide_roles::test_drop_owned_and_revoke_leave_no_dependency_on_initial_privileges_of_settings covers ours"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "Initial privileges an extension's script sets on a tablespace",
            set_up_script: &[
                "SET extension_control_path = '{TESTDATA}/extensions';",
                "CREATE ROLE ip_ts_role;",
                "CREATE ROLE ip_ts_role2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE EXTENSION dg_initprivs_ts;",
                    expected: Expected::Tag("CREATE EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_tablespace'::regclass AND objoid = (SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default');",
                    expected: Expected::Rows {
                        columns: &[Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("e"), T("{postgres=C/postgres,ip_ts_role=C/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass AS catalog, deptype FROM pg_shdepend WHERE refobjid = 'ip_ts_role'::regrole ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("catalog", REGCLASS), Column("deptype", CHAR)],
                        rows: &[
                            &[T("pg_tablespace"), T("a")],
                            &[T("pg_tablespace"), T("i")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP EXTENSION dg_initprivs_ts;",
                    expected: Expected::Tag("DROP EXTENSION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_tablespace'::regclass AND objoid = (SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default');",
                    expected: Expected::Rows {
                        columns: &[Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("e"), T("{postgres=C/postgres,ip_ts_role=C/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REASSIGN OWNED BY ip_ts_role TO ip_ts_role2;",
                    expected: Expected::Tag("REASSIGN OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_tablespace'::regclass AND objoid = (SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default');",
                    expected: Expected::Rows {
                        columns: &[Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("e"), T("{postgres=C/postgres,ip_ts_role2=C/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY ip_ts_role2;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT privtype, initprivs FROM pg_init_privs WHERE classoid = 'pg_tablespace'::regclass AND objoid = (SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default');",
                    expected: Expected::Rows {
                        columns: &[Column("privtype", CHAR), Column("initprivs", ACLITEM_ARRAY)],
                        rows: &[
                            &[T("e"), T("{postgres=C/postgres}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE ip_ts_role, ip_ts_role2;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "role \"ip_ts_role\" cannot be dropped because some objects depend on it", detail: "privileges for tablespace pg_default", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP OWNED BY ip_ts_role;",
                    expected: Expected::Tag("DROP OWNED"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE ip_ts_role, ip_ts_role2;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
