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

# pg_checkversioned, the fork's check of a stopped cluster's versioned storage (design item D12): its C main, the
# frontend library cargo.pl builds from this folder's workspace with the frontend wrappers pg's build script
# generates, and Postgres' WAL reader, linked with Postgres' frontend code.

doltgres_fsck_build = custom_target('fsck',
  output: [host_system == 'windows' ? 'fsck.lib' : 'libfsck.a', 'fe_calls.c', 'fe_calls.h'],
  depfile: 'libfsck.d',
  depend_files: files('Cargo.toml', 'Cargo.lock', 'fehost/Cargo.toml', '../pg/build.rs', '../pg/gen_binding.pl',
    '../pg/fe_wrapper.h', '../pg/frontend-calls.list', '../pg/frontend-entries.list'),
  env: doltgres_env,
  command: [perl, doltgres_cargo, 'build-frontend', meson.current_source_dir() / '..',
    meson.current_build_dir() / 'target', get_option('cassert') ? 'cassert' : 'release',
    '@OUTPUT0@', '@DEPFILE@'] + cc.cmd_array(),
)

pg_checkversioned = executable('pg_checkversioned',
  files('pg_checkversioned.c', '../../backend/access/transam/xlogreader.c'),
  doltgres_fsck_build[1], doltgres_fsck_build[2],
  include_directories: include_directories('.'),
  dependencies: [frontend_code, lz4, zstd],
  c_args: ['-DFRONTEND'],
  link_args: [doltgres_fsck_build[0].full_path()] + doltgres_native_libs,
  link_depends: doltgres_fsck_build[0],
  kwargs: default_bin_args,
)
bin_targets += pg_checkversioned
