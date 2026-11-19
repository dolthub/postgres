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

//! Tests that DROP ROLE checks only the current branch, as DROP OWNED and REASSIGN OWNED do, and that another
//! branch's objects of a dropped role keep naming it, read as a role that no longer exists.

use harness::conn::{connect, rows};
use harness::server::{Server, Target};

#[test]
#[ignore = "a session cannot create or connect to another branch of a database yet"]
fn test_objects_in_another_branch_name_a_dropped_role() {
    let target = Target::from_env().unwrap_or_else(|err| panic!("{err}"));
    let server = Server::start(&target, "").unwrap();
    let mut main = connect(&server, "postgres", "postgres");
    main.exec("CREATE ROLE rd_role", &[]).unwrap();
    main.exec("SELECT dolt_branch('other')", &[]).unwrap();
    let mut other = connect(&server, "postgres/other", "postgres");
    other.exec("CREATE TABLE rd_t (a INT)", &[]).unwrap();
    other.exec("ALTER TABLE rd_t OWNER TO rd_role", &[]).unwrap();
    other.exec("CREATE TABLE rd_u (a INT)", &[]).unwrap();
    other.exec("GRANT SELECT ON rd_u TO rd_role", &[]).unwrap();
    other.exec("CREATE TABLE rd_r (r REGROLE)", &[]).unwrap();
    other.exec("INSERT INTO rd_r VALUES ('rd_role')", &[]).unwrap();
    main.exec("DROP ROLE rd_role", &[]).unwrap();
    other.close();
    let mut other = connect(&server, "postgres/other", "postgres");
    let owner = "SELECT relowner >= 4293918720, pg_get_userbyid(relowner) = 'unknown (OID=' || relowner || ')' \
                 FROM pg_class WHERE relname = 'rd_t'";
    assert_eq!(rows(&mut other, owner), [["t", "t"]]);
    let grantee = "SELECT count(*) FROM aclexplode((SELECT relacl FROM pg_class WHERE relname = 'rd_u')) a \
                   WHERE a.privilege_type = 'SELECT' \
                   AND a.grantee = (SELECT relowner FROM pg_class WHERE relname = 'rd_t')";
    assert_eq!(rows(&mut other, grantee), [["1"]]);
    let regrole = "SELECT r::OID = (SELECT relowner FROM pg_class WHERE relname = 'rd_t'), r::TEXT = r::OID::TEXT \
                   FROM rd_r";
    assert_eq!(rows(&mut other, regrole), [["t", "t"]]);
    other.exec("ALTER TABLE rd_t ADD COLUMN b INT", &[]).unwrap();
    other.exec("GRANT INSERT ON rd_u TO pg_monitor", &[]).unwrap();
    other.exec("UPDATE rd_r SET r = r", &[]).unwrap();
    main.exec("CREATE ROLE rd_role", &[]).unwrap();
    other.close();
    let mut other = connect(&server, "postgres/other", "postgres");
    let named = "SELECT pg_get_userbyid(relowner) FROM pg_class WHERE relname = 'rd_t'";
    assert_eq!(rows(&mut other, named), [["rd_role"]]);
    let grantees = "SELECT a.grantee::REGROLE::TEXT \
                    FROM aclexplode((SELECT relacl FROM pg_class WHERE relname = 'rd_u')) a \
                    WHERE a.privilege_type = 'SELECT' ORDER BY 1";
    assert_eq!(rows(&mut other, grantees), [["postgres"], ["rd_role"]]);
    assert_eq!(rows(&mut other, "SELECT r::TEXT FROM rd_r"), [["rd_role"]]);
}
