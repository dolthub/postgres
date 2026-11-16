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
# targets.sh <source tree>: prints the meson targets CI builds for a tree, where nothing printed means every target,
# the server and the tests included. Stock Postgres, a tree without src/doltgres, and a tree with the Rust workspace
# build everything. A tree with only the C builds the C port's two libraries until the port's group C lets it link the
# server and its tests, and group C's landing removes that list here, the one place that names it.
set -euo pipefail

if [ -d "$1/src/doltgres" ] && [ ! -f "$1/src/doltgres/Cargo.toml" ]; then
  echo doltgres_cport doltgres_c
fi
