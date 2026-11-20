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

# The pinned zstd, compiled from every source upstream's library build compiles and with its defines, as a static
# library named as rustc looks for it, which replaces Postgres' libzstd. Its symbols are hidden, so postgres exports
# none of them, as it exports none of a shared libzstd's. With gcc and clang off Windows every external name it
# defines also carries the prefix dg_ (names.h): an ELF link resolves the names a shared library asks for against the
# executable's own, hidden ones included, where a library built against the system's zstd would otherwise meet this
# one's. The prefix isn't the doltgres_ of the fork's exported names, so the comparison of postgres' exports still
# shows a vendored name that leaks. On Windows the names stay zstd's, since a DLL's names resolve only in the DLLs it
# imports. It is built without zstd's tracing hooks (ZSTD_TRACE=0), weak references that any library on the link line
# could answer.

project('zstd', 'c', version: '1.5.7', default_options: ['warning_level=0'])

cc = meson.get_compiler('c')

# The folder of zstd's public headers, which its users include and pg's build script binds: include where the names
# are prefixed, which a build of pg outside meson must give DOLTGRES_INCLUDES too, since lib's headers there name
# functions the library doesn't define.
zstd_folder = meson.current_source_dir() / 'lib'
zstd_includes = include_directories('lib')

# The names header the library is built with, if any, which the fork's build checks against the library's symbols.
zstd_names = []

zstd_sources = files(
  'lib/common/entropy_common.c',
  'lib/common/error_private.c',
  'lib/common/fse_decompress.c',
  'lib/common/pool.c',
  'lib/common/threading.c',
  'lib/common/xxhash.c',
  'lib/common/zstd_common.c',
  'lib/compress/fse_compress.c',
  'lib/compress/hist.c',
  'lib/compress/huf_compress.c',
  'lib/compress/zstd_compress.c',
  'lib/compress/zstd_compress_literals.c',
  'lib/compress/zstd_compress_sequences.c',
  'lib/compress/zstd_compress_superblock.c',
  'lib/compress/zstd_double_fast.c',
  'lib/compress/zstd_fast.c',
  'lib/compress/zstd_lazy.c',
  'lib/compress/zstd_ldm.c',
  'lib/compress/zstd_opt.c',
  'lib/compress/zstd_preSplit.c',
  'lib/compress/zstdmt_compress.c',
  'lib/decompress/huf_decompress.c',
  'lib/decompress/zstd_ddict.c',
  'lib/decompress/zstd_decompress.c',
  'lib/decompress/zstd_decompress_block.c',
  'lib/dictBuilder/cover.c',
  'lib/dictBuilder/divsufsort.c',
  'lib/dictBuilder/fastcover.c',
  'lib/dictBuilder/zdict.c',
  'lib/legacy/zstd_v05.c',
  'lib/legacy/zstd_v06.c',
  'lib/legacy/zstd_v07.c',
)
zstd_args = ['-DXXH_NAMESPACE=ZSTD_', '-DDEBUGLEVEL=0', '-DZSTD_LEGACY_SUPPORT=5', '-DZSTD_MULTITHREAD',
  '-DZSTD_TRACE=0', '-DZSTDLIB_VISIBLE=', '-DZDICTLIB_VISIBLE=', '-DZSTDERRORLIB_VISIBLE=']
if cc.get_id() in ['gcc', 'clang']
  zstd_sources += files('lib/decompress/huf_decompress_amd64.S')
  # Each floating-point operation rounds on its own, as in every build of the fork (design item 15); a subproject
  # doesn't get the flag from Postgres' own.
  zstd_args += '-ffp-contract=off'
else
  zstd_args += '-DZSTD_DISABLE_ASM'
endif
if cc.get_id() in ['gcc', 'clang'] and host_machine.system() != 'windows'
  zstd_names = files('names.h')
  zstd_args += ['-include', meson.current_source_dir() / 'names.h']
  zstd_folder = meson.current_source_dir() / 'include'
  zstd_includes = include_directories('include')
endif

zstd_lib = static_library('zstd', zstd_sources,
  c_args: zstd_args,
  include_directories: include_directories('lib', 'lib/common', 'lib/legacy'),
  dependencies: dependency('threads'),
  pic: true,
  gnu_symbol_visibility: 'hidden',
  name_prefix: cc.get_argument_syntax() == 'msvc' ? '' : 'lib',
  name_suffix: cc.get_argument_syntax() == 'msvc' ? 'lib' : 'a',
)
meson.override_dependency('libzstd', declare_dependency(
  link_with: zstd_lib,
  include_directories: zstd_includes,
  version: meson.project_version(),
))
