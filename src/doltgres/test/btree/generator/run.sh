#!/bin/sh
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

# run.sh regenerates ../expected.tsv: it extracts Doltgrust at 9b345931d from the checkout DOLTGRUST_DIR names, read
# only, into a scratch directory given as its argument, beside a copy of the generator; runs stored.sql with PG_BIN's
# psql on the Postgres 19 server the PG environment variables name; builds the generator offline against Doltgrust's
# own Cargo.lock; and writes the corpus with the generator's sources' hash.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
scratch=$1
commit=9b345931df3633024720b34f0f4f585613d35147
rm -rf "$scratch" && mkdir -p "$scratch/doltgrust" "$scratch/generator"
git -C "$DOLTGRUST_DIR" archive "$commit" | tar -x -C "$scratch/doltgrust"
cp -R "$here/Cargo.toml" "$here/src" "$scratch/generator/"
cp "$scratch/doltgrust/Cargo.lock" "$scratch/doltgrust/rust-toolchain.toml" "$scratch/generator/"
"$PG_BIN/psql" -X -q -At -F "$(printf '\t')" -v ON_ERROR_STOP=1 -f "$here/stored.sql" > "$scratch/stored.tsv"
hash=$(cat "$here/Cargo.toml" "$here/src/main.rs" "$here/run.sh" "$here/stored.sql" | shasum -a 256 | cut -d' ' -f1)
(cd "$scratch/generator" && cargo run --offline --release --quiet -- "$commit" "$hash" "$scratch/stored.tsv") \
    > "$here/../expected.tsv"
