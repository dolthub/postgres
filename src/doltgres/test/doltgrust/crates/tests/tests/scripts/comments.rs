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
fn test_comments_on_shared_objects() {
    run_scripts(&[
        ScriptTest {
            name: "comments on roles and databases go to pg_shdescription",
            set_up_script: &[
                "CREATE ROLE sd_r;",
                "CREATE ROLE sd_plain;",
                "CREATE DATABASE sd_db;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT d.datname, shobj_description(d.oid, 'pg_database') FROM pg_database d WHERE datname IN ('postgres', 'template0', 'template1') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("shobj_description", TEXT)],
                        rows: &[
                            &[T("postgres"), T("default administrative connection database")],
                            &[T("template0"), T("unmodifiable empty database")],
                            &[T("template1"), T("default template for new databases")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON ROLE sd_r IS 'a role';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT rolname, shobj_description(oid, 'pg_authid') FROM pg_roles WHERE rolname LIKE 'sd\\_%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("rolname", NAME), Column("shobj_description", TEXT)],
                        rows: &[
                            &[T("sd_plain"), Null],
                            &[T("sd_r"), T("a role")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON ROLE sd_r IS 'a changed role';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT shobj_description('sd_r'::regrole, 'pg_authid');",
                    expected: Expected::Rows {
                        columns: &[Column("shobj_description", TEXT)],
                        rows: &[
                            &[T("a changed role")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON DATABASE sd_db IS 'a database';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON DATABASE sd_nope IS 'x';",
                    expected: Expected::Tag("COMMENT"),
                    notices: &[Diagnostic { severity: "WARNING", code: "3D000", message: r#"database "sd_nope" does not exist"#, ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE DATABASE sd_copy TEMPLATE sd_db;",
                    expected: Expected::Tag("CREATE DATABASE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT datname, shobj_description(oid, 'pg_database') FROM pg_database WHERE datname LIKE 'sd\\_%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("datname", NAME), Column("shobj_description", TEXT)],
                        rows: &[
                            &[T("sd_copy"), Null],
                            &[T("sd_db"), T("a database")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE sd_plain;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON ROLE sd_r IS 'no';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied", detail: "The current user must have the CREATEROLE attribute.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON DATABASE sd_db IS 'no';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of database sd_db", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON ROLE sd_r IS NULL;",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON DATABASE sd_db IS '';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_shdescription WHERE objoid >= 16384;",
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
                    query: "COMMENT ON ROLE sd_r IS 'again';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP ROLE sd_r;",
                    expected: Expected::Tag("DROP ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_shdescription WHERE classoid = 'pg_authid'::regclass;",
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
                    query: "COMMENT ON TABLESPACE pg_default IS 'x';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a parameter ACL's object address, another shared object",
            set_up_script: &[
                "CREATE ROLE pa_r;",
                "GRANT SET ON PARAMETER work_mem TO pa_r;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) FROM pg_get_object_address('parameter ACL', '{work_mem}', '{}');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("parameter work_mem")],
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
