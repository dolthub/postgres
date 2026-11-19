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

//! Dolt's go/libraries/doltcore/sqle/enginetest/ddl_queries.go, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_modify_and_change_column_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "alter modify column reorder middle",
            intent: "A column whose type is restated keeps its rows, and its place, since Postgres can't move it.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people ALTER COLUMN first_name TYPE varchar(163), ALTER COLUMN first_name SET NOT NULL, ALTER COLUMN first_name DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("first_name"), T("character varying"), T("NO")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("rating"), T("double precision"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT2), Column("age", INT4), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", INT4)],
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
                    dolt: Some(Dolt::MySqlOnly("MODIFY and CHANGE COLUMN ... FIRST or AFTER move a column, which Postgres can't, so the columns keep their order")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column reorder first",
            intent: "A column restated to move first keeps its rows and place.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people ALTER COLUMN first_name TYPE varchar(163), ALTER COLUMN first_name SET NOT NULL, ALTER COLUMN first_name DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("first_name"), T("character varying"), T("NO")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("rating"), T("double precision"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT2), Column("age", INT4), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", INT4)],
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
                    dolt: Some(Dolt::MySqlOnly("MODIFY and CHANGE COLUMN ... FIRST or AFTER move a column, which Postgres can't, so the columns keep their order")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column drop null constraint",
            intent: "Dropping a column's NOT NULL lets the column hold NULLs.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people ALTER COLUMN first_name TYPE varchar(163), ALTER COLUMN first_name DROP NOT NULL, ALTER COLUMN first_name DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("first_name"), T("character varying"), T("YES")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("rating"), T("double precision"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id;",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter change column rename and reorder",
            intent: "Renaming a column keeps its rows and place.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people RENAME COLUMN first_name TO christian_name; ALTER TABLE people ALTER COLUMN christian_name TYPE varchar(163), ALTER COLUMN christian_name SET NOT NULL, ALTER COLUMN christian_name DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("christian_name"), T("character varying"), T("NO")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("rating"), T("double precision"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("christian_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT2), Column("age", INT4), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", INT4)],
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
                    dolt: Some(Dolt::MySqlOnly("MODIFY and CHANGE COLUMN ... FIRST or AFTER move a column, which Postgres can't, so the columns keep their order")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter change column rename and reorder first",
            intent: "Renaming a column moved first keeps its rows and place.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people RENAME COLUMN first_name TO christian_name; ALTER TABLE people ALTER COLUMN christian_name TYPE varchar(163), ALTER COLUMN christian_name SET NOT NULL, ALTER COLUMN christian_name DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("christian_name"), T("character varying"), T("NO")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("rating"), T("double precision"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("christian_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT2), Column("age", INT4), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", INT4)],
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
                    dolt: Some(Dolt::MySqlOnly("MODIFY and CHANGE COLUMN ... FIRST or AFTER move a column, which Postgres can't, so the columns keep their order")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter change column drop null constraint",
            intent: "Renaming a column and dropping its NOT NULL lets it hold NULLs.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people ALTER COLUMN first_name TYPE varchar(163), ALTER COLUMN first_name DROP NOT NULL, ALTER COLUMN first_name DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("first_name"), T("character varying"), T("YES")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("rating"), T("double precision"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 8",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY id;",
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column not null with type mismatch in default",
            intent: "Setting a column NOT NULL with a default its type can't hold fails.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people ALTER COLUMN rating TYPE double precision, ALTER COLUMN rating DROP NOT NULL, ALTER COLUMN rating SET DEFAULT 'not a number';",
                    expected: Expected::Error(Diagnostic { code: "22P02", message: "invalid input syntax for type double precision: \"not a number\"", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column not null, existing null values",
            intent: "Setting NOT NULL on a column that holds NULLs fails.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people ALTER COLUMN num_episodes TYPE bigint, ALTER COLUMN num_episodes SET NOT NULL, ALTER COLUMN num_episodes DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "23502", message: "column \"num_episodes\" of relation \"people\" contains null values", schema: "public", table: "people", column: "num_episodes", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter, rename primary key column",
            intent: "A renamed primary key column keeps its key with its changed type.",
            set_up_script: &[
                "CREATE TABLE t (pk1 varchar(100), pk2 varchar(50), PRIMARY KEY (pk1, pk2));",
                "ALTER TABLE t RENAME COLUMN pk2 TO pkTwo; ALTER TABLE t ALTER COLUMN pkTwo TYPE varchar(20), ALTER COLUMN pkTwo DROP DEFAULT;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 't' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk1"), T("character varying"), T("NO")],
                            &[T("pktwo"), T("character varying"), T("NO")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_modify_column_type_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "alter modify column type similar types",
            intent: "Widening an integer column keeps its rows and its index's lookups.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint); CREATE INDEX  ON test (v1);",
                "INSERT INTO test VALUES (0, 3), (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 TYPE int, ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("v1"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT4)],
                        rows: &[
                            &[T("0"), T("3")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", INT4)],
                        rows: &[
                            &[T("0"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type different types",
            intent: "Changing an integer column to varchar keeps its values as text and its index's lookups.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint); CREATE INDEX  ON test (v1);",
                "INSERT INTO test VALUES (0, 3), (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 TYPE varchar(20), ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("v1"), T("character varying"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("3")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 = '3';",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type different types reversed",
            intent: "Changing a varchar column to an integer needs a USING expression.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 varchar(20)); CREATE INDEX  ON test (v1);",
                "INSERT INTO test VALUES (0, '3'), (1, '2');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 TYPE bigint, ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"v1\" cannot be cast automatically to type bigint", hint: "You might need to specify \"USING v1::bigint\".", ..E }),
                    dolt: Some(Dolt::Differs("Postgres changes a column's type only through an assignment cast or a USING expression, so the change fails, where MySQL converts the stored values")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("v1"), T("character varying"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", VARCHAR)],
                        rows: &[
                            &[T("0"), T("3")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 = 3;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "operator does not exist: character varying = integer", hint: "No operator matches the given name and argument types. You might need to add explicit type casts.", position: 29, ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type primary key",
            intent: "Changing a primary key column's type keeps its rows and lookups.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint); CREATE INDEX  ON test (v1);",
                "INSERT INTO test VALUES (0, 3), (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN pk TYPE varchar(20), ALTER COLUMN pk DROP DEFAULT;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("character varying"), T("NO")],
                            &[T("v1"), T("bigint"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("v1", INT8)],
                        rows: &[
                            &[T("0"), T("3")],
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("the key's type is now varchar, as MySQL's MODIFY makes it")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", VARCHAR), Column("v1", INT8)],
                        rows: &[
                            &[T("0"), T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("the key's type is now varchar, as MySQL's MODIFY makes it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type incompatible types with empty table",
            intent: "Changing a column to a type its own has no cast to fails, even on an empty table.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bit(20)); CREATE INDEX  ON test (v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN pk TYPE timestamp(6), ALTER COLUMN pk DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"pk\" cannot be cast automatically to type timestamp without time zone", hint: "You might need to specify \"USING pk::timestamp(6) without time zone\".", ..E }),
                    dolt: Some(Dolt::Differs("Postgres changes a column's type only through an assignment cast or a USING expression, so the change fails, where MySQL converts the stored values")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("pk"), T("bigint"), T("NO")],
                            &[T("v1"), T("bit"), T("YES")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT8), Column("v1", BIT)],
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
            name: "alter modify column type incompatible types with non-empty table",
            intent: "Changing a column to a type its own has no cast to fails on a table with rows.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bit(20)); CREATE INDEX  ON test (v1);",
                "INSERT INTO test VALUES (1, 1::bit(20));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN pk TYPE timestamp, ALTER COLUMN pk DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"pk\" cannot be cast automatically to type timestamp without time zone", hint: "You might need to specify \"USING pk::timestamp without time zone\".", ..E }),
                    dolt: Some(Dolt::Differs("Postgres changes a column's type only through an assignment cast or a USING expression, so the change fails, where MySQL converts the stored values")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type different types incompatible values",
            intent: "Changing a varchar column whose values aren't numbers to an integer fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 varchar(20)); CREATE INDEX  ON test (v1);",
                "INSERT INTO test VALUES (0, 3), (1, 'a');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 TYPE bigint, ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"v1\" cannot be cast automatically to type bigint", hint: "You might need to specify \"USING v1::bigint\".", ..E }),
                    dolt: Some(Dolt::Differs("Postgres changes a column's type only through an assignment cast or a USING expression, so the change fails, where MySQL converts the stored values")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type foreign key parent",
            intent: "Changing the type of a column a foreign key references fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON test (v1);",
                "CREATE TABLE test2(pk bigint PRIMARY KEY, v1 bigint, FOREIGN KEY (v1) REFERENCES test(v1)); CREATE INDEX  ON test2 (v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test ALTER COLUMN v1 TYPE varchar(20), ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "foreign key constraint \"test2_v1_fkey\" cannot be implemented", detail: "Key columns \"v1\" of the referencing table and \"v1\" of the referenced table are of incompatible types: bigint and character varying.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses a type change that leaves a foreign key's columns of incompatible types, with its own error")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type foreign key child",
            intent: "Changing the type of a foreign key's column away from its parent's fails.",
            set_up_script: &[
                "CREATE TABLE test(pk bigint PRIMARY KEY, v1 bigint); CREATE UNIQUE INDEX ON test (v1);",
                "CREATE TABLE test2(pk bigint PRIMARY KEY, v1 bigint, FOREIGN KEY (v1) REFERENCES test(v1)); CREATE INDEX  ON test2 (v1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test2 ALTER COLUMN v1 TYPE varchar(20), ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "foreign key constraint \"test2_v1_fkey\" cannot be implemented", detail: "Key columns \"v1\" of the referencing table and \"v1\" of the referenced table are of incompatible types: character varying and bigint.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses a type change that leaves a foreign key's columns of incompatible types, with its own error")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter modify column type, make primary key spatial",
            intent: "Changing an integer primary key column to a geometric type fails.",
            set_up_script: &[
                "CREATE TABLE point_tbl (p int PRIMARY KEY);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE point_tbl ALTER COLUMN p TYPE point;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "column \"p\" cannot be cast automatically to type point", hint: "You might need to specify \"USING p::point\".", ..E }),
                    dolt: Some(Dolt::MySqlOnly("MODIFY COLUMN ... PRIMARY KEY repeats the key, which Postgres leaves out; the column keeps its primary key")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_drop_column_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "alter drop column",
            intent: "Dropping a column removes it and its values from the table.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people DROP rating;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("first_name"), T("character varying"), T("NO")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT2), Column("age", INT4), Column("uuid", VARCHAR), Column("num_episodes", INT4)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter drop column with optional column keyword",
            intent: "DROP COLUMN with the COLUMN keyword drops the column.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people DROP COLUMN rating;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("id"), T("integer"), T("NO")],
                            &[T("first_name"), T("character varying"), T("NO")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT2), Column("age", INT4), Column("uuid", VARCHAR), Column("num_episodes", INT4)],
                        rows: &[
                            &[T("0"), T("Homer"), T("Simpson"), T("1"), T("40"), Null, Null],
                            &[T("1"), T("Marge"), T("Simpson"), T("1"), T("38"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("2"), T("Bart"), T("Simpson"), T("0"), T("10"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("3"), T("Lisa"), T("Simpson"), T("0"), T("8"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("4"), T("Moe"), T("Szyslak"), T("0"), T("48"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                            &[T("5"), T("Barney"), T("Gumble"), T("0"), T("40"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop primary key column",
            intent: "Dropping the primary key column leaves the other columns' rows.",
            set_up_script: &[
                "CREATE TABLE people (id int PRIMARY KEY, first_name varchar(100) NOT NULL, last_name varchar(100) NOT NULL, is_married smallint, age int, rating float, uuid varchar(64), num_episodes int);",
                "CREATE TABLE episodes (id int PRIMARY KEY, name varchar(100) NOT NULL, air_date timestamp, rating float);",
                "CREATE TABLE appearances (character_id int NOT NULL, episode_id int NOT NULL, comments varchar(100), PRIMARY KEY (character_id, episode_id));",
                "INSERT INTO people VALUES \n		(0, 'Homer', 'Simpson', 1, 40, 8.5, NULL, NULL),\n		(1, 'Marge', 'Simpson', 1, 38, 8, '00000000-0000-0000-0000-000000000001', 111),\n		(2, 'Bart', 'Simpson', 0, 10, 9, '00000000-0000-0000-0000-000000000002', 222),\n		(3, 'Lisa', 'Simpson', 0, 8, 10, '00000000-0000-0000-0000-000000000003', 333),\n		(4, 'Moe', 'Szyslak', 0, 48, 6.5, '00000000-0000-0000-0000-000000000004', 444),\n		(5, 'Barney', 'Gumble', 0, 40, 4, '00000000-0000-0000-0000-000000000005', 555);",
                "INSERT INTO episodes VALUES \n		(1, 'Simpsons Roasting On an Open Fire', '1989-12-18 03:00:00', 8.0),\n		(2, 'Bart the Genius', '1990-01-15 03:00:00', 9.0),\n		(3, 'Homer''s Odyssey', '1990-01-22 03:00:00', 7.0),\n		(4, 'There''s No Disgrace Like Home', '1990-01-29 03:00:00', 8.5);",
                "INSERT INTO appearances VALUES\n		(0, 1, 'Homer is great in this one'),\n		(1, 1, 'Marge is here too'),\n		(0, 2, 'Homer is great in this one too'),\n		(2, 2, 'This episode is named after Bart'),\n		(3, 2, 'Lisa is here too'),\n		(4, 2, 'I think there''s a prank call scene'),\n		(0, 3, 'Homer is in every episode'),\n		(1, 3, 'Marge shows up a lot too'),\n		(3, 3, 'Lisa is the best Simpson'),\n		(5, 3, 'I''m making this all up');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE people DROP COLUMN id;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'people' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("first_name"), T("character varying"), T("NO")],
                            &[T("last_name"), T("character varying"), T("NO")],
                            &[T("is_married"), T("smallint"), T("YES")],
                            &[T("age"), T("integer"), T("YES")],
                            &[T("rating"), T("double precision"), T("YES")],
                            &[T("uuid"), T("character varying"), T("YES")],
                            &[T("num_episodes"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 7",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM people ORDER BY first_name;",
                    expected: Expected::Rows {
                        columns: &[Column("first_name", VARCHAR), Column("last_name", VARCHAR), Column("is_married", INT2), Column("age", INT4), Column("rating", FLOAT8), Column("uuid", VARCHAR), Column("num_episodes", INT4)],
                        rows: &[
                            &[T("Barney"), T("Gumble"), T("0"), T("40"), T("4"), T("00000000-0000-0000-0000-000000000005"), T("555")],
                            &[T("Bart"), T("Simpson"), T("0"), T("10"), T("9"), T("00000000-0000-0000-0000-000000000002"), T("222")],
                            &[T("Homer"), T("Simpson"), T("1"), T("40"), T("8.5"), Null, Null],
                            &[T("Lisa"), T("Simpson"), T("0"), T("8"), T("10"), T("00000000-0000-0000-0000-000000000003"), T("333")],
                            &[T("Marge"), T("Simpson"), T("1"), T("38"), T("8"), T("00000000-0000-0000-0000-000000000001"), T("111")],
                            &[T("Moe"), T("Szyslak"), T("0"), T("48"), T("6.5"), T("00000000-0000-0000-0000-000000000004"), T("444")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_broken_ddl_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "drop first of two primary key columns",
            intent: "Dropping one column of a two-column primary key leaves the rows.",
            set_up_script: &[
                "CREATE TABLE test (p1 int, p2 int, c1 int, c2 int); CREATE INDEX  ON test (c1);",
                "INSERT INTO test VALUES (0, 1, 2, 3), (4, 5, 6, 7);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE test DROP COLUMN p1;",
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'test' ORDER BY ordinal_position;",
                    expected: Expected::Rows {
                        columns: &[Column("column_name", NAME), Column("data_type", VARCHAR), Column("is_nullable", VARCHAR)],
                        rows: &[
                            &[T("p2"), T("integer"), T("YES")],
                            &[T("c1"), T("integer"), T("YES")],
                            &[T("c2"), T("integer"), T("YES")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("SHOW CREATE TABLE is MySQL's; information_schema.columns shows the table's columns")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test ORDER BY pk;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"pk\" does not exist", hint: "Perhaps you meant to reference the column \"test.p2\".", position: 29, ..E }),
                    dolt: Some(Dolt::Bug("Dolt's expectation is right, but Dolt main fails it and keeps the script among its BrokenDDLScripts")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test WHERE v1 = 3;",
                    expected: Expected::Error(Diagnostic { code: "42703", message: "column \"v1\" does not exist", hint: "Perhaps you meant to reference the column \"test.c1\".", position: 26, ..E }),
                    dolt: Some(Dolt::Bug("Dolt's expectation is right, but Dolt main fails it and keeps the script among its BrokenDDLScripts")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter string column to truncate data",
            intent: "Shortening a varchar column below its values' length fails.",
            set_up_script: &[
                "CREATE TABLE t1 (a int PRIMARY KEY, b varchar(3));",
                "INSERT INTO t1 VALUES (1, 'hi'), (2, 'bye');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t1 ALTER COLUMN b TYPE varchar(2), ALTER COLUMN b DROP NOT NULL, ALTER COLUMN b DROP DEFAULT;",
                    expected: Expected::Error(Diagnostic { code: "22001", message: "value too long for type character varying(2)", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses a shorter varchar over longer values with its value too long error")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "alter datetime column with invalid values",
            intent: "Changing a timestamp column to timestamp keeps values far in the future.",
            set_up_script: &[
                "CREATE TABLE t3(pk bigint PRIMARY KEY, v1 timestamp); CREATE INDEX  ON t3 (v1);",
                "INSERT INTO t3 VALUES (0,'1999-11-02 17:39:38'),(1,'3021-01-08 02:59:27');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE t3 ALTER COLUMN v1 TYPE timestamp, ALTER COLUMN v1 DROP NOT NULL, ALTER COLUMN v1 DROP DEFAULT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::Differs("MySQL's TIMESTAMP ends in 2038, so its change fails on the year 3021, where Postgres' timestamp holds it")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "table with commit column should maintain its data in diff",
            intent: "dolt_diff of a table with a column named commit shows that column's values.",
            set_up_script: &[
                "CREATE TABLE t (pk int PRIMARY KEY, COMMIT text);",
                "SELECT dolt_add('.');",
                "SELECT set_config('test.Commit1', (dolt_commit('-am', 'creating table t'))::text, FALSE);",
                "INSERT INTO t VALUES (1, 'hi');",
                "SELECT set_config('test.Commit2', (dolt_commit('-am', 'insert data'))::text, FALSE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT to_pk, to_commit, from_pk, from_commit, dolt_diff_type FROM dolt_diff(current_setting('test.Commit1', TRUE), current_setting('test.Commit2', TRUE), 't');",
                    expected: Expected::Rows {
                        columns: &[Column("to_pk", INT4), Column("to_commit", TEXT), Column("from_pk", INT4), Column("from_commit", TEXT), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("1"), T("hi"), Null, Null, T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug("Dolt's expectation is right, but Dolt main fails it, its own to_commit hiding the table's, and keeps the script among its BrokenDDLScripts. Doltgrust's own columns carry the dolt_ prefix (Appendix C)")),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_add_index_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "duplicate key after adding a column to a table with an expression index",
            intent: "After adding a column to a table with expression indexes, duplicate keys are refused, skipped, or updated.",
            set_up_script: &[
                "CREATE TABLE test (id int PRIMARY KEY, name text UNIQUE);",
                "CREATE INDEX lower_name ON test ((lower(name)));",
                "CREATE INDEX upper_name ON test ((upper(name)));",
                "ALTER TABLE test ADD COLUMN extra timestamp;",
                "INSERT INTO test (id, name) VALUES (1, 'v1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test (id, name) VALUES (1, 'duplicate');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_pkey\"", detail: "Key (id)=(1) already exists.", schema: "public", table: "test", constraint: "test_pkey", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (id, name) VALUES (2, 'v1');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"test_name_key\"", detail: "Key (name)=(v1) already exists.", schema: "public", table: "test", constraint: "test_name_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (id, name) VALUES (1, 'ignored') ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test (id, name) VALUES (1, 'updated') ON CONFLICT (id) DO UPDATE SET name = excluded.name;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("ON DUPLICATE KEY UPDATE ... VALUES(name) is ON CONFLICT ... DO UPDATE SET name = excluded.name, whose tag counts the row once where MySQL counts an update twice")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, name, extra FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("extra", TIMESTAMP)],
                        rows: &[
                            &[T("1"), T("updated"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "duplicate unique key on keyless table after adding a column with an expression index",
            intent: "After adding a column to a table without a primary key, duplicates of its unique column are refused, skipped, or updated.",
            set_up_script: &[
                "CREATE TABLE keyless_test (id int, name text UNIQUE);",
                "CREATE INDEX keyless_lower_name ON keyless_test ((lower(name)));",
                "ALTER TABLE keyless_test ADD COLUMN extra timestamp;",
                "INSERT INTO keyless_test (id, name) VALUES (1, 'v1');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO keyless_test (id, name) VALUES (2, 'v1');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "duplicate key value violates unique constraint \"keyless_test_name_key\"", detail: "Key (name)=(v1) already exists.", schema: "public", table: "keyless_test", constraint: "keyless_test_name_key", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless_test (id, name) VALUES (2, 'v1') ON CONFLICT DO NOTHING;",
                    expected: Expected::Tag("INSERT 0 0"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO keyless_test (id, name) VALUES (3, 'v1') ON CONFLICT (name) DO UPDATE SET id = excluded.id;",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("ON DUPLICATE KEY UPDATE ... VALUES(id) is ON CONFLICT ... DO UPDATE SET id = excluded.id, whose tag counts the row once where MySQL counts an update twice")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, name, extra FROM keyless_test;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("extra", TIMESTAMP)],
                        rows: &[
                            &[T("3"), T("v1"), Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "add unique constraint on keyless table",
            intent: "Adding a unique index over duplicate values of a table without a primary key fails.",
            set_up_script: &[
                "CREATE TABLE test (uk int);",
                "INSERT INTO test VALUES (0), (0);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX m ON test (uk);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: "could not create unique index \"m\"", detail: "Key (uk)=(0) is duplicated.", schema: "public", table: "test", constraint: "m", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_add_drop_primary_keys_scripts() {
    run_scripts(&[
        ScriptTest {
            name: "drop primary key blocked when foreign key present",
            intent: "Dropping a primary key a foreign key references fails.",
            set_up_script: &[
                "CREATE TABLE parent (a int PRIMARY KEY);",
                "CREATE TABLE child (b int PRIMARY KEY, c int); CREATE INDEX  ON child (c);",
                "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (c) REFERENCES parent (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE parent DROP CONSTRAINT parent_pkey;",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop constraint parent_pkey on table parent because other objects depend on it", detail: "constraint fk1 on table child depends on index parent_pkey", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    dolt: Some(Dolt::Differs("Postgres refuses to drop a primary key a foreign key depends on with its dependency error")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop primary key succeeds when foreign key present on other column",
            intent: "Dropping a primary key succeeds when the table's foreign key references another column.",
            set_up_script: &[
                "CREATE TABLE parent (a int PRIMARY KEY, d int); CREATE UNIQUE INDEX ON parent (d);",
                "CREATE TABLE child (b int PRIMARY KEY, c int); CREATE INDEX  ON child (c);",
                "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (c) REFERENCES parent (d);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE parent DROP CONSTRAINT parent_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "drop primary key succeeds when foreign key present on other table",
            intent: "Dropping a primary key succeeds when a foreign key on another table references another column.",
            set_up_script: &[
                "CREATE TABLE unrelated (a int PRIMARY KEY, d int);",
                "CREATE TABLE parent (a int PRIMARY KEY);",
                "CREATE TABLE child (b int PRIMARY KEY, c int); CREATE INDEX  ON child (c);",
                "ALTER TABLE child ADD CONSTRAINT fk1 FOREIGN KEY (c) REFERENCES parent (a);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "ALTER TABLE unrelated DROP CONSTRAINT unrelated_pkey;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
