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

# txn's C port, one file per Rust file.
doltgres_cport_txn_sources = files(
  'base.c',
  'blobs.c',
  'change.c',
  'counted.c',
  'entry.c',
  'fold.c',
  'free_space.c',
  'identity.c',
  'insert.c',
  'memtable.c',
  'memtable/branch.c',
  'memtable/index.c',
  'memtable/map.c',
  'memtable/pages.c',
  'memtable/pins.c',
  'memtable/range.c',
  'memtable/slice.c',
  'memtable/split.c',
  'memtable/table.c',
  'memtable/testing.c',
  'memtable/tuples.c',
  'memtable/version.c',
  'outcome.c',
  'record.c',
  'rewrite.c',
  'roots.c',
  'runs.c',
  'spill.c',
  'state.c',
  'status.c',
  'vacuum.c',
  'values.c',
  'visible.c',
  'written.c',
)
doltgres_cport_sources += doltgres_cport_txn_sources
