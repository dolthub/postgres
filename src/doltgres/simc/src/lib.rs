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

//! The C port's test crate (the port text's section 13), never linked into `postgres`: the simulation's host as the C
//! port's host table and `sim`'s test API as `extern "C"` functions, which the C tests' modules link. The C-facing
//! modules reference the C port's symbols, which only a test module links, so a test build leaves them out.

/// bindgen's declarations of the C port's host table, the types it passes, and the C functions simc calls.
#[cfg(not(test))]
#[allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code,
    unsafe_op_in_unsafe_fn,
    clippy::missing_safety_doc,
    clippy::type_complexity
)]
mod ffi {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

#[cfg(not(test))]
mod api;
#[cfg(not(test))]
mod c;
#[cfg(not(test))]
mod entry;
#[cfg(not(test))]
mod fs;
#[cfg(not(test))]
mod table;
