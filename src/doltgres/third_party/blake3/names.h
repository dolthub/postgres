/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The prefix dg_ on every external name the vendored BLAKE3 defines (design
 * item 15), which gcc and clang compile its sources with and its header in
 * include gives its users, so no shared library's reference to BLAKE3's own
 * names can bind to the fork's copy. The function BLAKE3 defines before any
 * declaration of it is a macro, since gcc renames a function only from its
 * declaration. The SIMD forms are each one architecture's, whose sources the
 * library builds only there.
 */

#define blake3_compress_subtree_wide dg_blake3_compress_subtree_wide
#pragma redefine_extname blake3_compress_in_place dg_blake3_compress_in_place
#pragma redefine_extname blake3_compress_in_place_portable dg_blake3_compress_in_place_portable
#pragma redefine_extname blake3_compress_xof dg_blake3_compress_xof
#pragma redefine_extname blake3_compress_xof_portable dg_blake3_compress_xof_portable
#pragma redefine_extname blake3_hash_many dg_blake3_hash_many
#pragma redefine_extname blake3_hash_many_portable dg_blake3_hash_many_portable
#pragma redefine_extname blake3_hasher_finalize dg_blake3_hasher_finalize
#pragma redefine_extname blake3_hasher_finalize_seek dg_blake3_hasher_finalize_seek
#pragma redefine_extname blake3_hasher_init dg_blake3_hasher_init
#pragma redefine_extname blake3_hasher_init_derive_key dg_blake3_hasher_init_derive_key
#pragma redefine_extname blake3_hasher_init_derive_key_raw dg_blake3_hasher_init_derive_key_raw
#pragma redefine_extname blake3_hasher_init_keyed dg_blake3_hasher_init_keyed
#pragma redefine_extname blake3_hasher_reset dg_blake3_hasher_reset
#pragma redefine_extname blake3_hasher_update dg_blake3_hasher_update
#pragma redefine_extname blake3_simd_degree dg_blake3_simd_degree
#pragma redefine_extname blake3_version dg_blake3_version
#pragma redefine_extname blake3_xof_many dg_blake3_xof_many
#if defined(__x86_64__)
#pragma redefine_extname blake3_compress_in_place_avx512 dg_blake3_compress_in_place_avx512
#pragma redefine_extname blake3_compress_in_place_sse2 dg_blake3_compress_in_place_sse2
#pragma redefine_extname blake3_compress_in_place_sse41 dg_blake3_compress_in_place_sse41
#pragma redefine_extname blake3_compress_xof_avx512 dg_blake3_compress_xof_avx512
#pragma redefine_extname blake3_compress_xof_sse2 dg_blake3_compress_xof_sse2
#pragma redefine_extname blake3_compress_xof_sse41 dg_blake3_compress_xof_sse41
#pragma redefine_extname blake3_hash_many_avx2 dg_blake3_hash_many_avx2
#pragma redefine_extname blake3_hash_many_avx512 dg_blake3_hash_many_avx512
#pragma redefine_extname blake3_hash_many_sse2 dg_blake3_hash_many_sse2
#pragma redefine_extname blake3_hash_many_sse41 dg_blake3_hash_many_sse41
#pragma redefine_extname blake3_xof_many_avx512 dg_blake3_xof_many_avx512
#elif defined(__aarch64__)
#pragma redefine_extname blake3_hash_many_neon dg_blake3_hash_many_neon
#endif
