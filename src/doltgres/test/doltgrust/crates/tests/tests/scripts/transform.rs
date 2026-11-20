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
fn test_transforms() {
    run_scripts(&[
        ScriptTest {
            name: "CREATE TRANSFORM, its checks, CREATE FUNCTION's TRANSFORM FOR TYPE, and DROP TRANSFORM",
            set_up_script: &[
                "CREATE TYPE trf_t AS (a int);",
                "CREATE DOMAIN trf_d AS int;",
                "CREATE FUNCTION trf_from(internal) RETURNS internal LANGUAGE internal IMMUTABLE STRICT AS 'int4recv';",
                "CREATE FUNCTION trf_to(internal) RETURNS trf_t LANGUAGE internal IMMUTABLE STRICT AS 'int4recv';",
                "CREATE FUNCTION trf_vol(internal) RETURNS internal LANGUAGE internal STRICT AS 'int4recv';",
                "CREATE FUNCTION trf_two(internal, int) RETURNS internal LANGUAGE internal IMMUTABLE AS 'int4recv';",
                "CREATE FUNCTION trf_set(internal) RETURNS SETOF internal LANGUAGE internal IMMUTABLE AS 'int4recv';",
                "CREATE FUNCTION trf_win(internal) RETURNS internal LANGUAGE internal WINDOW IMMUTABLE AS 'int4recv';",
                "CREATE FUNCTION trf_int(int) RETURNS trf_t LANGUAGE internal IMMUTABLE AS 'int4recv';",
                "CREATE FUNCTION trf_retint(internal) RETURNS int LANGUAGE internal IMMUTABLE AS 'int4recv';",
                "CREATE PROCEDURE trf_proc(internal) LANGUAGE internal AS 'int4recv';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR anyelement LANGUAGE sql (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "data type anyelement is a pseudo-type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_d LANGUAGE sql (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "data type trf_d is a domain", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE nosuch (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "language \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_vol(internal));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "transform function must not be volatile", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_two(internal, int));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "transform function must take one argument", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_set(internal));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "transform function must not return a set", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (TO SQL WITH FUNCTION trf_int(int));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "first argument of transform function must be type internal", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_win(internal));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "transform function must be a normal function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_retint(internal));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "return data type of FROM SQL function must be internal", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (TO SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "return data type of TO SQL function must be the transform data type", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_proc(internal));",
                    expected: Expected::Error(Diagnostic { code: "42809", message: "trf_proc(internal) is not a function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION nosuch(internal));",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function nosuch(internal) does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_from(internal), TO SQL WITH FUNCTION trf_to(internal));",
                    expected: Expected::Tag("CREATE TRANSFORM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42710", message: "transform for type trf_t language \"sql\" already exists", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT trftype::regtype, trflang, trffromsql::regproc, trftosql::regproc FROM pg_transform;",
                    expected: Expected::Rows {
                        columns: &[Column("trftype", REGTYPE), Column("trflang", OID), Column("trffromsql", REGPROC), Column("trftosql", REGPROC)],
                        rows: &[
                            &[T("trf_t"), T("14"), T("trf_from"), T("trf_to")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid), pg_describe_object(refclassid, refobjid, refobjsubid), deptype FROM pg_depend WHERE classid = 'pg_transform'::regclass ORDER BY 2;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("transform for trf_t language sql"), T("function trf_from(internal)"), T("n")],
                            &[T("transform for trf_t language sql"), T("function trf_to(internal)"), T("n")],
                            &[T("transform for trf_t language sql"), T("type trf_t"), T("n")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_identify_object('pg_transform'::regclass, (SELECT oid FROM pg_transform), 0);",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("schema", TEXT), Column("name", TEXT), Column("identity", TEXT)],
                        rows: &[
                            &[T("transform"), Null, Null, T("for public.trf_t language sql")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pg_identify_object_as_address('pg_transform'::regclass, (SELECT oid FROM pg_transform), 0);",
                    expected: Expected::Rows {
                        columns: &[Column("type", TEXT), Column("object_names", TEXT_ARRAY), Column("object_args", TEXT_ARRAY)],
                        rows: &[
                            &[T("transform"), T("{public.trf_t}"), T("{sql}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid) FROM pg_get_object_address('transform', '{trf_t}', '{sql}');",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT)],
                        rows: &[
                            &[T("transform for trf_t language sql")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE TRANSFORM FOR trf_t LANGUAGE sql (TO SQL WITH FUNCTION trf_to(internal));",
                    expected: Expected::Tag("CREATE TRANSFORM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT trftype::regtype, trflang, trffromsql::regproc, trftosql::regproc FROM pg_transform;",
                    expected: Expected::Rows {
                        columns: &[Column("trftype", REGTYPE), Column("trflang", OID), Column("trffromsql", REGPROC), Column("trftosql", REGPROC)],
                        rows: &[
                            &[T("trf_t"), T("14"), T("-"), T("trf_to")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(refclassid, refobjid, refobjsubid), deptype FROM pg_depend WHERE classid = 'pg_transform'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("function trf_to(internal)"), T("n")],
                            &[T("type trf_t"), T("n")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trf_f(trf_t) RETURNS int LANGUAGE sql TRANSFORM FOR TYPE trf_t AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trf_g(int) RETURNS int LANGUAGE sql TRANSFORM FOR TYPE int AS 'SELECT 1';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "transform for type integer language \"sql\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trf_h(int) RETURNS int LANGUAGE plpgsql TRANSFORM FOR TYPE trf_t AS 'BEGIN RETURN 1; END';",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "transform for type trf_t language \"plpgsql\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION trf_arr(trf_t[]) RETURNS int LANGUAGE sql TRANSFORM FOR TYPE trf_t[], FOR TYPE trf_t AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, protrftypes::regtype[] FROM pg_proc WHERE proname IN ('trf_f', 'trf_arr') ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("protrftypes", REGTYPE_ARRAY)],
                        rows: &[
                            &[T("trf_arr"), T("{trf_t,trf_t}")],
                            &[T("trf_f"), T("{trf_t}")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_get_functiondef('trf_f'::regproc);",
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_functiondef", TEXT)],
                        rows: &[
                            &[T("CREATE OR REPLACE FUNCTION public.trf_f(trf_t)\n RETURNS integer\n TRANSFORM FOR TYPE trf_t\n LANGUAGE sql\nAS $function$SELECT 1$function$\n")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_describe_object(classid, objid, objsubid), pg_describe_object(refclassid, refobjid, refobjsubid), deptype FROM pg_depend WHERE refclassid = 'pg_transform'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_describe_object", TEXT), Column("pg_describe_object", TEXT), Column("deptype", CHAR)],
                        rows: &[
                            &[T("function trf_arr(trf_t[])"), T("transform for trf_t language sql"), T("n")],
                            &[T("function trf_f(trf_t)"), T("transform for trf_t language sql"), T("n")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE PROCEDURE trf_p() LANGUAGE sql TRANSFORM FOR TYPE trf_t AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE PROCEDURE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT proname, protrftypes::regtype[] FROM pg_proc WHERE proname = 'trf_p';",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME), Column("protrftypes", REGTYPE_ARRAY)],
                        rows: &[
                            &[T("trf_p"), T("{trf_t}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMENT ON TRANSFORM FOR trf_t LANGUAGE sql IS 'a transform';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT obj_description((SELECT oid FROM pg_transform), 'pg_transform');",
                    expected: Expected::Rows {
                        columns: &[Column("obj_description", TEXT)],
                        rows: &[
                            &[T("a transform")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM FOR trf_t LANGUAGE sql;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop transform for trf_t language sql because other objects depend on it", detail: "function trf_f(trf_t) depends on transform for trf_t language sql\nfunction trf_arr(trf_t[]) depends on transform for trf_t language sql\nfunction trf_p() depends on transform for trf_t language sql", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM IF EXISTS FOR trf_t LANGUAGE plpgsql;",
                    notices: &[Diagnostic { code: "00000", message: "transform for type trf_t language \"plpgsql\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TRANSFORM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM IF EXISTS FOR trf_t LANGUAGE nosuch;",
                    notices: &[Diagnostic { code: "00000", message: "transform for type trf_t language \"nosuch\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TRANSFORM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM IF EXISTS FOR nosuch_t LANGUAGE sql;",
                    notices: &[Diagnostic { code: "00000", message: "type \"nosuch_t\" does not exist, skipping", ..N }],
                    expected: Expected::Tag("DROP TRANSFORM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM FOR trf_t LANGUAGE plpgsql;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "transform for type trf_t language \"plpgsql\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM FOR trf_t LANGUAGE nosuch;",
                    expected: Expected::Error(Diagnostic { code: "42704", message: "language \"nosuch\" does not exist", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE ROLE trf_r;",
                    expected: Expected::Tag("CREATE ROLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE trf_r;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE plpgsql (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type trf_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM FOR trf_t LANGUAGE sql;",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of type trf_t", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE trf_t OWNER TO trf_r;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE trf_r;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE plpgsql (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "must be owner of function trf_from", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "REVOKE USAGE ON LANGUAGE plpgsql FROM PUBLIC;",
                    expected: Expected::Tag("REVOKE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET ROLE trf_r;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE plpgsql (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Error(Diagnostic { code: "42501", message: "permission denied for language plpgsql", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET ROLE;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "GRANT USAGE ON LANGUAGE plpgsql TO PUBLIC;",
                    expected: Expected::Tag("GRANT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TYPE trf_t OWNER TO CURRENT_USER;",
                    expected: Expected::Tag("ALTER TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRANSFORM FOR trf_t LANGUAGE sql CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 3 other objects", detail: "drop cascades to function trf_f(trf_t)\ndrop cascades to function trf_arr(trf_t[])\ndrop cascades to function trf_p()", ..N }],
                    expected: Expected::Tag("DROP TRANSFORM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_transform;",
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
                    query: "SELECT proname FROM pg_proc WHERE proname LIKE 'trf\\_%' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("proname", NAME)],
                        rows: &[
                            &[T("trf_from")],
                            &[T("trf_int")],
                            &[T("trf_proc")],
                            &[T("trf_retint")],
                            &[T("trf_set")],
                            &[T("trf_to")],
                            &[T("trf_two")],
                            &[T("trf_vol")],
                            &[T("trf_win")],
                        ],
                        tag: "SELECT 9",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRANSFORM FOR trf_t LANGUAGE sql (FROM SQL WITH FUNCTION trf_from(internal));",
                    expected: Expected::Tag("CREATE TRANSFORM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION trf_from(internal);",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop function trf_from(internal) because other objects depend on it", detail: "transform for trf_t language sql depends on function trf_from(internal)", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TYPE trf_t CASCADE;",
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to 3 other objects", detail: "drop cascades to function trf_to(internal)\ndrop cascades to function trf_int(integer)\ndrop cascades to transform for trf_t language sql", ..N }],
                    expected: Expected::Tag("DROP TYPE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_transform;",
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
