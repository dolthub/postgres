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

# host's C port, one file per Rust file.
doltgres_cport_host_sources = files(
  'lib.c',
  'cache.c',
  'delta.c',
  'io.c',
  'local.c',
  'lock.c',
  'owned.c',
  'shared.c',
  'shared/word.c',
  'delta/blocks.c',
  'delta/directory.c',
  'delta/list.c',
  'delta/page.c',
)
doltgres_cport_sources += doltgres_cport_host_sources
