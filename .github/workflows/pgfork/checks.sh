#!/bin/bash
#
# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# checks.sh <source tree> <build folder> <checks file>: runs the static checks Doltgrust's CI runs, on the fork's own
# code, and writes one `<check> ok` or `<check> fail` line for each: pgindent over the C files and headers under
# src/doltgres and src/include/doltgres, and on a tree with the Rust workspace, rustfmt, and clippy with warnings as
# errors and the workspace's tests with the default features and with every feature, as the queue's gates run them,
# given Postgres' headers the way meson gives them to pg's build script.
set -uo pipefail

tree=$1
build=$2
out=$3
: > "$out"

# check <name> <command>...: runs a check and records whether it passed.
check() {
  local name=$1
  shift
  echo "::group::$name"
  if "$@"; then
    echo "$name ok" >> "$out"
  else
    echo "$name fail" >> "$out"
  fi
  echo "::endgroup::"
}

# pgindent_check runs pgindent's check over the fork's own C files.
pgindent_check() {
  (cd "$tree" && src/tools/pgindent/pgindent --indent="$build/src/tools/pg_bsd_indent/pg_bsd_indent" --check --diff \
    "${files[@]}")
}

files=()
while IFS= read -r file; do
  files+=("$file")
done < <(cd "$tree" && git ls-files 'src/doltgres/*.[ch]' 'src/include/doltgres/*.h')
# A tree without the fork's C, stock Postgres, passes pgindent's check, which gives a pull request's check a base.
if [ ${#files[@]} -gt 0 ]; then
  check pgindent pgindent_check
else
  echo "pgindent ok" >> "$out"
fi

if [ -f "$tree/src/doltgres/Cargo.toml" ]; then
  zstd="$tree/src/doltgres/third_party/zstd/include"
  [ -d "$zstd" ] || zstd="$tree/src/doltgres/third_party/zstd/lib"
  export RUSTC_BOOTSTRAP=pg
  export CARGO_TARGET_DIR="$build/checks-target"
  export DOLTGRES_INCLUDES="$build/src/include:$tree/src/include:$zstd"
  export DOLTGRES_LIBS="$build/src/doltgres/third_party/zstd"
  export DOLTGRES_FRONTEND_LIBS="$build/src/port:$build/src/common"
  cd "$tree/src/doltgres" || exit 1
  check rustfmt cargo fmt --all --check
  check clippy cargo clippy --locked --workspace --all-targets --profile cassert -- -D warnings
  check clippy-all-features cargo clippy --locked --workspace --all-targets --all-features --profile cassert -- \
    -D warnings
  check rust-tests cargo test --locked --workspace --profile cassert
  check rust-tests-all-features cargo test --locked --workspace --all-features --profile cassert
fi
