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

//! Generates bindgen's declarations of libzstd from `wrapper.h` over the include folders meson gives, and links the
//! library folders it gives.

// A build script runs at build time, never in a test: it runs bindgen and prints cargo's directives.
#![allow(clippy::disallowed_methods, clippy::disallowed_macros, clippy::disallowed_types)]

use std::env;
use std::path::PathBuf;

fn main() {
    let dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("cargo sets CARGO_MANIFEST_DIR"));
    let out = PathBuf::from(env::var("OUT_DIR").expect("cargo sets OUT_DIR"));
    println!("cargo:rerun-if-changed={}", dir.join("wrapper.h").display());
    println!("cargo:rerun-if-env-changed=DOLTGRES_INCLUDES");
    println!("cargo:rerun-if-env-changed=DOLTGRES_LIBS");
    for d in env::split_paths(&env::var_os("DOLTGRES_LIBS").unwrap_or_default()) {
        println!("cargo:rustc-link-search=native={}", d.display());
    }
    let mut builder = bindgen::Builder::default()
        .header(dir.join("wrapper.h").display().to_string())
        .allowlist_function("ZSTD_.*|ZDICT_.*")
        .allowlist_var("ZSTD_CONTENTSIZE_ERROR")
        .layout_tests(true)
        .generate_comments(false)
        .use_core();
    if let Some(dirs) = env::var_os("DOLTGRES_INCLUDES") {
        for d in env::split_paths(&dirs) {
            builder = builder.clang_arg(format!("-I{}", d.display()));
        }
    }
    builder.generate().expect("bindgen reads libzstd's headers").write_to_file(out.join("bindings.rs")).unwrap();
}
