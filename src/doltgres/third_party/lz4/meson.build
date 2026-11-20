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

# The pinned lz4, compiled from every source upstream's library build compiles and with its define, as a static
# library, which replaces Postgres' liblz4. Its symbols are hidden, so postgres exports none of them, as it exports
# none of a shared liblz4's. With gcc and clang off Windows every external name it defines also carries the prefix dg_
# (names.h), as the vendored zstd's do.

project('lz4', 'c', version: '1.10.0', default_options: ['warning_level=0'])

lz4_args = ['-DXXH_NAMESPACE=LZ4_', '-DLZ4LIB_VISIBILITY=', '-DLZ4FLIB_VISIBILITY=']
# Each floating-point operation rounds on its own, as in every build of the fork (design item 15); a subproject
# doesn't get the flag from Postgres' own.
if meson.get_compiler('c').get_id() in ['gcc', 'clang']
  lz4_args += '-ffp-contract=off'
endif
lz4_includes = include_directories('lib')

# The names header the library is built with, if any, which the fork's build checks against the library's symbols.
lz4_names = []
if meson.get_compiler('c').get_id() in ['gcc', 'clang'] and host_machine.system() != 'windows'
  lz4_names = files('names.h')
  lz4_args += ['-include', meson.current_source_dir() / 'names.h']
  lz4_includes = include_directories('include')
endif

lz4_lib = static_library('lz4',
  files('lib/lz4.c', 'lib/lz4file.c', 'lib/lz4frame.c', 'lib/lz4hc.c', 'lib/xxhash.c'),
  c_args: lz4_args,
  include_directories: include_directories('lib'),
  pic: true,
  gnu_symbol_visibility: 'hidden',
)
meson.override_dependency('liblz4', declare_dependency(
  link_with: lz4_lib,
  include_directories: lz4_includes,
  version: meson.project_version(),
))
