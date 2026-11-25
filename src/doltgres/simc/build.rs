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

//! Builds simc's view of the C port (the port text's section 13): bindgen's declarations of the C port's host table,
//! the types it passes, the C functions simc calls, and simc's own header, from the C headers the C tests' modules
//! compile, so each layout simc writes or reads is the C's own.

// A build script runs at build time, never in a test module: it runs bindgen and prints cargo's directives.
#![allow(clippy::disallowed_methods, clippy::disallowed_macros, clippy::disallowed_types)]

use std::env;
use std::path::PathBuf;

/// main generates the bindings into `OUT_DIR`.
fn main() {
    let dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("cargo sets CARGO_MANIFEST_DIR"));
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets OUT_DIR"));
    println!("cargo:rerun-if-env-changed=DOLTGRES_INCLUDES");
    let mut builder = bindgen::Builder::default()
        .header(dir.join("wrapper.h").display().to_string())
        // The C tests over sim build host with its testing feature, as cargo builds sim's dependents' tests.
        .clang_arg("-DDOLTGRES_HOST_TESTING")
        .allowlist_type("Host.*|Std.*|Simc.*|host_.*_t|std_.*_t|simc_.*_t")
        .allowlist_function("host_install|host_io_Io_testing|host_Error_fork|host_Error_drop|std_.*|simc_backend_.*")
        .allowlist_var("STD_UNIX_EPOCH|MAXIMUM_ALIGNOF")
        // A panic simc raises again in the C never returns, which entry.rs's own declaration says.
        .blocklist_function("std_resume_unwind")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .layout_tests(true)
        .generate_comments(false)
        .use_core();
    // MSVC only warns about the uint32 pointers generic-msvc.h passes to the Interlocked calls, which clang rejects.
    if env::var("CARGO_CFG_TARGET_ENV").is_ok_and(|e| e == "msvc") {
        builder = builder.clang_arg("-Wno-error=incompatible-pointer-types");
    }
    for d in env::split_paths(&env::var_os("DOLTGRES_INCLUDES").unwrap_or_default()) {
        builder = builder.clang_arg(format!("-I{}", d.display()));
    }
    builder.generate().expect("bindgen reads the C port's headers").write_to_file(out.join("bindings.rs")).unwrap();
}
