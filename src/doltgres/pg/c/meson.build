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
# thunks and the C forms of the Rust pg's build script generates, with their port-map rows and each entry's exported
# side, into this folder.
doltgres_cport_pg_binding = custom_target('doltgres_cport_pg_binding',
  input: files('../gen_binding.pl'),
  output: ['calls.h', 'calls.c', 'entries.c', 'outside.c', 'pg_calls.h', 'pg_calls.c', 'port-map.tsv', 'export.c'],
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

# Each entry's exported side, which the entry thunks call and which runs the entry's body, in the server's C port
# alone, since a test module's closure holds no entries.
doltgres_cport_pg_export = [doltgres_cport_pg_binding[7]]

# The frontend build's binding in C: the wrappers, their C forms, and the exported side of fsck's entry, with their
# port-map rows, which the C port's frontend configuration builds with frontend.c.
doltgres_cport_pg_fe_binding = custom_target('doltgres_cport_pg_fe_binding',
  input: files('../gen_binding.pl'),
  output: ['fe_calls.h', 'fe_calls.c', 'fe_calls.rs', 'fe_export.rs', 'fe_pg_calls.h', 'fe_pg_calls.c', 'fe_export.c',
    'fe_port-map.tsv'],
  depend_files: files('../frontend-calls.list', '../frontend-entries.list'),
  command: [perl, '@INPUT@', '--frontend', '--forms', meson.current_source_dir() / '..', '@OUTDIR@'],
)
doltgres_cport_pg_frontend_sources = files('frontend.c') + [doltgres_cport_pg_fe_binding[0],
  doltgres_cport_pg_fe_binding[1], doltgres_cport_pg_fe_binding[4], doltgres_cport_pg_fe_binding[5]]
