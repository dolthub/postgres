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
 * Blob trees (trees' blob.rs), which hold values too large for a row: leaves
 * of raw bytes cut where the content says, and internal nodes of each child's
 * address and length that end where the hash of a child's address says.
 */
#ifndef DOLTGRES_TREES_BLOB_H
#define DOLTGRES_TREES_BLOB_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/error.h"
#include "doltgres/host/io.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/store.h"

/* TREES_BLOB_MAX_LEAF is the largest leaf. */
#define TREES_BLOB_MAX_LEAF ((size_t) 32 << 10)

extern bool trees_blob_write_blob(HostIoIo *io, TreesStoreNodeStore store, const uint8 *data, size_t data_len,
								  uint32 target, TreesStoreTreeRef *result, ChunksErrorError *error);
extern ChunksAddressAddress trees_blob_blob_name(const uint8 *data, size_t data_len, uint32 target);
extern bool trees_blob_read_blob(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, StdVec *result,
								 ChunksErrorError *error);
extern bool trees_blob_read_slice(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, uint64 offset,
								  uint8 *out, size_t out_len, ChunksErrorError *error);

#endif							/* DOLTGRES_TREES_BLOB_H */
