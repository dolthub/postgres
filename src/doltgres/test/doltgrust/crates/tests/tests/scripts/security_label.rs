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
fn test_security_label() {
    run_scripts(&[
        ScriptTest {
            name: "SECURITY LABEL finds no label provider, since none is loaded",
            set_up_script: &[
                "CREATE ROLE sl_user;",
                "CREATE TABLE sl (a int);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SECURITY LABEL ON TABLE sl IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no security label providers have been loaded", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SECURITY LABEL FOR dummy ON TABLE sl IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"security label provider "dummy" is not loaded"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SECURITY LABEL ON ROLE sl_user IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no security label providers have been loaded", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SECURITY LABEL ON TABLE nope IS 'x';",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "no security label providers have been loaded", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SECURITY LABEL FOR selinux ON DATABASE postgres IS NULL;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: r#"security label provider "selinux" is not loaded"#, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}
