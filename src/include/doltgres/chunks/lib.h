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
 * chunks' crate root (chunks' lib.rs): its constants.
 */
#ifndef DOLTGRES_CHUNKS_LIB_H
#define DOLTGRES_CHUNKS_LIB_H

/*
 * CHUNKS_BLCKSZ is a block's size, 8192 as Postgres' default BLCKSZ, which
 * temporary files and relation sizes are counted in, fixed whatever block
 * size Postgres was built with.
 */
#define CHUNKS_BLCKSZ ((size_t) 8192)

/* CHUNKS_MAX_ENTRY is the largest entry a tree node holds, 16 KiB. */
#define CHUNKS_MAX_ENTRY ((size_t) 16 << 10)

#endif							/* DOLTGRES_CHUNKS_LIB_H */
