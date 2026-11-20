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

//! Prepared runs of script tests, as Dolt's prepared runner makes them: a statement's constants become parameters of
//! the types Postgres gives the constants, bound to their values.
//!
//! The fork's replacement for Doltgrust's crates/harness/src/prepared.rs at 1cc06b328cd9, which finds a statement's
//! constants with Doltgrust's `parser` and `nodes` crates. The fork's harness hasn't ported prepared runs yet (pending:
//! the prepared mode, on pg_query), so a run that asks for one stops instead of running unprepared.

use crate::pgx::Arg;

/// Bound is a statement with its constants replaced by parameters: its text, its parameters' types, and their values.
pub type Bound = (String, Vec<u32>, Vec<Arg>);

/// bind would return a statement with its constants replaced by parameters, and stops the run, since the fork's harness
/// has no prepared runs yet.
pub fn bind(_sql: &str) -> Option<Bound> {
    panic!("the fork's harness has no prepared runs yet; unset {}", crate::script::PREPARED_ENV);
}
