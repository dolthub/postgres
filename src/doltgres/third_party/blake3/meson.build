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

# The pinned BLAKE3 (pins.tsv), its C release, the trusted library the C port's chunks hash addresses with (the port
# text's section 15): its portable code and dispatch, and on x86-64 each SIMD form in a library of its own built with
# the instructions it needs, which the dispatch picks by what the processor has, as the Rust crate's does. With gcc
# and clang off Windows every external name it defines also carries the prefix dg_ (names.h), as the vendored
# zstd's and lz4's do.

project('blake3', 'c', version: '1.8.7', default_options: ['warning_level=0'])

cc = meson.get_compiler('c')
host_cpu = host_machine.cpu_family()

blake3_c = files('c/blake3.c', 'c/blake3_dispatch.c', 'c/blake3_portable.c')
blake3_simd = []
blake3_args = []
# Each floating-point operation rounds on its own, as in every build of the fork (design item 15); a subproject
# doesn't get the flag from Postgres' own.
if meson.get_compiler('c').get_id() in ['gcc', 'clang']
  blake3_args += '-ffp-contract=off'
endif
blake3_includes = include_directories('c')

# The names header the library is built with, if any, which the fork's build checks against the library's symbols.
blake3_names = []
if cc.get_id() in ['gcc', 'clang'] and host_machine.system() != 'windows'
  blake3_names = files('names.h')
  blake3_args += ['-include', meson.current_source_dir() / 'names.h']
  blake3_includes = include_directories('include')
endif
if host_cpu == 'x86_64'
  foreach form : [['sse2', 'c/blake3_sse2.c', ['-msse2'], []], ['sse41', 'c/blake3_sse41.c', ['-msse4.1'], []],
      ['avx2', 'c/blake3_avx2.c', ['-mavx2'], ['/arch:AVX2']],
      ['avx512', 'c/blake3_avx512.c', ['-mavx512f', '-mavx512vl'], ['/arch:AVX512']]]
    blake3_simd += static_library('blake3_' + form[0],
      files(form[1]),
      c_args: blake3_args + (cc.get_id() == 'msvc' ? form[3] : form[2]),
    )
  endforeach
elif host_cpu == 'aarch64'
  blake3_c += files('c/blake3_neon.c')
  blake3_args += '-DBLAKE3_USE_NEON=1'
else
  blake3_args += ['-DBLAKE3_NO_SSE2', '-DBLAKE3_NO_SSE41', '-DBLAKE3_NO_AVX2', '-DBLAKE3_NO_AVX512']
endif

blake3_lib = static_library('blake3', blake3_c, c_args: blake3_args, link_whole: blake3_simd)
blake3_dep = declare_dependency(include_directories: blake3_includes, link_with: blake3_lib)
