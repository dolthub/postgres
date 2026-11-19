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

# The binding's generated C (the port text's section 1): gen_binding.pl writes, from pg's lists, the wrappers and
# thunks and the C forms of the Rust pg's build script generates, with their port-map rows, into this folder.
doltgres_cport_pg_binding = custom_target('doltgres_cport_pg_binding',
  input: files('../gen_binding.pl'),
  output: ['calls.h', 'calls.c', 'entries.c', 'outside.c', 'pg_calls.h', 'pg_calls.c', 'port-map.tsv'],
  depend_files: files('../calls.list', '../test-calls.list', '../entries.list', '../test-entries.list',
    '../settings.list', '../structs.list', '../globals.list'),
  command: [perl, '@INPUT@', '--forms'] + (get_option('injection_points') ? ['--testing'] : []) +
    [meson.current_source_dir() / '..', '@OUTDIR@'],
)
doltgres_cport_pg_include = include_directories('.')

# pg's C port, one file per Rust file, and the C forms of its generated Rust with the headers they include.
doltgres_cport_pg_sources = files(
  'lib.c',
  'word.c',
  'zstd.c',
) + [doltgres_cport_pg_binding[0], doltgres_cport_pg_binding[4], doltgres_cport_pg_binding[5]]
doltgres_cport_sources += doltgres_cport_pg_sources
