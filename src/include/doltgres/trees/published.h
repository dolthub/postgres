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
 * Tree names a stored value may hold (trees' published.rs): a root a branded
 * writer wrote and published, still a root until a durable name names it, or
 * a name read back from storage, with the repository id each is bound to.
 */
#ifndef DOLTGRES_TREES_PUBLISHED_H
#define DOLTGRES_TREES_PUBLISHED_H

#include "doltgres/std/hash.h"
#include "doltgres/trees/store.h"

/* TREES_PUBLISHED_PUBLISHED_NAMED coerces a Published to the Named it implements. */
#define TREES_PUBLISHED_PUBLISHED_NAMED(x) ((TreesStoreNamed) {&TREES_PUBLISHED_PUBLISHED_NAMED_OPS, (x)})

extern const TreesStoreNamedOps TREES_PUBLISHED_PUBLISHED_NAMED_OPS;

extern TreesPublishedRepoId trees_published_RepoId_of(const ChunksPacksBackendPacks *packs);
extern TreesPublishedRepoId trees_published_RepoId_unique(void);
extern bool trees_published_RepoId_accept(const TreesPublishedRepoId *self, const TreesPublishedPublished *name,
										  const char *door, size_t door_len, ChunksErrorError *error);
extern bool trees_published_RepoId_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_published_RepoId_PartialEq_eq(const TreesPublishedRepoId *self, const TreesPublishedRepoId *other);
extern void trees_published_RepoId_Hash_hash(const void *self_, StdHasher state);
extern TreesStoreTreeRef trees_published_Held_as_ref(const TreesPublishedHeld *self);
extern TreesPublishedPublished trees_published_Held_published(TreesPublishedHeld self);
extern void trees_published_Held_drop(TreesPublishedHeld *self);
extern ChunksAddressAddress trees_published_Published_address(const TreesPublishedPublished *self);
extern TreesPublishedPublished trees_published_Published_decoded(TreesPublishedRepoId repo,
																 ChunksAddressAddress address,
																 ChunksTrailerLocator locator);
extern TreesPublishedRepoId trees_published_Published_repo(const TreesPublishedPublished *self);
extern TreesPublishedPublished trees_published_Published_child(const TreesPublishedPublished *self,
															   ChunksAddressAddress address,
															   ChunksTrailerLocator locator);
extern TreesStoreTree trees_published_Published_base(const TreesPublishedPublished *self);
extern TreesStoreTreeRef trees_published_Published_as_ref(const TreesPublishedPublished *self);
extern bool trees_published_Published_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_published_Published_Display_fmt(const void *value, StdFormatter *f);
extern void trees_published_Published_parts(const TreesPublishedPublished *self, ChunksAddressAddress *result_0,
											ChunksTrailerLocator *result_1);
extern bool trees_published_Published_PartialEq_eq(const TreesPublishedPublished *self,
												   const TreesPublishedPublished *other);
extern void trees_published_Published_Hash_hash(const void *self_, StdHasher state);
extern TreesPublishedPublished trees_published_Published_Clone_clone(const TreesPublishedPublished *self);
extern void trees_published_Published_drop(TreesPublishedPublished *self);

#endif							/* DOLTGRES_TREES_PUBLISHED_H */
