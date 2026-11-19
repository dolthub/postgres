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

//! Dolt's go/libraries/doltcore/sqle/enginetest/dolt_queries_legacy.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_legacy_create_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "create single column schema",
            intent: "A table with one column is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (id int PRIMARY KEY);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'testtable' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create two column schema",
            intent: "A table with two columns is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (id int PRIMARY KEY, age int);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'testtable' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create two column keyless schema",
            intent: "A table with two columns and no primary key is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (id int, age int);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'testtable' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table syntax error",
            intent: "A CREATE TABLE with a syntax error is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable id int, age int;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"id\"", position: 24, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table bad table name",
            intent: "A CREATE TABLE with an invalid table name is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE -testTable (id int PRIMARY KEY, age int);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"-\"", position: 14, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table reserved name",
            intent: "Table names beginning with dolt_ are reserved for Dolt's own tables.",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE dolt_table (id int PRIMARY KEY, age int);",
                    expected: Expected::Error(Diagnostic { code: "42939", message: "Invalid table name dolt_table. Table names beginning with `dolt_` are reserved for internal use", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table name already in use",
            intent: "A CREATE TABLE of an existing table's name is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE people (id int PRIMARY KEY, age int);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"people\" already exists", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table if not exists on existing table",
            intent: "CREATE TABLE IF NOT EXISTS of an existing table succeeds and leaves it unchanged.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE IF NOT EXISTS people (id int PRIMARY KEY, age int);",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "NOTICE", code: "42P07", message: "relation \"people\" already exists, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM people;",
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
            name: "create table with types",
            intent: "A table with columns of several types is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (id int PRIMARY KEY, age int, first_name varchar(255), is_married boolean);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'testtable' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table all supported types",
            intent: "A table with a column of each supported type is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (c0 int PRIMARY KEY, c1 smallint, c2 smallint, c3 integer, c4 integer, c5 bigint, c6 bool, c7 boolean, c8 bit(10), c9 text, c10 text, c11 text, c12 text, c16 char(5), c17 varchar(255), c18 varchar(80), c19 float, c20 double precision, c22 bigint, c23 smallint, c24 integer, c25 integer, c26 numeric(20), c27 boolean);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM testTable;",
                    expected: Expected::Rows {
                        columns: &[Column("c0", INT4), Column("c1", INT2), Column("c2", INT2), Column("c3", INT4), Column("c4", INT4), Column("c5", INT8), Column("c6", BOOL), Column("c7", BOOL), Column("c8", BIT), Column("c9", TEXT), Column("c10", TEXT), Column("c11", TEXT), Column("c12", TEXT), Column("c16", BPCHAR), Column("c17", VARCHAR), Column("c18", VARCHAR), Column("c19", FLOAT8), Column("c20", FLOAT8), Column("c22", INT8), Column("c23", INT2), Column("c24", INT4), Column("c25", INT4), Column("c26", NUMERIC), Column("c27", BOOL)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with composite primary key",
            intent: "A table with a composite primary key is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (id int, age int, first_name varchar(80), is_married bool, PRIMARY KEY (id, age));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'testtable' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with not null constraints",
            intent: "A table with NOT NULL columns is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (id int, age int, first_name varchar(80) NOT NULL, is_married bool, PRIMARY KEY (id, age));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'testtable' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table with quoted columns",
            intent: "A table with quoted column names is created.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE testTable (\"id\" int, \"age\" int, \"timestamp\" varchar(80), \"is married\" bool, PRIMARY KEY (\"id\", \"age\"));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'testtable' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table ip2nation",
            intent: "The ip2nation table is created with its defaults and keys.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ip2nation (ip bigint NOT NULL DEFAULT 0, country char(2) NOT NULL DEFAULT '', PRIMARY KEY (ip));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'ip2nation' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "create table ip2nationCountries",
            intent: "The ip2nationCountries table is created with its defaults and keys.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ip2nationCountries (code varchar(4) NOT NULL DEFAULT '', iso_code_2 varchar(2) NOT NULL DEFAULT '', iso_code_3 varchar(3) DEFAULT '', iso_country varchar(255) NOT NULL DEFAULT '', country varchar(255) NOT NULL DEFAULT '', lat float NOT NULL DEFAULT 0.0, lon float NOT NULL DEFAULT 0.0, PRIMARY KEY (code));",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable, column_default FROM information_schema.columns WHERE table_name = 'ip2nationcountries' ORDER BY ordinal_position;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_legacy_delete_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "delete everything",
            intent: "A DELETE without WHERE removes every row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where id equals",
            intent: "A DELETE where id equals a value removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id = 2;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where id less than",
            intent: "A DELETE where id is less than a value removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id < 3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where id greater than",
            intent: "A DELETE where id is greater than a value removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id > 3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where id less than or equal",
            intent: "A DELETE where id is at most a value removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id <= 3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where id greater than or equal",
            intent: "A DELETE where id is at least a value removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id >= 3;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where id equals nothing",
            intent: "A DELETE whose filter matches no row removes nothing.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id = 9999;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where last_name matches some =",
            intent: "A DELETE filtering last_name with = removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE last_name = 'Simpson';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where last_name matches some <>",
            intent: "A DELETE filtering last_name with <> removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE last_name <> 'Simpson';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete where last_name matches some like",
            intent: "A DELETE filtering last_name with LIKE removes only the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE last_name LIKE '%pson';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete order by",
            intent: "A DELETE of every row, which MySQL orders, removes them all.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's DELETE takes ORDER BY and LIMIT, which Postgres writes as a subquery that picks the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete order by asc limit",
            intent: "Deleting the first three rows by key removes only those.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id IN (SELECT id FROM people ORDER BY id ASC LIMIT 3);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's DELETE takes ORDER BY and LIMIT, which Postgres writes as a subquery that picks the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete order by desc limit",
            intent: "Deleting the last three rows by key removes only those.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id IN (SELECT id FROM people ORDER BY id DESC LIMIT 3);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's DELETE takes ORDER BY and LIMIT, which Postgres writes as a subquery that picks the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete order by desc limit offset",
            intent: "Deleting three rows by descending key after skipping one removes only those.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE id IN (SELECT id FROM people ORDER BY id DESC LIMIT 3 OFFSET 1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's DELETE takes ORDER BY and LIMIT, which Postgres writes as a subquery that picks the rows")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete invalid table",
            intent: "A DELETE from a missing table is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM nobody;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"nobody\" does not exist", position: 13, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "delete invalid column",
            intent: "A DELETE filtering on a missing column is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DELETE FROM people WHERE z = 'dne';",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"z\" does not exist", position: 26, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_legacy_drop_table_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "drop table",
            intent: "DROP TABLE removes a table.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE people;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"people\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop table case insensitive",
            intent: "DROP TABLE finds a table whatever the case of its unquoted name.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE PEOPLE;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"people\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop table if exists",
            intent: "DROP TABLE IF EXISTS removes an existing table.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE IF EXISTS people;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"people\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop non existent table",
            intent: "DROP TABLE of a missing table is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE notfound;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "table \"notfound\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop non existent table if exists",
            intent: "DROP TABLE IF EXISTS of a missing table succeeds.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE IF EXISTS notFound;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: "table \"notfound\" does not exist, skipping", ..N }],
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop many tables",
            intent: "One DROP TABLE removes several tables.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE people, appearances, episodes;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"people\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM appearances;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"appearances\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM episodes;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"episodes\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop many tables some don't exist",
            intent: "A DROP TABLE naming several tables, some missing, behaves as Postgres' DROP TABLE does.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "DROP TABLE IF EXISTS people, not_real, appearances, episodes;",
                    expected: Expected::Ok,
                    notices: &[Diagnostic { severity: "NOTICE", code: "00000", message: "table \"not_real\" does not exist, skipping", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"people\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM appearances;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"appearances\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM episodes;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"episodes\" does not exist", position: 15, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_legacy_index_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "create unique index success and failure",
            intent: "A unique index is created over unique values and refused over duplicates.",
            set_up_script: &[
                "CREATE TABLE pass_unique (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "CREATE TABLE fail_unique (pk1 bigint PRIMARY KEY, v1 bigint, v2 bigint);",
                "INSERT INTO pass_unique VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);",
                "INSERT INTO fail_unique VALUES (1, 1, 1), (2, 2, 2), (3, 2, 3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx_v1 ON pass_unique(v1);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX idx_v1 ON fail_unique(v1);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: "relation \"idx_v1\" already exists", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_legacy_insert_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "insert no columns",
            intent: "An INSERT without a column list writes every column.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert no columns too few values",
            intent: "An INSERT without a column list and too few values fills the rest with defaults.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres fills the columns a VALUES list leaves out with their defaults, where MySQL requires a value for every column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert no columns too many values",
            intent: "An INSERT without a column list and too many values is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222, 'abc');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more expressions than target columns", position: 105, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert full columns",
            intent: "An INSERT naming every column writes the row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating, uuid, num_episodes) VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert full columns mixed order",
            intent: "An INSERT naming every column in another order writes the row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (num_episodes, uuid, rating, age, is_married, last_name, first_name, id) VALUES (222, '00000000-0000-0000-0000-000000000002', 9, 10, 0, 'Simpson', 'Bart', 2);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert full columns negative values",
            intent: "An INSERT of negative values writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating, uuid, num_episodes) VALUES (-7, 'Maggie', 'Simpson', 0, -1, -5.1, '00000000-0000-0000-0000-000000000005', 677);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = -7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("-7"), T("Maggie"), T("Simpson"), T("0"), T("-1"), T("-5.1"), T("00000000-0000-0000-0000-000000000005"), T("677")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert full columns null values",
            intent: "An INSERT of NULLs into nullable columns writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating, uuid, num_episodes) VALUES (2, 'Bart', 'Simpson', NULL, NULL, NULL, NULL, NULL);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns",
            intent: "An INSERT naming some columns writes them and leaves the rest NULL.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart', 'Simpson');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns mixed order",
            intent: "An INSERT naming some columns in another order writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (last_name, first_name, id) VALUES ('Simpson', 'Bart', 2);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns duplicate column",
            intent: "An INSERT naming a column twice is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, first_name) VALUES (2, 'Bart', 'Simpson', 'Bart');",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"first_name\" specified more than once", position: 48, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns invalid column",
            intent: "An INSERT naming a missing column is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, middle) VALUES (2, 'Bart', 'Simpson', 'Nani');",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"middle\" of relation \"people\" does not exist", position: 48, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert missing non-nullable column",
            intent: "An INSERT leaving out a NOT NULL column is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name) VALUES (2, 'Bart');",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"last_name\" of relation \"people\" violates not-null constraint", detail: "Failing row contains (2, Bart, null, null, null, null, null, null).", schema: "public", table: "people", column: "last_name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns mismatch too many values",
            intent: "An INSERT with more values than columns is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart', 'Simpson', 0);",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more expressions than target columns", position: 78, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns mismatch too few values",
            intent: "An INSERT with fewer values than columns is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart');",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more target columns than expressions", position: 37, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns functions",
            intent: "An INSERT of function results writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, UPPER('Bart'), 'Simpson');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("BART"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns multiple rows 2",
            intent: "An INSERT of two rows writes both.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (0, 'Bart', 'Simpson'), (1, 'Homer', 'Simpson');",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id < 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("0"), T("Bart"), T("Simpson")],
                            &[T("1"), T("Homer"), T("Simpson")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns multiple rows 5",
            intent: "An INSERT of five rows writes them all.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES\n					(7, 'Maggie', 'Simpson', 0, 1, 5.1),\n					(8, 'Milhouse', 'Van Houten', 0, 8, 3.5),\n					(9, 'Jacqueline', 'Bouvier', 1, 80, 2),\n					(10, 'Patty', 'Bouvier', 0, 40, 7),\n					(11, 'Selma', 'Bouvier', 0, 40, 7);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id > 6 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("5.1")],
                            &[T("8"), T("Milhouse"), T("Van Houten"), T("0"), T("8"), T("3.5")],
                            &[T("9"), T("Jacqueline"), T("Bouvier"), T("1"), T("80"), T("2")],
                            &[T("10"), T("Patty"), T("Bouvier"), T("0"), T("40"), T("7")],
                            &[T("11"), T("Selma"), T("Bouvier"), T("0"), T("40"), T("7")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns multiple rows null pk",
            intent: "An INSERT of rows with a NULL primary key is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (0, 'Bart', 'Simpson'), (1, 'Homer', NULL);",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"last_name\" of relation \"people\" violates not-null constraint", detail: "Failing row contains (1, Homer, null, null, null, null, null, null).", schema: "public", table: "people", column: "last_name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns multiple rows duplicate",
            intent: "An INSERT of two rows with the same key behaves as Postgres does.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart', 'Simpson'), (2, 'Bart', 'Simpson');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"people_pkey\"", detail: "Key (id)=(2) already exists.", schema: "public", table: "people", constraint: "people_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert partial columns existing pk",
            intent: "An INSERT of an existing key behaves as Postgres does.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "CREATE TABLE temppeople (id bigint PRIMARY KEY, first_name varchar(1023), last_name varchar(1023));",
                "INSERT INTO temppeople VALUES (2, 'Bart', 'Simpson');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO temppeople (id, first_name, last_name) VALUES (2, 'Bart', 'Simpson');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"temppeople_pkey\"", detail: "Key (id)=(2) already exists.", schema: "public", table: "temppeople", constraint: "temppeople_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch int -> string",
            intent: "An INSERT of a int value into a string column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 'Maggie', 100, 0, 1, 5.1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("100"), T("0"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch int -> bool",
            intent: "An INSERT of a int value into a bool column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 'Maggie', 'Simpson', 1, 1, 5.1);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("1"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch string -> int",
            intent: "An INSERT of a string value into a int column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES ('7', 'Maggie', 'Simpson', 0, 1, 5.1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch string -> float",
            intent: "An INSERT of a string value into a float column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 'Maggie', 'Simpson', 0, 1, '5.1');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch string -> uint",
            intent: "An INSERT of a string value into a uint column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, num_episodes) VALUES (7, 'Maggie', 'Simpson', 0, 1, '100');",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, num_episodes FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("100")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch float -> string",
            intent: "An INSERT of a float value into a string column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 8.1, 'Simpson', 0, 1, 5.1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("8.1"), T("Simpson"), T("0"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch float -> bool",
            intent: "An INSERT of a float value into a bool column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 'Maggie', 'Simpson', 0.5, 1, 5.1);",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("1"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch float -> int",
            intent: "An INSERT of a float value into a int column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 'Maggie', 'Simpson', 0, 1.0, 5.1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch bool -> int",
            intent: "An INSERT of a bool value into a int column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (1, 'Maggie', 'Simpson', 0, 1, 5.1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 1 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("1"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("5.1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch bool -> float",
            intent: "An INSERT of a bool value into a float column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 'Maggie', 'Simpson', 0, 1, 1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "insert type mismatch bool -> string",
            intent: "An INSERT of a bool value into a string column behaves as Postgres' assignment casts allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES (7, 1, 'Simpson', 0, 1, 5.1);",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id = 7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("1"), T("Simpson"), T("0"), T("1"), T("5.1")],
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
fn test_legacy_join_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "legacy join tests",
            intent: "Cross joins, comma joins with WHERE, and JOIN ... ON across three tables return the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM people, episodes ORDER BY people.id, episodes.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC), Column("id", INT8), Column("name", VARCHAR), Column("air_date", TIMESTAMP), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555"), T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                        ],
                        tag: "SELECT 24",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people p, episodes e WHERE e.id = p.id ORDER BY p.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC), Column("id", INT8), Column("name", VARCHAR), Column("air_date", TIMESTAMP), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p.*, e.* FROM people p, episodes e, appearances a WHERE a.episode_id = e.id AND a.character_id = p.id ORDER BY p.id, e.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC), Column("id", INT8), Column("name", VARCHAR), Column("air_date", TIMESTAMP), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM people p, episodes e, appearances a WHERE a.episode_id = e.id AND a.character_id = p.id;",
                    expected: Expected::Error(Diagnostic { code: "42702", message: "column reference \"id\" is ambiguous", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p.*, e.* FROM people p, episodes e, appearances a WHERE a.episode_id = id AND a.character_id = id;",
                    expected: Expected::Error(Diagnostic { code: "42702", message: "column reference \"id\" is ambiguous", position: 79, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id, p.id, e.name, p.first_name, p.last_name FROM people p, episodes e WHERE e.id = p.id ORDER BY p.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("id", INT8), Column("name", VARCHAR), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id AS eid, p.id AS pid, e.name AS ename, p.first_name AS pfirst_name, p.last_name last_name FROM people p, episodes e WHERE e.id = p.id ORDER BY pid;",
                    expected: Expected::Rows {
                        columns: &[Column("eid", INT8), Column("pid", INT8), Column("ename", VARCHAR), Column("pfirst_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id AS eid, p.id AS \"p.id\", e.name AS ename, p.first_name AS pfirst_name, p.last_name last_name FROM people p, episodes e WHERE e.id = p.id ORDER BY \"p.id\";",
                    expected: Expected::Rows {
                        columns: &[Column("eid", INT8), Column("p.id", INT8), Column("ename", VARCHAR), Column("pfirst_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people p JOIN episodes e ON e.id = p.id ORDER BY p.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC), Column("id", INT8), Column("name", VARCHAR), Column("air_date", TIMESTAMP), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("4"), T("There's No Disgrace Like Home"), T("1990-01-29 03:00:00"), T("8.5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p.*, e.* FROM people p JOIN appearances a ON a.character_id = p.id JOIN episodes e ON a.episode_id = e.id ORDER BY p.id, e.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC), Column("id", INT8), Column("name", VARCHAR), Column("air_date", TIMESTAMP), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null, T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("1"), T("Simpsons Roasting On an Open Fire"), T("1989-12-18 03:00:00"), T("8")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444"), T("2"), T("Bart the Genius"), T("1990-01-15 03:00:00"), T("9")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555"), T("3"), T("Homer's Odyssey"), T("1990-01-22 03:00:00"), T("7")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id, p.id, e.name, p.first_name, p.last_name FROM people p JOIN episodes e ON e.id = p.id ORDER BY p.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("id", INT8), Column("name", VARCHAR), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.name, p.first_name, p.last_name FROM people p JOIN episodes e ON e.id = p.id ORDER BY p.id;",
                    expected: Expected::Rows {
                        columns: &[Column("name", VARCHAR), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id, p.id, e.name, p.first_name, p.last_name FROM people p\n								JOIN episodes e ON e.id = p.id\n								ORDER BY e.name;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("id", INT8), Column("name", VARCHAR), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id, p.id, e.name, p.first_name, p.last_name FROM people p\n								JOIN episodes e ON e.id = p.id\n								ORDER BY age;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("id", INT8), Column("name", VARCHAR), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id AS eid, p.id AS pid, e.name AS ename, p.first_name AS pfirst_name, p.last_name last_name FROM people p JOIN episodes e ON e.id = p.id ORDER BY pid;",
                    expected: Expected::Rows {
                        columns: &[Column("eid", INT8), Column("pid", INT8), Column("ename", VARCHAR), Column("pfirst_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id AS eid, p.id AS pid, e.name AS ename, p.first_name AS pfirst_name, p.last_name last_name FROM people p JOIN episodes e ON e.id = p.id ORDER BY ename;",
                    expected: Expected::Rows {
                        columns: &[Column("eid", INT8), Column("pid", INT8), Column("ename", VARCHAR), Column("pfirst_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT e.id AS eid, p.id AS \"p.id\", e.name AS ename, p.first_name AS pfirst_name, p.last_name last_name FROM people p JOIN episodes e ON e.id = p.id ORDER BY \"p.id\";",
                    expected: Expected::Rows {
                        columns: &[Column("eid", INT8), Column("p.id", INT8), Column("ename", VARCHAR), Column("pfirst_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("1"), T("Simpsons Roasting On an Open Fire"), T("Marge"), T("Simpson")],
                            &[T("2"), T("2"), T("Bart the Genius"), T("Bart"), T("Simpson")],
                            &[T("3"), T("3"), T("Homer's Odyssey"), T("Lisa"), T("Simpson")],
                            &[T("4"), T("4"), T("There's No Disgrace Like Home"), T("Moe"), T("Szyslak")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.episode_id AS eid, p.id AS pid, p.first_name\n						FROM appearances a JOIN people p ON a.character_id = p.id ORDER BY eid, pid;",
                    expected: Expected::Rows {
                        columns: &[Column("eid", INT8), Column("pid", INT8), Column("first_name", VARCHAR)],
                        rows: &[
                            &[T("1"), T("0"), T("Homer")],
                            &[T("1"), T("1"), T("Marge")],
                            &[T("2"), T("0"), T("Homer")],
                            &[T("2"), T("2"), T("Bart")],
                            &[T("2"), T("3"), T("Lisa")],
                            &[T("2"), T("4"), T("Moe")],
                            &[T("3"), T("0"), T("Homer")],
                            &[T("3"), T("1"), T("Marge")],
                            &[T("3"), T("3"), T("Lisa")],
                            &[T("3"), T("5"), T("Barney")],
                        ],
                        tag: "SELECT 10",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_legacy_replace_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "replace no columns",
            intent: "An upsert without a column list writes every column.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace set",
            intent: "An upsert with SET assignments writes the row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating, uuid, num_episodes) VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222) ON CONFLICT (id) DO UPDATE SET first_name = EXCLUDED.first_name, last_name = EXCLUDED.last_name, is_married = EXCLUDED.is_married, age = EXCLUDED.age, rating = EXCLUDED.rating, uuid = EXCLUDED.uuid, num_episodes = EXCLUDED.num_episodes;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's REPLACE ... SET is an upsert of a column list, and its FALSE is 0")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace no columns too few values",
            intent: "An upsert without a column list and too few values fills the rest with defaults.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::Differs("Postgres fills the columns a VALUES list leaves out with their defaults, where MySQL requires a value for every column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace no columns too many values",
            intent: "An upsert without a column list and too many values is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222, 'abc') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more expressions than target columns", position: 105, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace full columns",
            intent: "An upsert naming every column writes the row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating, uuid, num_episodes) VALUES (2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace full columns mixed order",
            intent: "An upsert naming every column in another order writes the row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (num_episodes, uuid, rating, age, is_married, last_name, first_name, id) VALUES (222, '00000000-0000-0000-0000-000000000002', 9, 10, 0, 'Simpson', 'Bart', 2) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace full columns negative values",
            intent: "An upsert of negative values writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating, uuid, num_episodes) VALUES (-7, 'Maggie', 'Simpson', 0, -1, -5.1, '00000000-0000-0000-0000-000000000005', 677) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = -7 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("-7"), T("Maggie"), T("Simpson"), T("0"), T("-1"), T("-5.1"), T("00000000-0000-0000-0000-000000000005"), T("677")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace full columns null values",
            intent: "An upsert of NULLs into nullable columns writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating, uuid, num_episodes) VALUES (2, 'Bart', 'Simpson', NULL, NULL, NULL, NULL, NULL) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns",
            intent: "An upsert naming some columns writes them and leaves the rest NULL.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart', 'Simpson') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns mixed order",
            intent: "An upsert naming some columns in another order writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (last_name, first_name, id) VALUES ('Simpson', 'Bart', 2) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns duplicate column",
            intent: "An upsert naming a column twice is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, first_name) VALUES (2, 'Bart', 'Simpson', 'Bart') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "42701", message: "column \"first_name\" specified more than once", position: 48, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns invalid column",
            intent: "An upsert naming a missing column is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, middle) VALUES (2, 'Bart', 'Simpson', 'Nani') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"middle\" of relation \"people\" does not exist", position: 48, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace missing non-nullable column",
            intent: "An upsert leaving out a NOT NULL column is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name) VALUES (2, 'Bart') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"last_name\" of relation \"people\" violates not-null constraint", detail: "Failing row contains (2, Bart, null, null, null, null, null, null).", schema: "public", table: "people", column: "last_name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns mismatch too many values",
            intent: "An upsert with more values than columns is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart', 'Simpson', 0) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more expressions than target columns", position: 78, ..E }),
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns mismatch too few values",
            intent: "An upsert with fewer values than columns is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "INSERT has more target columns than expressions", position: 37, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns functions",
            intent: "An upsert of function results writes them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, UPPER('Bart'), 'Simpson') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("2"), T("BART"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns multiple rows 2",
            intent: "An upsert of two rows writes both.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (0, 'Bart', 'Simpson'), (1, 'Homer', 'Simpson') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id < 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("0"), T("Bart"), T("Simpson")],
                            &[T("1"), T("Homer"), T("Simpson")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns multiple rows 5",
            intent: "An upsert of five rows writes them all.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES\n					(7, 'Maggie', 'Simpson', 0, 1, 5.1),\n					(8, 'Milhouse', 'Van Houten', 0, 8, 3.5),\n					(9, 'Jacqueline', 'Bouvier', 1, 80, 2),\n					(10, 'Patty', 'Bouvier', 0, 40, 7),\n					(11, 'Selma', 'Bouvier', 0, 40, 7) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE id > 6 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("7"), T("Maggie"), T("Simpson"), T("0"), T("1"), T("5.1")],
                            &[T("8"), T("Milhouse"), T("Van Houten"), T("0"), T("8"), T("3.5")],
                            &[T("9"), T("Jacqueline"), T("Bouvier"), T("1"), T("80"), T("2")],
                            &[T("10"), T("Patty"), T("Bouvier"), T("0"), T("40"), T("7")],
                            &[T("11"), T("Selma"), T("Bouvier"), T("0"), T("40"), T("7")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns multiple rows null pk",
            intent: "An upsert of rows with a NULL primary key is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (0, 'Bart', 'Simpson'), (1, 'Homer', NULL) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"last_name\" of relation \"people\" violates not-null constraint", detail: "Failing row contains (1, Homer, null, null, null, null, null, null).", schema: "public", table: "people", column: "last_name", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns multiple rows duplicate",
            intent: "An upsert of two rows with the same key behaves as Postgres does.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name) VALUES (2, 'Bart', 'Simpson'), (2, 'Bart', 'Simpson') ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "ON CONFLICT DO UPDATE command cannot affect row a second time", hint: "Ensure that no rows proposed for insertion within the same command have duplicate constrained values.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres' ON CONFLICT DO UPDATE refuses to change a row twice in one statement, where MySQL's REPLACE replaces it again")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name FROM people WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns multiple rows replace existing pk",
            intent: "An upsert over an existing key replaces that row and writes the new one.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n				(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n				(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO people (id, first_name, last_name, is_married, age, rating) VALUES\n					(0, 'Homer', 'Simpson', 1, 45, 100),\n					(8, 'Milhouse', 'Van Houten', 0, 8, 100) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"is_married\" = EXCLUDED.\"is_married\", \"age\" = EXCLUDED.\"age\", \"rating\" = EXCLUDED.\"rating\", \"uuid\" = EXCLUDED.\"uuid\", \"num_episodes\" = EXCLUDED.\"num_episodes\";",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, is_married, age, rating FROM people WHERE rating = 100 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("45"), T("100")],
                            &[T("8"), T("Milhouse"), T("Van Houten"), T("0"), T("8"), T("100")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "replace partial columns existing pk",
            intent: "An upsert of an existing key behaves as Postgres does.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "CREATE TABLE temppeople (id bigint PRIMARY KEY, first_name varchar(1023), last_name varchar(1023), num bigint);",
                "INSERT INTO temppeople VALUES (2, 'Bart', 'Simpson', 44);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO temppeople (id, first_name, last_name, num) VALUES (2, 'Bart', 'Simpson', 88) ON CONFLICT (\"id\") DO UPDATE SET \"first_name\" = EXCLUDED.\"first_name\", \"last_name\" = EXCLUDED.\"last_name\", \"num\" = EXCLUDED.\"num\";",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, first_name, last_name, num FROM temppeople WHERE id = 2 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("num", INT8)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("88")],
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
fn test_legacy_select_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "legacy basic select tests",
            intent: "SELECTs with filters, ordering, LIMIT, and OFFSET return the matching rows.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 1 OFFSET 0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 1 OFFSET 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 1 OFFSET 5;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 1 OFFSET 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 0 OFFSET 0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT -1;",
                    expected: Expected::Error(Diagnostic { code: "2201W", message: "LIMIT must not be negative", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT -1,1;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "LIMIT #,# syntax is not supported", hint: "Use separate LIMIT and OFFSET clauses.", position: 22, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people LIMIT 100;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age < 40;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age < 40 LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age < 40 LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age < 40 LIMIT 100;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY rating;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY first_name;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY last_name DESC, first_name ASC;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY first_name LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY last_name DESC, first_name ASC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE 40 > age;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age <= 40;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE 40 >= age;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age > 40;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE 40 < age;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age >= 40;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE 40 <= age;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE last_name > 'Simpson';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE last_name < 'Simpson';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE last_name = 'Simpson';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE rating > 8.0 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE rating < 8.0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE rating = 8.0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE 8.0 < rating;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE 8.0 > rating;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE 8.0 = rating;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married = TRUE;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: bigint = boolean", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married = FALSE;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: bigint = boolean", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married <> FALSE;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: bigint <> boolean", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of WHERE must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married AND age > 38;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of AND must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married OR age < 20;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of OR must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name IN ('Homer', 'Marge');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age IN (-10, 40);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE rating IN (-10.0, 8.5);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name IN ('Homer', 40);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age IN (-10.0, 40);",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name NOT IN ('Homer', 'Marge');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name IN ('Homer');",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name IN (1.0);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = numeric", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE uuid IS NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE uuid IS NOT NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married IS TRUE;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of IS TRUE must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married IS NOT TRUE;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of IS NOT TRUE must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married IS FALSE;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of IS FALSE must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE is_married IS NOT FALSE;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of IS NOT FALSE must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age IS TRUE;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of IS TRUE must be type boolean, not type bigint", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT age + 1 AS a FROM people WHERE is_married ORDER BY a;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of WHERE must be type boolean, not type bigint", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT is_married AND age >= 40 FROM people WHERE last_name = 'Simpson' ORDER BY id LIMIT 2;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of AND must be type boolean, not type bigint", position: 8, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name, age <= 10 OR age >= 40 AS not_marge FROM people WHERE last_name = 'Simpson' ORDER BY id DESC;",
                    expected: Expected::Rows {
                        columns: &[Column("first_name", VARCHAR), Column("not_marge", BOOL)],
                        rows: &[
                            &[T("Lisa"), T("t")],
                            &[T("Bart"), T("t")],
                            &[T("Marge"), T("f")],
                            &[T("Homer"), T("t")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT -age AS age FROM people WHERE is_married ORDER BY age;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "argument of WHERE must be type boolean, not type bigint", position: 38, ..E }),
                    dolt: Some(Dolt::Differs("MySQL treats a number as a boolean, where Postgres needs a boolean")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE -rating = -8.5;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE -first_name = 'Homer';",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: - character varying", hint: "No operator matches the given name and argument type. You might need to add an explicit type cast.", position: 28, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age + 1 = 41;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age - 1 = 39;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age / 2 = 20;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age * 2 = 80;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age % 4 = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age / 4 + 2 * 2 = 14;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name + 1 = 41;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying + integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name - 1 = 39;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying - integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name / 2 = 20;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying / integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name * 2 = 80;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying * integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name % 4 = 0;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying % integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 39, ..E }),
                    dolt: Some(Dolt::Differs("MySQL converts the string to a number, where Postgres has no operator between varchar and a number")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE \"uuid\" IS NOT NULL AND first_name <> 'Marge' ORDER BY last_name DESC, age;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name, last_name FROM people WHERE age >= 40;",
                    expected: Expected::Rows {
                        columns: &[Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("Homer"), T("Simpson")],
                            &[T("Moe"), T("Szyslak")],
                            &[T("Barney"), T("Gumble")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name AS f, last_name AS l FROM people WHERE age >= 40;",
                    expected: Expected::Rows {
                        columns: &[Column("f", VARCHAR), Column("l", VARCHAR)],
                        rows: &[
                            &[T("Homer"), T("Simpson")],
                            &[T("Moe"), T("Szyslak")],
                            &[T("Barney"), T("Gumble")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name AS f, last_name AS f FROM people WHERE age >= 40;",
                    expected: Expected::Rows {
                        columns: &[Column("f", VARCHAR), Column("f", VARCHAR)],
                        rows: &[
                            &[T("Homer"), T("Simpson")],
                            &[T("Moe"), T("Szyslak")],
                            &[T("Barney"), T("Gumble")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name, first_name FROM people WHERE age >= 40 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("first_name", VARCHAR), Column("first_name", VARCHAR)],
                        rows: &[
                            &[T("Homer"), T("Homer")],
                            &[T("Moe"), T("Moe")],
                            &[T("Barney"), T("Barney")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name AS f, last_name AS f FROM people, people WHERE age >= 40;",
                    expected: Expected::Error(Diagnostic { code: "42712", message: "table name \"people\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people p, people p WHERE age >= 40;",
                    expected: Expected::Error(Diagnostic { code: "42712", message: "table name \"p\" specified more than once", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name FROM people ORDER BY age, first_name;",
                    expected: Expected::Rows {
                        columns: &[Column("first_name", VARCHAR)],
                        rows: &[
                            &[T("Lisa")],
                            &[T("Bart")],
                            &[T("Marge")],
                            &[T("Barney")],
                            &[T("Homer")],
                            &[T("Moe")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT first_name AS f FROM people ORDER BY age, f;",
                    expected: Expected::Rows {
                        columns: &[Column("f", VARCHAR)],
                        rows: &[
                            &[T("Lisa")],
                            &[T("Bart")],
                            &[T("Marge")],
                            &[T("Barney")],
                            &[T("Homer")],
                            &[T("Moe")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p.first_name AS f, p.last_name AS l FROM people p WHERE p.first_name = 'Homer';",
                    expected: Expected::Rows {
                        columns: &[Column("f", VARCHAR), Column("l", VARCHAR)],
                        rows: &[
                            &[T("Homer"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT p.first_name, p.last_name FROM people p WHERE p.first_name = 'Homer';",
                    expected: Expected::Rows {
                        columns: &[Column("first_name", VARCHAR), Column("last_name", VARCHAR)],
                        rows: &[
                            &[T("Homer"), T("Simpson")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT m.first_name AS f, p.last_name AS l FROM people p WHERE p.f = 'Homer';",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "missing FROM-clause entry for table \"m\"", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id AS i, first_name AS f, last_name AS l, is_married AS m, age AS a,\n						rating AS r, uuid AS u, num_episodes AS n FROM people WHERE age >= 40;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT8), Column("f", VARCHAR), Column("l", VARCHAR), Column("m", INT8), Column("a", INT8), Column("r", FLOAT8), Column("u", VARCHAR), Column("n", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age <> 40;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE age > 80;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, age FROM people WHERE age > 80;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("age", INT8)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dne;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: "relation \"dne\" does not exist", position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people JOIN dne;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \";\"", position: 30, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE dne > 8.0;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"dne\" does not exist", position: 28, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE rating > 8.0 ORDER BY dne;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"dne\" does not exist", position: 50, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE FUNCTION(first_name);",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function function(character varying) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 28, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = '0';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
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
fn test_legacy_update_script_tests() {
    run_scripts(&[
        ScriptTest {
            name: "update one row, one col, primary key where clause",
            intent: "An UPDATE of one column of the row a key filter picks changes only it.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Domer' WHERE id = 0;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Domer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update one row, one col, non-primary key where clause",
            intent: "An UPDATE of one column of the row another column's filter picks changes only it.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Domer' WHERE first_name = 'Homer';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name = 'Domer';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Domer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update one row, two cols, primary key where clause",
            intent: "An UPDATE of two columns of the row a key filter picks changes only them.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Ned', last_name = 'Flanders' WHERE id = 0;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Ned"), T("Flanders"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update one row, all cols, non-primary key where clause",
            intent: "An UPDATE of every column of one row changes the whole row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Ned', last_name = 'Flanders', is_married = 0, rating = 10,\n				age = 45, num_episodes = 150, uuid = '00000000-0000-0000-0000-000000000050'\n				WHERE age = 38;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE uuid = '00000000-0000-0000-0000-000000000050';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("1"), T("Ned"), T("Flanders"), T("0"), T("45"), T("10"), T("00000000-0000-0000-0000-000000000050"), T("150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update one row, set columns to existing values",
            intent: "An UPDATE that sets a row's columns to their own values leaves it unchanged.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Homer', last_name = 'Simpson', is_married = 1, rating = 8.5, age = 40,\n				num_episodes = NULL, uuid = NULL WHERE id = 0;",
                    expected: Expected::Ok,
                    dolt: Some(Dolt::MySqlOnly("MySQL's TRUE and FALSE are 1 and 0, which the statement writes into the bigint column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE id = 0;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update one row, null out existing values",
            intent: "An UPDATE sets a row's nullable columns to NULL.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Homer', last_name = 'Simpson', is_married = NULL, rating = NULL, age = NULL,\n				num_episodes = NULL, uuid = NULL WHERE first_name = 'Homer';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE first_name = 'Homer';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), Null, Null, Null, Null, Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update multiple rows, set two columns",
            intent: "An UPDATE of two columns changes every matching row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Changed', rating = 0.0 WHERE last_name = 'Simpson';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE last_name = 'Simpson';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Changed"), T("Simpson"), T("1"), T("40"), T("0"), Null, Null],
                            &[T("1"), T("Changed"), T("Simpson"), T("1"), T("38"), T("0"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Changed"), T("Simpson"), T("0"), T("10"), T("0"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Changed"), T("Simpson"), T("0"), T("8"), T("0"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update no matching rows",
            intent: "An UPDATE that matches no row changes nothing.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Changed', rating = 0.0 WHERE last_name = 'Flanders';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update without where clause",
            intent: "An UPDATE without WHERE changes every row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Changed', rating = 0.0;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Changed"), T("Simpson"), T("1"), T("40"), T("0"), Null, Null],
                            &[T("1"), T("Changed"), T("Simpson"), T("1"), T("38"), T("0"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Changed"), T("Simpson"), T("0"), T("10"), T("0"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Changed"), T("Simpson"), T("0"), T("8"), T("0"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Changed"), T("Szyslak"), T("0"), T("48"), T("0"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Changed"), T("Gumble"), T("0"), T("40"), T("0"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update set first_name = last_name",
            intent: "An UPDATE sets a column from another column of the same row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = last_name;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Simpson"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Simpson"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Simpson"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Simpson"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Szyslak"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Gumble"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update increment age",
            intent: "An UPDATE increments a column of every row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET age = age + 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("41"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("39"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("11"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("9"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("49"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("41"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update reverse rating",
            intent: "An UPDATE negates a column of every row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET rating = -rating;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("-8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("-8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("-9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("-10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("-6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("-4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update datetime field",
            intent: "An UPDATE sets a timestamp column.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE episodes SET air_date = '1993-03-24 20:00:00' WHERE id = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM episodes WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("name", VARCHAR), Column("air_date", TIMESTAMP), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("1"), T("Simpsons Roasting On an Open Fire"), T("1993-03-24 20:00:00"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update name field",
            intent: "An UPDATE sets a text column.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE episodes SET name = 'fake_name' WHERE id = 1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM episodes WHERE id = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("name", VARCHAR), Column("air_date", TIMESTAMP), Column("rating", FLOAT8)],
                        rows: &[
                            &[T("1"), T("fake_name"), T("1989-12-18 03:00:00"), T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update multiple rows, =",
            intent: "An UPDATE filtered with = changes every matching row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = 'Homer' WHERE last_name = 'Simpson';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people WHERE last_name = 'Simpson';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Homer"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Homer"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Homer"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update multiple rows, <>",
            intent: "An UPDATE filtered with <> changes every matching row.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET last_name = 'Simpson' WHERE last_name <> 'Simpson';",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Simpson"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Simpson"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update multiple rows pk increment order by desc",
            intent: "Incrementing every primary key in one UPDATE behaves as Postgres' unique checks allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET id = id + 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"people_pkey\"", detail: "Key (id)=(1) already exists.", schema: "public", table: "people", constraint: "people_pkey", ..E }),
                    dolt: Some(Dolt::Differs("MySQL's UPDATE takes ORDER BY, changing the rows from the highest key down, where Postgres' has no order and checks the key as each row changes")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT8), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT8), Column("age", INT8), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", NUMERIC)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update multiple rows pk increment order by asc",
            intent: "Incrementing every primary key in ascending order behaves as Postgres' unique checks allow.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET id = id + 1 ORDER BY id ASC;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "syntax error at or near \"ORDER\"", position: 31, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update primary key col",
            intent: "An UPDATE changes a row's primary key.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET id = 0 WHERE first_name = 'Marge';",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"people_pkey\"", detail: "Key (id)=(0) already exists.", schema: "public", table: "people", constraint: "people_pkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "update null constraint failure",
            intent: "An UPDATE that sets a NOT NULL column to NULL is refused.",
            set_up_script: &[
                "CREATE TABLE people (id bigint PRIMARY KEY, first_name varchar(1024) NOT NULL, last_name varchar(1024) NOT NULL, is_married bigint, age bigint, rating double precision, uuid varchar(1024), num_episodes numeric(20));",
                "CREATE TABLE episodes (id bigint PRIMARY KEY, name varchar(1024) NOT NULL, air_date timestamp, rating double precision);",
                "CREATE TABLE appearances (character_id bigint NOT NULL, episode_id bigint NOT NULL, comments varchar(1024), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES\n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8.0, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9.0, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10.0, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4.0, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES\n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "UPDATE people SET first_name = NULL WHERE id = 0;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "null value in column \"first_name\" of relation \"people\" violates not-null constraint", detail: "Failing row contains (0, null, Simpson, 1, 40, 8.5, null, null).", schema: "public", table: "people", column: "first_name", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
