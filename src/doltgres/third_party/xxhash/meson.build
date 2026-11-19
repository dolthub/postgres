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
# reference to xxHash's own names can bind to the fork's copy (design item 15).
project('xxhash', 'c', version: '0.8.3', default_options: ['warning_level=0'])

xxhash_args = ['-DXXH_NAMESPACE=dg_']
xxhash_lib = static_library('xxhash', files('xxhash.c'), c_args: xxhash_args)
xxhash_dep = declare_dependency(compile_args: xxhash_args, include_directories: include_directories('.'),
  link_with: xxhash_lib)
