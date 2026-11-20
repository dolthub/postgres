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
 * Tree names a stored value may hold (trees' published.rs): the repository
 * id a name's chunks are bound to, a root a branded writer holds until it
 * publishes it, and the published names a stored value may hold.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/packs/backend.h"
#include "doltgres/std/atomic.h"
#include "doltgres/trees/published.h"

/* TREES_PUBLISHED_REPO_ID_UNIQUE_NEXT is unique's NEXT, the id the next unique RepoId takes. */
static StdAtomicU64 TREES_PUBLISHED_REPO_ID_UNIQUE_NEXT = STD_ATOMIC_U64_INIT(0);

/* trees_published_RepoId_of returns the id of the repository whose packs these are. */
TreesPublishedRepoId
trees_published_RepoId_of(const ChunksPacksBackendPacks *packs)
{
	TreesPublishedRepoId result;

	result.memory = false;
	result.id = chunks_packs_backend_Packs_location(packs);
	return result;
}

/*
 * trees_published_RepoId_unique returns an id no repository's packs have, for
 * a store whose names never reach a repository's doors: a memory store, or a
 * check of a repository's files that opens none of it.
 */
TreesPublishedRepoId
trees_published_RepoId_unique(void)
{
	TreesPublishedRepoId result;

	result.memory = true;
	result.id = std_AtomicU64_fetch_add_relaxed(&TREES_PUBLISHED_REPO_ID_UNIQUE_NEXT, 1);
	return result;
}

/*
 * trees_published_RepoId_accept refuses, at a door of this repository, a
 * name bound to another one, the one comparison every door that installs a
 * name makes.
 */
bool
trees_published_RepoId_accept(const TreesPublishedRepoId *self, const TreesPublishedPublished *name,
							  const char *door, size_t door_len, ChunksErrorError *error)
{
	StdStr		shown = {door, door_len};
	StdFmtPiece pieces[] = {STD_FMT_LIT("a name of another repository reached "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&shown, std_str_Display_fmt}};
	StdFmtArguments arguments = {pieces, 2, args};

	if (trees_published_RepoId_PartialEq_eq(&name->repo, self))
		return true;
	error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
	std_fmt_format_Checked(&error->u.invariant, &arguments);
	return false;
}

/* trees_published_RepoId_Debug_fmt is RepoId's derived Debug. */
bool
trees_published_RepoId_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesPublishedRepoId *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "RepoId", strlen("RepoId"));

	std_DebugStruct_field(&s, "memory", strlen("memory"), &self->memory, std_bool_Debug_fmt);
	std_DebugStruct_field(&s, "id", strlen("id"), &self->id, std_u64_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* trees_published_RepoId_PartialEq_eq is RepoId's derived PartialEq. */
bool
trees_published_RepoId_PartialEq_eq(const TreesPublishedRepoId *self, const TreesPublishedRepoId *other)
{
	return self->memory == other->memory && self->id == other->id;
}

/* trees_published_RepoId_Hash_hash is RepoId's derived Hash: its two fields in order. */
void
trees_published_RepoId_Hash_hash(const void *self_, StdHasher state)
{
	const TreesPublishedRepoId *self = self_;

	std_Hasher_write_u8(state, (uint8) self->memory);
	std_Hasher_write_u64(state, self->id);
}

/* trees_published_Held_as_ref returns the root's name, to build further chunks with or read it back. */
TreesStoreTreeRef
trees_published_Held_as_ref(const TreesPublishedHeld *self)
{
	return trees_store_Fresh_as_ref(&self->root);
}

/*
 * trees_published_Held_published is Held's Holding: the root as published,
 * its registration moved into the name's Arc, for its writer's publish alone
 * once the writer finished.  The held root stays its caller's until it
 * returns, so a failed allocation leaves the caller its registration to drop.
 */
TreesPublishedPublished
trees_published_Held_published(TreesPublishedHeld self)
{
	TreesPublishedPublished result;
	TreesStoreTreeRef root = trees_store_Fresh_as_ref(&self.root);

	result.address = root.address;
	result.locator = root.locator;
	result.pin = std_arc_new(&self.pin, sizeof(self.pin), _Alignof(ChunksPacksRootsPin));
	result.repo = self.repo;
	return result;
}

/* trees_published_Held_drop is Held's drop glue: its registration. */
void
trees_published_Held_drop(TreesPublishedHeld *self)
{
	chunks_packs_roots_Pin_drop(&self->pin);
	chunks_packs_roots_RootSlot_unused(self->pin.slot);
}

/* trees_published_Published_address returns the address of the tree's root node. */
ChunksAddressAddress
trees_published_Published_address(const TreesPublishedPublished *self)
{
	return self->address;
}

/*
 * trees_published_Published_decoded returns a name stored bytes give where
 * no published name leads to them, for the entry points of stored records
 * alone (a ref, a graph or alias lookup, the WAL, the manifest).
 */
TreesPublishedPublished
trees_published_Published_decoded(TreesPublishedRepoId repo, ChunksAddressAddress address,
								  ChunksTrailerLocator locator)
{
	TreesPublishedPublished result = {address, locator, NULL, repo};

	return result;
}

/* trees_published_Published_repo returns the repository the name entered from. */
TreesPublishedRepoId
trees_published_Published_repo(const TreesPublishedPublished *self)
{
	return self->repo;
}

/*
 * trees_published_Published_child returns the name of a tree a record read
 * through this one names, carrying its registration, so that a tree a
 * just-published root names stays a root while any name decoded from it
 * lives, for the decoders of records alone.
 */
TreesPublishedPublished
trees_published_Published_child(const TreesPublishedPublished *self, ChunksAddressAddress address,
								ChunksTrailerLocator locator)
{
	TreesPublishedPublished result = {address, locator, NULL, self->repo};

	if (self->pin != NULL)
		result.pin = std_arc_clone(self->pin);
	return result;
}

/*
 * trees_published_Published_base returns the name as a tree's base, for the
 * doors that install a published root as one or read the manifest's bases.
 */
TreesStoreTree
trees_published_Published_base(const TreesPublishedPublished *self)
{
	return trees_store_Tree_at(self->address, self->locator);
}

/* trees_published_Published_as_ref returns the name to build further chunks with. */
TreesStoreTreeRef
trees_published_Published_as_ref(const TreesPublishedPublished *self)
{
	return trees_store_TreeRef_of(trees_store_Tree_at(self->address, self->locator));
}

/* trees_published_Published_Debug_fmt is Published's Debug: its root's address as Display writes it. */
bool
trees_published_Published_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesPublishedPublished *self = value;
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&self->address, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, 1, args};

	return std_fmt_write_fmt_Checked(f, &arguments);
}

/* trees_published_Published_Display_fmt is Published's Display: its root's address's, with the formatter's options. */
bool
trees_published_Published_Display_fmt(const void *value, StdFormatter *f)
{
	const TreesPublishedPublished *self = value;

	return chunks_address_Address_Display_fmt(&self->address, f);
}

/* trees_published_Published_parts is Published's Named: its root's address and where it was stored. */
void
trees_published_Published_parts(const TreesPublishedPublished *self, ChunksAddressAddress *result_0,
								ChunksTrailerLocator *result_1)
{
	*result_0 = self->address;
	*result_1 = self->locator;
}

/* trees_published_Published_Named_parts is the parts of TREES_PUBLISHED_PUBLISHED_NAMED_OPS. */
static void
trees_published_Published_Named_parts(void *self, ChunksAddressAddress *result_0, ChunksTrailerLocator *result_1)
{
	trees_published_Published_parts(self, result_0, result_1);
}

/* TREES_PUBLISHED_PUBLISHED_NAMED_OPS is Published's Named table. */
const TreesStoreNamedOps TREES_PUBLISHED_PUBLISHED_NAMED_OPS = {
	.parts = trees_published_Published_Named_parts,
};

/* trees_published_Published_PartialEq_eq is Published's PartialEq: the same parts. */
bool
trees_published_Published_PartialEq_eq(const TreesPublishedPublished *self, const TreesPublishedPublished *other)
{
	return chunks_address_Address_PartialEq_eq(&self->address, &other->address) &&
		chunks_trailer_Locator_PartialEq_eq(&self->locator, &other->locator);
}

/* trees_published_Published_Hash_hash is Published's Hash: its parts', the address's and then the locator's. */
void
trees_published_Published_Hash_hash(const void *self_, StdHasher state)
{
	const TreesPublishedPublished *self = self_;

	chunks_address_Address_Hash_hash(&self->address, state);
	chunks_trailer_Locator_Hash_hash(&self->locator, state);
}

/* trees_published_Published_Clone_clone is Published's derived Clone: the same name, sharing its registration. */
TreesPublishedPublished
trees_published_Published_Clone_clone(const TreesPublishedPublished *self)
{
	return trees_published_Published_child(self, self->address, self->locator);
}

/* trees_published_Pin_drop is the drop of the Pin an Arc of a name's registration holds. */
static void
trees_published_Pin_drop(void *pin_)
{
	ChunksPacksRootsPin *pin = pin_;

	chunks_packs_roots_Pin_drop(pin);
	chunks_packs_roots_RootSlot_unused(pin->slot);
}

/* trees_published_Published_drop is Published's drop glue: its share of its registration, when it carries one. */
void
trees_published_Published_drop(TreesPublishedPublished *self)
{
	if (self->pin != NULL)
		std_arc_drop(self->pin, sizeof(ChunksPacksRootsPin), _Alignof(ChunksPacksRootsPin), trees_published_Pin_drop);
}
