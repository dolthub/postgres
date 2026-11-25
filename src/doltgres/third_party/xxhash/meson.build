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
# The pinned xxHash (pins.tsv), its C release, the trusted library whose XXH3 the C port's chunks checksums with, as
# xxhash-rust's does (the port text's section 15): xxhash.c, which compiles xxhash.h's definitions. Its names carry
# the prefix dg_ through upstream's XXH_NAMESPACE, in the library and in its users, so no shared library's
# reference to xxHash's own names can bind to the fork's copy (design item 15). Its symbols are hidden, so postgres
# exports none of them.
project('xxhash', 'c', version: '0.8.3', default_options: ['warning_level=0'])

xxhash_args = ['-DXXH_NAMESPACE=dg_']
xxhash_lib_args = xxhash_args
# Each floating-point operation rounds on its own, as in every build of the fork (design item 15); a subproject
# doesn't get the flag from Postgres' own. Under the undefined behavior sanitizer, xxHash's own file goes without the
# alignment check alone, a flag that changes nothing in a build without it: on GCC for aarch64, 0.8.3's NEON load
# (xxhash.h's XXH_vld1q_u64) reads 16 bytes through a 16-byte-aligned type at addresses its stripes leave unaligned,
# whose fix (upstream PR 1199) no release carries yet (bugs row 462).
if meson.get_compiler('c').get_id() in ['gcc', 'clang']
  xxhash_lib_args += ['-ffp-contract=off', '-fno-sanitize=alignment']
endif
xxhash_lib = static_library('xxhash', files('xxhash.c'), c_args: xxhash_lib_args,
  gnu_symbol_visibility: 'hidden')
xxhash_dep = declare_dependency(compile_args: xxhash_args, include_directories: include_directories('.'),
  link_with: xxhash_lib)
