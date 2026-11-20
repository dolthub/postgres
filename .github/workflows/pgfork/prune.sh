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
# prune.sh <build folder>: deletes what only building needed from a finished build, so the build CI keeps for its test
# jobs holds the programs, libraries, generated files, and meson's records the suites run from: meson's folders of
# object files, the target folders cargo builds the Rust in, and on Windows the debuggers' and linker's databases.
set -euo pipefail

find "$1" \( -name '*.p' -o -path "$1/src/doltgres/target*" \) -type d -prune -exec rm -rf {} +
find "$1" \( -name '*.pdb' -o -name '*.ilk' \) -type f -delete
