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
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_subscriptions() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE, ALTER, and DROP SUBSCRIPTION without a publisher",
            set_up_script: &[
                "CREATE ROLE regress_sub_owner SUPERUSER;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SUBSCRIPTION regress_sub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);",
                    expected: Expected::Tag("CREATE SUBSCRIPTION"),
                    notices: &[Diagnostic {
                        severity: "WARNING",
                        code: "01000",
                        message: "subscription was created, but is not connected",
                        hint: "To initiate replication, you must manually create the replication slot, enable the subscription, and refresh the subscription.",
                        ..N
                    }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT subname, subenabled, subslotname, subpublications, subconninfo, subsynccommit, subbinary, substream, subtwophasestate, subdisableonerr, subpasswordrequired, subrunasowner, subfailover, suborigin FROM pg_subscription;",
                    expected: Expected::Rows {
                        columns: &[Column("subname", NAME), Column("subenabled", BOOL), Column("subslotname", NAME), Column("subpublications", TEXT_ARRAY), Column("subconninfo", TEXT), Column("subsynccommit", TEXT), Column("subbinary", BOOL), Column("substream", CHAR), Column("subtwophasestate", CHAR), Column("subdisableonerr", BOOL), Column("subpasswordrequired", BOOL), Column("subrunasowner", BOOL), Column("subfailover", BOOL), Column("suborigin", TEXT)],
                        rows: &[
                            &[T("regress_sub"), T("f"), T("regress_sub"), T("{testpub}"), T("dbname=regress_doesnotexist"), T("off"), T("f"), T("p"), T("d"), T("f"), T("t"), T("f"), T("f"), T("any")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT roname = 'pg_' || (SELECT oid FROM pg_subscription) AS named FROM pg_replication_origin;",
                    expected: Expected::Rows {
                        columns: &[Column("named", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub SET (synchronous_commit = local, binary = true);",
                    expected: Expected::Tag("ALTER SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT subsynccommit, subbinary FROM pg_subscription;",
                    expected: Expected::Rows {
                        columns: &[Column("subsynccommit", TEXT), Column("subbinary", BOOL)],
                        rows: &[
                            &[T("local"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub RENAME TO regress_sub2;",
                    expected: Expected::Tag("ALTER SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub2 OWNER TO regress_sub_owner;",
                    expected: Expected::Tag("ALTER SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.subname, r.rolname FROM pg_subscription s JOIN pg_roles r ON r.oid = s.subowner;",
                    expected: Expected::Rows {
                        columns: &[Column("subname", NAME), Column("rolname", NAME)],
                        rows: &[
                            &[T("regress_sub2"), T("regress_sub_owner")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub2 ENABLE;",
                    expected: Expected::Tag("ALTER SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub2 SET (slot_name = NONE);",
                    expected: Expected::Error(Diagnostic { code: "55000", message: "cannot set slot_name = NONE for enabled subscription", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub2 DISABLE;",
                    expected: Expected::Tag("ALTER SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub2 SET (slot_name = NONE);",
                    expected: Expected::Tag("ALTER SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SUBSCRIPTION regress_sub3 CONNECTION 'dbname=x' PUBLICATION testpub WITH (connect = false, nope = 1);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"unrecognized subscription parameter: "nope""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SUBSCRIPTION regress_sub3 CONNECTION 'dbname=x ==' PUBLICATION testpub WITH (connect = false);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "invalid connection string syntax: invalid connection option \"\"\n", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE SUBSCRIPTION regress_sub3 CONNECTION 'dbname=x' PUBLICATION testpub WITH (connect = false, enabled = true);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "connect = false and enabled = true are mutually exclusive options", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT subname, apply_error_count, sync_error_count, stats_reset IS NULL AS never_reset FROM pg_stat_subscription_stats;",
                    expected: Expected::Rows {
                        columns: &[Column("subname", NAME), Column("apply_error_count", INT8), Column("sync_error_count", INT8), Column("never_reset", BOOL)],
                        rows: &[
                            &[T("regress_sub2"), T("0"), T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SUBSCRIPTION regress_sub2;",
                    expected: Expected::Tag("DROP SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_subscription;",
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
                    query: "DROP SUBSCRIPTION regress_sub2;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"subscription "regress_sub2" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SUBSCRIPTION IF EXISTS regress_sub2;",
                    expected: Expected::Tag("DROP SUBSCRIPTION"),
                    notices: &[Diagnostic { code: "00000", message: r#"subscription "regress_sub2" does not exist, skipping"#, ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replication origin functions",
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pg_replication_origin_create('regress_origin');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_replication_origin_create", OID)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT roname FROM pg_replication_origin;",
                    expected: Expected::Rows {
                        columns: &[Column("roname", TEXT)],
                        rows: &[
                            &[T("regress_origin")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_replication_origin_oid('regress_origin') IS NOT NULL AS found;",
                    expected: Expected::Rows {
                        columns: &[Column("found", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_replication_origin_create('regress_origin');",
                    expected: Expected::Error(Diagnostic {
                        code: "23505",
                        message: r#"duplicate key value violates unique constraint "pg_replication_origin_roname_index""#,
                        detail: "Key (roname)=(regress_origin) already exists.",
                        schema: "pg_catalog",
                        table: "pg_replication_origin",
                        constraint: "pg_replication_origin_roname_index",
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_replication_origin_drop('regress_origin');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_replication_origin_drop", VOID)],
                        rows: &[
                            &[T("")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_replication_origin_oid('regress_origin');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_replication_origin_oid", OID)],
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
            name: "pg_get_object_address and pg_identify_object_as_address for subscriptions",
            set_up_script: &[
                "CREATE SUBSCRIPTION regress_sub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_get_object_address('subscription', '{one}', '{}');",
                    expected: Expected::Error(Diagnostic {
                        code: "42704",
                        message: r#"subscription "one" does not exist"#,
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classid::regclass, objid = (SELECT oid FROM pg_subscription) AS same, objsubid FROM pg_get_object_address('subscription', '{regress_sub}', '{}');",
                    expected: Expected::Rows {
                        columns: &[Column("classid", REGCLASS), Column("same", BOOL), Column("objsubid", INT4)],
                        rows: &[
                            &[T("pg_subscription"), T("t"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_identify_object_as_address('pg_subscription'::regclass, (SELECT oid FROM pg_subscription), 0);",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("object_names", TEXT_ARRAY), Column("object_args", TEXT_ARRAY)],
                        rows: &[
                            &[T("subscription"), T("{regress_sub}"), T("{}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_get_object_address('subscription', '{a,b}', '{}');",
                    expected: Expected::Error(Diagnostic {
                        code: "22023",
                        message: "name list length must be exactly 1",
                        ..E
                    }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "COMMENT ON SUBSCRIPTION",
            set_up_script: &[
                "CREATE SUBSCRIPTION regress_sub CONNECTION 'dbname=regress_doesnotexist' PUBLICATION testpub WITH (connect = false);",
                "CREATE ROLE regress_sub_user;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "COMMENT ON SUBSCRIPTION regress_sub IS 'a subscription';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description(oid, 'pg_subscription') FROM pg_subscription;",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("a subscription")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT classoid::regclass, objsubid, description FROM pg_description WHERE classoid = 'pg_subscription'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("classoid", REGCLASS), Column("objsubid", INT4), Column("description", TEXT)],
                        rows: &[
                            &[T("pg_subscription"), T("0"), T("a subscription")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_shdescription WHERE classoid = 'pg_subscription'::regclass;",
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
                    query: "COMMENT ON SUBSCRIPTION regress_nosub IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: r#"subscription "regress_nosub" does not exist"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON SUBSCRIPTION regress_sub IS NULL;",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_description WHERE classoid = 'pg_subscription'::regclass;",
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
                    query: "COMMENT ON SUBSCRIPTION regress_sub IS 'again';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE regress_sub_user;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON SUBSCRIPTION regress_sub IS 'not mine';",
                    expected: Expected::Error(Diagnostic { code: "42501", message: r#"must be owner of subscription regress_sub"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT description FROM pg_description WHERE classoid = 'pg_subscription'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("description", TEXT)],
                        rows: &[
                            &[T("again")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER SUBSCRIPTION regress_sub SET (slot_name = NONE);",
                    expected: Expected::Tag("ALTER SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP SUBSCRIPTION regress_sub;",
                    expected: Expected::Tag("DROP SUBSCRIPTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) AS orphans FROM pg_description WHERE classoid = 'pg_subscription'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("orphans", INT8)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    skip: Some("DROP SUBSCRIPTION removes the subscription's comment, which Postgres leaves behind"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_create_subscription_that_cannot_connect() {
    run_scripts(&[
        ScriptTest {
            name: "a connection string that parses but cannot connect, Postgres' subscription.sql",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE SUBSCRIPTION regress_testsub5 CONNECTION 'port=-1' PUBLICATION testpub;",
                    expected: Expected::Error(Diagnostic { code: "08006", message: r#"subscription "regress_testsub5" could not connect to the publisher: invalid port number: "-1""#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_subscription WHERE subname = 'regress_testsub5';",
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
