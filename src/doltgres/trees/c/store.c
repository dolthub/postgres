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
 * Node stores and tree names (trees' store.rs): so far the names a tree is
 * read by, a decoded Tree or the TreeRef a writer just wrote, which Named
 * gives the parts of, the Fresh roots a store's top-level writes return, the
 * NodeStore and Branded tables stores read and write through, and the
 * MemoryStore that keeps nodes in memory.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/header.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/lock.h"
#include "doltgres/chunks/packs/writers.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/foldhash.h"
#include "doltgres/std/hashmap.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/build.h"
#include "doltgres/trees/lib.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/published.h"
#include "doltgres/trees/store.h"
#include "xxhash.h"
#ifdef DOLTGRES_TEST_TREES
#include "doltgres/simc/simc.h"
#include "doltgres/std/test.h"
#include "doltgres/trees/build.h"
#endif

#define RUST_FILE "trees" STD_SEP "src" STD_SEP "store.rs"

/* trees_store_Tree_decoded returns the name stored bytes give, for the decoders of stored names alone. */
TreesStoreTree
trees_store_Tree_decoded(ChunksAddressAddress address, ChunksTrailerLocator locator)
{
	TreesStoreTree result = {address, locator};

	return result;
}

/* trees_store_Tree_as_ref returns the name to build further chunks with, as a written root's TreeRef is. */
TreesStoreTreeRef
trees_store_Tree_as_ref(const TreesStoreTree *self)
{
	return trees_store_TreeRef_of(*self);
}

/* trees_store_Tree_at returns a name within this crate. */
TreesStoreTree
trees_store_Tree_at(ChunksAddressAddress address, ChunksTrailerLocator locator)
{
	TreesStoreTree result = {address, locator};

	return result;
}

/* trees_store_Sealed_Debug_fmt is Sealed's derived Debug: its name, which C keeps no value for. */
bool
trees_store_Sealed_Debug_fmt(const void *value, StdFormatter *f)
{
	return std_fmt_write_str(f, "Sealed", 6);
}

/* trees_store_Tree_Debug_fmt is Tree's derived Debug. */
bool
trees_store_Tree_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesStoreTree *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Tree", 4);

	std_DebugStruct_field(&s, "address", 7, &self->address, chunks_address_Address_Debug_fmt);
	std_DebugStruct_field(&s, "locator", 7, &self->locator, chunks_trailer_Locator_Debug_fmt);
	std_DebugStruct_field(&s, "sealed", 6, NULL, trees_store_Sealed_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* trees_store_Tree_PartialEq_eq is Tree's derived PartialEq: the same address and locator. */
bool
trees_store_Tree_PartialEq_eq(const void *self_, const void *other_)
{
	const TreesStoreTree *self = self_;
	const TreesStoreTree *other = other_;

	return chunks_address_Address_PartialEq_eq(&self->address, &other->address) &&
		chunks_trailer_Locator_PartialEq_eq(&self->locator, &other->locator);
}

/* trees_store_TreeRef_PartialEq_eq is TreeRef's derived PartialEq: the same address and locator. */
bool
trees_store_TreeRef_PartialEq_eq(const void *self_, const void *other_)
{
	const TreesStoreTreeRef *self = self_;
	const TreesStoreTreeRef *other = other_;

	return chunks_address_Address_PartialEq_eq(&self->address, &other->address) &&
		chunks_trailer_Locator_PartialEq_eq(&self->locator, &other->locator);
}

/* trees_store_Tree_Hash_hash is Tree's derived Hash: its address, then its locator. */
void
trees_store_Tree_Hash_hash(const void *self_, StdHasher state)
{
	const TreesStoreTree *self = self_;

	chunks_address_Address_Hash_hash(&self->address, state);
	chunks_trailer_Locator_Hash_hash(&self->locator, state);
}

/* trees_store_Tree_Display_fmt is Tree's Display: its address's. */
bool
trees_store_Tree_Display_fmt(const void *value, StdFormatter *f)
{
	return chunks_address_Address_Display_fmt(&((const TreesStoreTree *) value)->address, f);
}

/* trees_store_Tree_Named_parts is Tree's Named: its address and locator. */
static void
trees_store_Tree_Named_parts(void *self, ChunksAddressAddress *result_0, ChunksTrailerLocator *result_1)
{
	*result_0 = ((const TreesStoreTree *) self)->address;
	*result_1 = ((const TreesStoreTree *) self)->locator;
}

/* TREES_STORE_TREE_NAMED_OPS is Tree's Named table. */
const TreesStoreNamedOps TREES_STORE_TREE_NAMED_OPS = {trees_store_Tree_Named_parts};

/* trees_store_name returns the tree a name names, within this crate. */
TreesStoreTree
trees_store_name(TreesStoreNamed named)
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;

	named.ops->parts(named.self, &address, &locator);
	return trees_store_Tree_at(address, locator);
}

/* trees_store_TreeRef_written returns the name of a chunk a node store's write wrote, for those writes alone. */
TreesStoreTreeRef
trees_store_TreeRef_written(ChunksAddressAddress address, ChunksTrailerLocator locator)
{
	TreesStoreTreeRef result = {address, locator};

	return result;
}

/* trees_store_TreeRef_of returns the name of a tree within this crate as one a writer wrote. */
TreesStoreTreeRef
trees_store_TreeRef_of(TreesStoreTree tree)
{
	TreesStoreTreeRef result = {tree.address, tree.locator};

	return result;
}

/* trees_store_TreeRef_tree returns the name within this crate as a tree. */
TreesStoreTree
trees_store_TreeRef_tree(const TreesStoreTreeRef *self)
{
	return trees_store_Tree_at(self->address, self->locator);
}

/* trees_store_TreeRef_Debug_fmt is TreeRef's derived Debug. */
bool
trees_store_TreeRef_Debug_fmt(const void *value, StdFormatter *f)
{
	const TreesStoreTreeRef *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "TreeRef", 7);

	std_DebugStruct_field(&s, "address", 7, &self->address, chunks_address_Address_Debug_fmt);
	std_DebugStruct_field(&s, "locator", 7, &self->locator, chunks_trailer_Locator_Debug_fmt);
	std_DebugStruct_field(&s, "sealed", 6, NULL, trees_store_Sealed_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/* trees_store_TreeRef_Named_parts is TreeRef's Named: its address and locator. */
static void
trees_store_TreeRef_Named_parts(void *self, ChunksAddressAddress *result_0, ChunksTrailerLocator *result_1)
{
	*result_0 = ((const TreesStoreTreeRef *) self)->address;
	*result_1 = ((const TreesStoreTreeRef *) self)->locator;
}

/* TREES_STORE_TREE_REF_NAMED_OPS is TreeRef's Named table. */
const TreesStoreNamedOps TREES_STORE_TREE_REF_NAMED_OPS = {trees_store_TreeRef_Named_parts};

/* trees_store_TreeRef_Display_fmt is TreeRef's Display: its address's. */
bool
trees_store_TreeRef_Display_fmt(const void *value, StdFormatter *f)
{
	return chunks_address_Address_Display_fmt(&((const TreesStoreTreeRef *) value)->address, f);
}

/* trees_TupleAddressVecU8_t is the tuple (Address, Vec<u8>): an entry of MemoryStore's nodes. */
typedef struct trees_TupleAddressVecU8_t
{
	ChunksAddressAddress f0;
	StdVec		f1;
} trees_TupleAddressVecU8_t;

/* trees_TupleAddressChunk_t is the tuple (Address, Chunk): an entry of MemoryStore's records. */
typedef struct trees_TupleAddressChunk_t
{
	ChunksAddressAddress f0;
	ChunksChunkChunk f1;
} trees_TupleAddressChunk_t;

/* trees_store_VecU8_drop is Vec<u8>'s drop glue, a value of MemoryStore's nodes. */
static void
trees_store_VecU8_drop(void *value)
{
	std_vec_free_Checked(value, 1, 1);
}

/* trees_store_VecU8_PartialEq_eq is Vec<u8>'s PartialEq: the same bytes. */
bool
trees_store_VecU8_PartialEq_eq(const void *a, const void *b)
{
	const StdVec *self = a;
	const StdVec *other = b;

	return self->len == other->len && (self->len == 0 || memcmp(self->ptr, other->ptr, self->len) == 0);
}

/* TREES_STORE_HASH_MAP_ADDRESS_VEC_U8_TYPE is the table of a HashMap<Address, Vec<u8>, AddressState>. */
static const StdHashMapType TREES_STORE_HASH_MAP_ADDRESS_VEC_U8_TYPE = {
	.size = sizeof(trees_TupleAddressVecU8_t),
	.align = _Alignof(trees_TupleAddressVecU8_t),
	.value_offset = offsetof(trees_TupleAddressVecU8_t, f1),
	.key_hash = chunks_address_Address_Hash_hash,
	.key_eq = chunks_address_Address_PartialEq_eq,
	.value_drop = trees_store_VecU8_drop,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* TREES_STORE_HASH_MAP_ADDRESS_CHUNK_TYPE is the table of a HashMap<Address, Chunk, AddressState>. */
static const StdHashMapType TREES_STORE_HASH_MAP_ADDRESS_CHUNK_TYPE = {
	.size = sizeof(trees_TupleAddressChunk_t),
	.align = _Alignof(trees_TupleAddressChunk_t),
	.value_offset = offsetof(trees_TupleAddressChunk_t, f1),
	.key_hash = chunks_address_Address_Hash_hash,
	.key_eq = chunks_address_Address_PartialEq_eq,
	.value_drop = chunks_chunk_Chunk_drop,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* The HashMap<Address, Vec<u8>, AddressState> instance, MemoryStore's nodes. */
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS AddressVecU8AddressState
#define CHUNKS_HASH_TYPE (&TREES_STORE_HASH_MAP_ADDRESS_VEC_U8_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 0
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_K ChunksAddressAddress
#define CHUNKS_HASH_V StdVec
#define CHUNKS_HASH_ENTRY trees_TupleAddressVecU8_t
#define CHUNKS_HASH_K_CMP chunks_address_Address_Ord_cmp
#define CHUNKS_HASH_V_EQ trees_store_VecU8_PartialEq_eq
#define CHUNKS_HASH_K_DEBUG chunks_address_Address_Debug_fmt
#define CHUNKS_HASH_V_DEBUG chunks_chunk_VecU8_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* The HashMap<Address, Chunk, AddressState> instance, MemoryStore's records. */
#define CHUNKS_HASH_SCOPE extern
#define CHUNKS_HASH_DECLARE
#define CHUNKS_HASH_DEFINE
#define CHUNKS_HASH_TS AddressChunkAddressState
#define CHUNKS_HASH_TYPE (&TREES_STORE_HASH_MAP_ADDRESS_CHUNK_TYPE)
#define CHUNKS_HASH_ENTRY_FREEZE 1
#define CHUNKS_HASH_ENTRY_COPY 0
#define CHUNKS_HASH_ENTRY_BRANCHLESS 1
#define CHUNKS_HASH_K ChunksAddressAddress
#define CHUNKS_HASH_V ChunksChunkChunk
#define CHUNKS_HASH_ENTRY trees_TupleAddressChunk_t
#define CHUNKS_HASH_K_CMP chunks_address_Address_Ord_cmp
#define CHUNKS_HASH_V_EQ chunks_chunk_Chunk_PartialEq_eq
#define CHUNKS_HASH_K_DEBUG chunks_address_Address_Debug_fmt
#define CHUNKS_HASH_V_DEBUG chunks_chunk_Chunk_Debug_fmt
#include "doltgres/chunks/hash_template.h"

/* trees_store_Fresh_of returns a top-level write's root within this crate. */
TreesStoreFresh
trees_store_Fresh_of(TreesStoreTree tree)
{
	TreesStoreFresh result;

	result.tree = trees_store_TreeRef_of(tree);
	return result;
}

/* trees_store_Fresh_as_ref returns the root's name, to build further chunks with or read it back. */
TreesStoreTreeRef
trees_store_Fresh_as_ref(const TreesStoreFresh *self)
{
	return self->tree;
}

/* trees_store_Fresh_Named_parts is Fresh's Named: its name's parts. */
static void
trees_store_Fresh_Named_parts(void *self, ChunksAddressAddress *result_0, ChunksTrailerLocator *result_1)
{
	TreesStoreFresh *fresh = self;

	trees_store_TreeRef_Named_parts(&fresh->tree, result_0, result_1);
}

/* TREES_STORE_FRESH_NAMED_OPS is Fresh's Named table. */
const TreesStoreNamedOps TREES_STORE_FRESH_NAMED_OPS = {
	.parts = trees_store_Fresh_Named_parts,
};

/* trees_store_rooted_closure_0 is rooted's address count: the length of the address table of the hashed bytes' header. */
static bool
trees_store_rooted_closure_0(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
							 ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		body;

	if (!chunks_header_Header_read(hashed, hashed_len, &header, &body, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/*
 * trees_store_rooted_frame_0_t is rooted's state across its pad: its
 * arguments, its result, and the stored chunk, locators, hashed copy, and
 * stamps it holds.
 */
typedef struct trees_store_rooted_frame_0_t
{
	HostIoIo   *io;
	TreesStoreBranded store;
	TreesStoreTree tree;
	ChunksChunkChunk stored;
	bool		stored_live;
	StdVec		locators;
	bool		locators_live;
	StdVec		hashed;
	bool		hashed_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	TreesStoreFresh *result;
	ChunksErrorError *error;
	bool		ok;
} trees_store_rooted_frame_0_t;

/* trees_store_rooted_body_0 is rooted's body. */
static void
trees_store_rooted_body_0(void *frame_)
{
	trees_store_rooted_frame_0_t *frame = frame_;
	ChunksTrailerStored split;
	ChunksHeaderHeader header;
	size_t		body;
	size_t		named;
	StdSizeHint hint;
	TreesStoreTreeRef copy;

	if (frame->store.ops->wrote(frame->store.self, frame->tree.locator))
	{
		*frame->result = trees_store_Fresh_of(frame->tree);
		frame->ok = true;
		return;
	}
	if (!frame->store.ops->node_store.read_record(frame->store.self, frame->io, &frame->tree.address,
												  frame->tree.locator, &frame->stored, frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->stored_live = true;
	if (!chunks_chunk_Chunk_split(&frame->stored, trees_store_rooted_closure_0, NULL, NULL, &split, frame->error))
	{
		frame->stored_live = false;
		chunks_chunk_Chunk_drop(&frame->stored);
		frame->ok = false;
		return;
	}
	if (!chunks_header_Header_read(split.hashed.ptr, split.hashed.len, &header, &body, frame->error))
	{
		frame->stored_live = false;
		chunks_chunk_Chunk_drop(&frame->stored);
		frame->ok = false;
		return;
	}
	named = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	hint.lower = named;
	hint.has_upper = true;
	hint.upper = named;
	std_vec_with_capacity_trusted(&frame->locators, hint, sizeof(ChunksTrailerLocator),
								  _Alignof(ChunksTrailerLocator));
	frame->locators_live = true;
	std_vec_extend_trusted_reserve(&frame->locators, hint, sizeof(ChunksTrailerLocator),
								   _Alignof(ChunksTrailerLocator));
	for (size_t i = 0, i_end = named; i < i_end; i = std_forward_unchecked_usize_Checked(i, 1))
	{
		ChunksTrailerLocator locator = chunks_trailer_Stored_locator(&split, i);

		memcpy(STD_PTR_AT(frame->locators.ptr, frame->locators.len, sizeof(locator)), &locator, sizeof(locator));
		frame->locators.len = std_wrapping_add_usize(frame->locators.len, 1);
	}
	std_slice_to_vec_Checked(&frame->hashed, split.hashed.ptr, split.hashed.len, 1, 1);
	frame->hashed_live = true;
	if (!chunks_trailer_Stored_stamps(&split, &frame->stamps, frame->error))
	{
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
		frame->locators_live = false;
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		frame->stored_live = false;
		chunks_chunk_Chunk_drop(&frame->stored);
		frame->ok = false;
		return;
	}
	frame->stamps_live = true;
	frame->hashed_live = false;
	if (!frame->store.ops->node_store.write(frame->store.self, frame->io, &frame->hashed, frame->locators.ptr,
											frame->locators.len, &frame->stamps, &copy, frame->error))
	{
		frame->stamps_live = false;
		chunks_trailer_Stamps_drop(&frame->stamps);
		frame->locators_live = false;
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
		frame->stored_live = false;
		chunks_chunk_Chunk_drop(&frame->stored);
		frame->ok = false;
		return;
	}
	frame->stamps_live = false;
	chunks_trailer_Stamps_drop(&frame->stamps);
	*frame->result = trees_store_Fresh_of(trees_store_TreeRef_tree(&copy));
	frame->locators_live = false;
	std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	frame->stored_live = false;
	chunks_chunk_Chunk_drop(&frame->stored);
	frame->ok = true;
}

/* trees_store_rooted_pad_0 is rooted's pad: the stamps, hashed copy, locators, and stored chunk, while it holds them. */
static void
trees_store_rooted_pad_0(void *frame_)
{
	trees_store_rooted_frame_0_t *frame = frame_;

	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	if (frame->stored_live)
		chunks_chunk_Chunk_drop(&frame->stored);
}

/*
 * trees_store_rooted returns a top-level write's root as a root the store
 * wrote itself: the tree when the store wrote it, or else a copy of its root
 * node the store writes, naming its children as any node it writes does.
 */
bool
trees_store_rooted(HostIoIo *io, TreesStoreBranded store, TreesStoreTree tree, TreesStoreFresh *result,
				   ChunksErrorError *error)
{
	trees_store_rooted_frame_0_t frame = {.io = io, .store = store, .tree = tree, .result = result, .error = error};

	std_landing(&frame, trees_store_rooted_body_0, trees_store_rooted_pad_0);
	return frame.ok;
}

/*
 * trees_store_write_root writes a chunk, a tree node or a record, as a
 * top-level write's root, with the locators of its addresses and its rows'
 * stamps; it takes the hashed bytes.
 */
bool
trees_store_write_root(HostIoIo *io, TreesStoreBranded store, StdVec *hashed, const ChunksTrailerLocator *locators,
					   size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreFresh *result,
					   ChunksErrorError *error)
{
	TreesStoreTreeRef written;

	if (!store.ops->node_store.write(store.self, io, hashed, locators, locators_len, stamps, &written, error))
		return false;
	*result = trees_store_Fresh_of(trees_store_TreeRef_tree(&written));
	return true;
}

/* trees_store_NodeStore_prefetch is NodeStore's default prefetch, which reads nothing ahead. */
bool
trees_store_NodeStore_prefetch(void *self, HostIoIo *io, TreesStoreTree tree, ChunksErrorError *error)
{
	return true;
}

/* trees_store_NodeStore_locate is NodeStore's default locate: nothing, a store that keeps no chunk for good. */
bool
trees_store_NodeStore_locate(void *self, HostIoIo *io, const ChunksAddressAddress *address, bool *some,
							 ChunksTrailerLocator *result, ChunksErrorError *error)
{
	*some = false;
	return true;
}

/* trees_store_is_node reports whether a chunk's hashed bytes are a tree node rather than a record. */
static bool
trees_store_is_node(const uint8 *hashed, size_t hashed_len)
{
	if (hashed_len == 0)
		return false;
	switch (hashed[0])
	{
		case CHUNKS_HEADER_TREE_LEAF:
		case CHUNKS_HEADER_TREE_INTERNAL:
		case CHUNKS_HEADER_BLOB_LEAF:
			return true;
		default:
			return false;
	}
}

/* trees_store_stamps_of_closure_0 is stamps_of's address count: the length of the address table of the hashed bytes' header. */
static bool
trees_store_stamps_of_closure_0(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
								ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		body;

	if (!chunks_header_Header_read(hashed, hashed_len, &header, &body, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/* trees_store_stamps_of returns the row stamps in a stored chunk. */
bool
trees_store_stamps_of(const ChunksChunkChunk *chunk, ChunksTrailerStamps *result, ChunksErrorError *error)
{
	ChunksTrailerStored split;

	if (!chunks_chunk_Chunk_split(chunk, trees_store_stamps_of_closure_0, NULL, NULL, &split, error))
		return false;
	return chunks_trailer_Stored_stamps(&split, result, error);
}

/* trees_store_cached_stamps_frame_0_t is cached_stamps' state across its pad: its arguments, its result, and the read and stamps it holds. */
typedef struct trees_store_cached_stamps_frame_0_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreNamed tree;
	TreesNodeFlat read;
	bool		read_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	StdArcInner **result;
	ChunksErrorError *error;
	bool		ok;
} trees_store_cached_stamps_frame_0_t;

/* trees_store_cached_stamps_body_0 is cached_stamps' body. */
static void
trees_store_cached_stamps_body_0(void *frame_)
{
	trees_store_cached_stamps_frame_0_t *frame = frame_;
	TreesStoreTree tree_2 = trees_store_name(frame->tree);
	TreesNodeNode node;

	if (!frame->store.ops->read(frame->store.self, frame->io, &tree_2.address, tree_2.locator, &frame->read,
								frame->error))
	{
		frame->ok = false;
		return;
	}
	frame->read_live = true;
	node = trees_node_Flat_node(&frame->read);
	if (!trees_node_Node_stamps(&node, &frame->stamps, frame->error))
	{
		frame->read_live = false;
		trees_node_Flat_drop(&frame->read);
		frame->ok = false;
		return;
	}
	frame->stamps_live = true;
	*frame->result = std_arc_new(&frame->stamps, sizeof(ChunksTrailerStamps), _Alignof(ChunksTrailerStamps));
	frame->stamps_live = false;
	frame->read_live = false;
	trees_node_Flat_drop(&frame->read);
	frame->ok = true;
}

/* trees_store_cached_stamps_pad_0 is cached_stamps' pad: the stamps and the read, while it holds them. */
static void
trees_store_cached_stamps_pad_0(void *frame_)
{
	trees_store_cached_stamps_frame_0_t *frame = frame_;

	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
	if (frame->read_live)
		trees_node_Flat_drop(&frame->read);
}

/* trees_store_cached_stamps returns the stamps of the node a tree name names, which its flat bytes hold beside it, in an Arc<Stamps>. */
bool
trees_store_cached_stamps(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree, StdArcInner **result,
						  ChunksErrorError *error)
{
	trees_store_cached_stamps_frame_0_t frame = {.io = io, .store = store, .tree = tree, .result = result,
	.error = error};

	std_landing(&frame, trees_store_cached_stamps_body_0, trees_store_cached_stamps_pad_0);
	return frame.ok;
}

/* trees_store_MemoryStore_Default_default is MemoryStore's Default: no nodes or records, no writes, and a unique repository id. */
TreesStoreMemoryStore
trees_store_MemoryStore_Default_default(void)
{
	TreesStoreMemoryStore result;

	chunks_hash_HashMapAddressVecU8AddressState_Default_default(&result.nodes);
	chunks_hash_HashMapAddressChunkAddressState_Default_default(&result.records);
	result.written = 0;
	result.repo = trees_published_RepoId_unique();
	return result;
}

/* trees_store_MemoryStore_new returns an empty store. */
TreesStoreMemoryStore
trees_store_MemoryStore_new(void)
{
	return trees_store_MemoryStore_Default_default();
}

/* trees_store_MemoryStore_repo returns the store's own repository id, which the names decoded from its records are bound to. */
TreesPublishedRepoId
trees_store_MemoryStore_repo(const TreesStoreMemoryStore *self)
{
	return self->repo;
}

/* trees_store_MemoryStore_written returns the number of nodes written, including ones already stored. */
size_t
trees_store_MemoryStore_written(const TreesStoreMemoryStore *self)
{
	return self->written;
}

/* trees_store_MemoryStore_len returns the number of distinct nodes stored. */
size_t
trees_store_MemoryStore_len(const TreesStoreMemoryStore *self)
{
	return chunks_hash_HashMapAddressVecU8AddressState_len(&self->nodes);
}

/* trees_store_MemoryStore_is_empty reports whether no node is stored. */
bool
trees_store_MemoryStore_is_empty(const TreesStoreMemoryStore *self)
{
	return chunks_hash_HashMapAddressVecU8AddressState_is_empty(&self->nodes);
}

/* trees_store_MemoryStore_Branded_wrote is MemoryStore's Branded wrote: every chunk it holds, since it wrote them all. */
static bool
trees_store_MemoryStore_Branded_wrote(void *self, ChunksTrailerLocator locator)
{
	return true;
}

/* trees_store_MemoryStore_NodeStore_as_dyn is MemoryStore's NodeStore as_dyn: itself as a NodeStore. */
static TreesStoreNodeStore
trees_store_MemoryStore_NodeStore_as_dyn(void *self)
{
	return TREES_STORE_MEMORY_STORE_NODE_STORE(self);
}

/*
 * trees_store_MemoryStore_NodeStore_read is MemoryStore's NodeStore read: a
 * copy of the node's flat bytes, since the read owns what it returns and the
 * store keeps its own.
 */
static bool
trees_store_MemoryStore_NodeStore_read(void *self, HostIoIo *io, const ChunksAddressAddress *address,
									   ChunksTrailerLocator locator, TreesNodeFlat *result, ChunksErrorError *error)
{
	TreesStoreMemoryStore *store = self;
	const StdVec *flat = chunks_hash_HashMapAddressVecU8AddressState_get(&store->nodes, address);

	if (flat == NULL)
	{
		error->tag = CHUNKS_ERROR_ERROR_MISSING;
		error->u.missing.f0 = *address;
		error->u.missing.has_f1 = false;
		return false;
	}
	result->f0.tag = HOST_CACHE_READ_UNCACHED;
	std_vec_clone(&result->f0.u.uncached, flat, 1, 1);
	return true;
}

/* trees_store_MemoryStore_NodeStore_read_record is MemoryStore's NodeStore read_record: a copy of the record. */
static bool
trees_store_MemoryStore_NodeStore_read_record(void *self, HostIoIo *io, const ChunksAddressAddress *address,
											  ChunksTrailerLocator locator, ChunksChunkChunk *result,
											  ChunksErrorError *error)
{
	TreesStoreMemoryStore *store = self;
	const ChunksChunkChunk *record = chunks_hash_HashMapAddressChunkAddressState_get(&store->records, address);

	if (record == NULL)
	{
		error->tag = CHUNKS_ERROR_ERROR_MISSING;
		error->u.missing.f0 = *address;
		error->u.missing.has_f1 = false;
		return false;
	}
	chunks_chunk_Chunk_Clone_clone(record, result);
	return true;
}

/* trees_store_MemoryStore_NodeStore_locate is MemoryStore's NodeStore locate: an unknown locator for a node it holds. */
static bool
trees_store_MemoryStore_NodeStore_locate(void *self, HostIoIo *io, const ChunksAddressAddress *address, bool *some,
										 ChunksTrailerLocator *result, ChunksErrorError *error)
{
	TreesStoreMemoryStore *store = self;

	*some = chunks_hash_HashMapAddressVecU8AddressState_contains_key(&store->nodes, address);
	if (*some)
		*result = chunks_trailer_Locator_Default_default();
	return true;
}

/*
 * trees_store_MemoryStore_write_tree_frame_0_t is write_tree's state across
 * its pad: its arguments, its result, and the record, chunk, and flat
 * bytes it holds.
 */
typedef struct trees_store_MemoryStore_write_tree_frame_0_t
{
	TreesStoreMemoryStore *self;
	StdVec		hashed;
	bool		hashed_live;
	const ChunksTrailerLocator *locators;
	size_t		locators_len;
	const ChunksTrailerStamps *stamps;
	ChunksChunkChunk record;
	bool		record_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	StdVec		flat;
	bool		flat_live;
	TreesStoreTree *result;
	ChunksErrorError *error;
	bool		ok;
} trees_store_MemoryStore_write_tree_frame_0_t;

/* trees_store_MemoryStore_write_tree_body_0 is write_tree's body. */
static void
trees_store_MemoryStore_write_tree_body_0(void *frame_)
{
	trees_store_MemoryStore_write_tree_frame_0_t *frame = frame_;
	ChunksAddressAddress address;

	frame->self->written = STD_ADD(usize, frame->self->written, 1, 405, 9);
	if (!trees_store_is_node(frame->hashed.ptr, frame->hashed.len))
	{
		ChunksChunkChunk old;

		chunks_chunk_Chunk_stored(frame->hashed.ptr, frame->hashed.len, frame->locators, frame->locators_len,
								  frame->stamps, &frame->record);
		frame->record_live = true;
		address = chunks_chunk_Chunk_address(&frame->record);
		frame->record_live = false;
		if (chunks_hash_HashMapAddressChunkAddressState_insert(&frame->self->records, &address, &frame->record, &old))
			chunks_chunk_Chunk_drop(&old);
		*frame->result = trees_store_Tree_at(address, chunks_trailer_Locator_Default_default());
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
		frame->ok = true;
		return;
	}
	chunks_chunk_Chunk_stored(frame->hashed.ptr, frame->hashed.len, frame->locators, frame->locators_len,
							  frame->stamps, &frame->chunk);
	frame->chunk_live = true;
	address = chunks_chunk_Chunk_address(&frame->chunk);
	if (!trees_node_flatten(&frame->chunk, &frame->flat, frame->error))
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
		frame->ok = false;
		return;
	}
	frame->flat_live = true;
	{
		StdVec		old;

		frame->flat_live = false;
		if (chunks_hash_HashMapAddressVecU8AddressState_insert(&frame->self->nodes, &address, &frame->flat, &old))
			std_vec_free_Checked(&old, 1, 1);
	}
	*frame->result = trees_store_Tree_at(address, chunks_trailer_Locator_Default_default());
	frame->chunk_live = false;
	chunks_chunk_Chunk_drop(&frame->chunk);
	frame->hashed_live = false;
	std_vec_free_Checked(&frame->hashed, 1, 1);
	frame->ok = true;
}

/* trees_store_MemoryStore_write_tree_pad_0 is write_tree's pad: the flat bytes, the chunk, the record, and the hashed bytes, while it holds them. */
static void
trees_store_MemoryStore_write_tree_pad_0(void *frame_)
{
	trees_store_MemoryStore_write_tree_frame_0_t *frame = frame_;

	if (frame->flat_live)
		std_vec_free_Checked(&frame->flat, 1, 1);
	if (frame->chunk_live)
		chunks_chunk_Chunk_drop(&frame->chunk);
	if (frame->record_live)
		chunks_chunk_Chunk_drop(&frame->record);
	if (frame->hashed_live)
		std_vec_free_Checked(&frame->hashed, 1, 1);
}

/* trees_store_MemoryStore_write_tree is write, returning the chunk's name as a tree; it takes the hashed bytes. */
static bool
trees_store_MemoryStore_write_tree(TreesStoreMemoryStore *self, StdVec *hashed, const ChunksTrailerLocator *locators,
								   size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTree *result,
								   ChunksErrorError *error)
{
	trees_store_MemoryStore_write_tree_frame_0_t frame = {.self = self, .hashed = *hashed, .hashed_live = true,
		.locators = locators, .locators_len = locators_len, .stamps = stamps, .result = result,
	.error = error};

	std_landing(&frame, trees_store_MemoryStore_write_tree_body_0, trees_store_MemoryStore_write_tree_pad_0);
	return frame.ok;
}

/* trees_store_MemoryStore_NodeStore_write is MemoryStore's NodeStore write: write_tree's name as one a writer wrote. */
static bool
trees_store_MemoryStore_NodeStore_write(void *self, HostIoIo *io, StdVec *hashed, const ChunksTrailerLocator *locators,
										size_t locators_len, const ChunksTrailerStamps *stamps,
										TreesStoreTreeRef *result, ChunksErrorError *error)
{
	TreesStoreTree tree;

	if (!trees_store_MemoryStore_write_tree(self, hashed, locators, locators_len, stamps, &tree, error))
		return false;
	*result = trees_store_TreeRef_of(tree);
	return true;
}

/* TREES_STORE_MEMORY_STORE_BRANDED_OPS is MemoryStore's Branded table, with its NodeStore table first. */
const TreesStoreBrandedOps TREES_STORE_MEMORY_STORE_BRANDED_OPS = {
	.node_store = {
		.read = trees_store_MemoryStore_NodeStore_read,
		.read_record = trees_store_MemoryStore_NodeStore_read_record,
		.write = trees_store_MemoryStore_NodeStore_write,
		.as_dyn = trees_store_MemoryStore_NodeStore_as_dyn,
		.prefetch = trees_store_NodeStore_prefetch,
		.locate = trees_store_MemoryStore_NodeStore_locate,
	},
	.wrote = trees_store_MemoryStore_Branded_wrote,
};

/* trees_store_MemoryStore_drop is MemoryStore's drop glue: its nodes and records. */
void
trees_store_MemoryStore_drop(TreesStoreMemoryStore *self)
{
	chunks_hash_HashMapAddressVecU8AddressState_drop(&self->nodes);
	chunks_hash_HashMapAddressChunkAddressState_drop(&self->records);
}

/*
 * trees_store_key returns the node cache's key of the copy of a node stored
 * at a locator in some packs.  It is the one place a key is built: a key
 * names one stored copy, so the locator is a known one.
 */
static HostCacheKey
trees_store_key(const ChunksPacksBackendPacks *packs, const ChunksAddressAddress *address,
				ChunksTrailerLocator locator)
{
	StdVec		stored;
	uint8		at[CHUNKS_TRAILER_LOCATOR_LEN];
	HostCacheKey key;

	std_vec_with_capacity_Checked(&stored, CHUNKS_TRAILER_LOCATOR_LEN, 1, 1);
	chunks_trailer_Locator_write(&locator, &stored);
	memcpy(at, stored.ptr, CHUNKS_TRAILER_LOCATOR_LEN);
	std_vec_free_Checked(&stored, 1, 1);
	key = host_cache_key(chunks_packs_backend_Packs_location(packs), address->f0, at);
	return key;
}

/* trees_store_cached_fill_0 is cached's fill of a frame: the node's flat bytes. */
static void
trees_store_cached_fill_0(void *captures, uint8 *frame, size_t frame_len)
{
	const StdVec *flat = captures;

	memcpy(frame, flat->ptr, frame_len);
}

/* trees_store_cached_frame_0_t is cached's state across its pad: the stored chunk and the flat bytes, while it holds them. */
typedef struct trees_store_cached_frame_0_t
{
	HostIoIo   *io;
	HostCachePlacement placement;
	const HostCacheKey *key;
	bool		(*stored) (void *captures, HostIoIo *io, ChunksChunkChunk *result, ChunksErrorError *error);
	void	   *stored_captures;
	TreesNodeFlat *result;
	ChunksErrorError *error;
	bool		ok;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	StdVec		flat;
	bool		flat_live;
} trees_store_cached_frame_0_t;

/* trees_store_cached_pad_0 is cached's pad: the chunk and the flat bytes, while live. */
static void
trees_store_cached_pad_0(void *frame_)
{
	trees_store_cached_frame_0_t *frame = frame_;

	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
	}
	if (frame->flat_live)
	{
		frame->flat_live = false;
		std_vec_free_Checked(&frame->flat, 1, 1);
	}
}

/* trees_store_cached_body_0 is cached's body. */
static void
trees_store_cached_body_0(void *frame_)
{
	trees_store_cached_frame_0_t *frame = frame_;
	HostError	host_error;
	HostCachePinned pinned;
	HostCacheRead read;
	bool		has;

	if (!host_cache_get(frame->io, frame->placement, frame->key, &has, &pinned, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	if (has)
	{
		frame->result->f0.tag = HOST_CACHE_READ_CACHED;
		frame->result->f0.u.cached = pinned;
		frame->ok = true;
		return;
	}
	if (!frame->stored(frame->stored_captures, frame->io, &frame->chunk, frame->error))
		return;
	frame->chunk_live = true;
	if (!trees_node_flatten(&frame->chunk, &frame->flat, frame->error))
	{
		trees_store_cached_pad_0(frame);
		return;
	}
	frame->flat_live = true;
	frame->chunk_live = false;
	chunks_chunk_Chunk_drop(&frame->chunk);
	if (!host_cache_install(frame->io, frame->placement, frame->key, frame->flat.len, trees_store_cached_fill_0,
							&frame->flat, NULL, &read, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		trees_store_cached_pad_0(frame);
		return;
	}
	frame->result->f0 = read;
	frame->ok = true;
	trees_store_cached_pad_0(frame);
}

/*
 * trees_store_cached returns a stored copy's node from a node cache, or
 * decodes it from the stored bytes `stored` reads and puts it there.
 */
static bool
trees_store_cached(HostIoIo *io, HostCachePlacement placement, const HostCacheKey *key,
				   bool (*stored) (void *captures, HostIoIo *io, ChunksChunkChunk *result, ChunksErrorError *error),
				   void *stored_captures, TreesNodeFlat *result, ChunksErrorError *error)
{
	trees_store_cached_frame_0_t frame = {.io = io, .placement = placement, .key = key, .stored = stored,
	.stored_captures = stored_captures, .result = result, .error = error};

	std_landing(&frame, trees_store_cached_body_0, trees_store_cached_pad_0);
	return frame.ok;
}

/* trees_store_Reader_read_captures_t is what Reader's read reads a stored copy with. */
typedef struct trees_store_Reader_read_captures_t
{
	const TreesStoreReader *self;
	const ChunksAddressAddress *address;
	ChunksTrailerLocator locator;
} trees_store_Reader_read_captures_t;

/* trees_store_Reader_read_closure_0 is read's |io| self.packs.read(io, self.reading, address, locator). */
static bool
trees_store_Reader_read_closure_0(void *captures, HostIoIo *io, ChunksChunkChunk *result, ChunksErrorError *error)
{
	trees_store_Reader_read_captures_t *read = captures;

	return chunks_packs_backend_Packs_read(read->self->packs, io, read->self->reading, read->address, read->locator,
										   result, error);
}

/*
 * trees_store_Reader_read returns the node with the address, from the cache
 * or else from its pack.  A read by address alone first asks the packs which
 * copy their index gives a reader now, and reads that copy as its locator
 * names it, so that every cached node is one stored copy's.
 */
bool
trees_store_Reader_read(const TreesStoreReader *self, HostIoIo *io, const ChunksAddressAddress *address,
						ChunksTrailerLocator locator, TreesNodeFlat *result, ChunksErrorError *error)
{
	HostError	host_error;
	HostCacheKey key;
	trees_store_Reader_read_captures_t captures = {self, address, locator};

	if (!host_io_Io_check(io, &host_error))
	{
		*error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	if (!chunks_trailer_Locator_is_known(&locator))
	{
		bool		has;

		if (!chunks_packs_backend_Packs_find(self->packs, io, self->reading, address, &has, &captures.locator,
											 error))
			return false;
		if (!has)
		{
			error->tag = CHUNKS_ERROR_ERROR_MISSING;
			error->u.missing.f0 = *address;
			error->u.missing.has_f1 = false;
			return false;
		}
	}
	key = trees_store_key(self->packs, address, captures.locator);
	return trees_store_cached(io, self->placement, &key, trees_store_Reader_read_closure_0, &captures, result, error);
}

/* trees_store_Reader_read_record returns the stored bytes of the record chunk with the address. */
bool
trees_store_Reader_read_record(const TreesStoreReader *self, HostIoIo *io, const ChunksAddressAddress *address,
							   ChunksTrailerLocator locator, ChunksChunkChunk *result, ChunksErrorError *error)
{
	return chunks_packs_backend_Packs_read(self->packs, io, self->reading, address, locator, result, error);
}

/* trees_store_Reader_NodeStore_as_dyn is Reader's NodeStore as_dyn: itself as a NodeStore. */
static TreesStoreNodeStore
trees_store_Reader_NodeStore_as_dyn(void *self)
{
	return TREES_STORE_READER_NODE_STORE(self);
}

/* trees_store_Reader_NodeStore_read is Reader's NodeStore read: Reader's own. */
static bool
trees_store_Reader_NodeStore_read(void *self, HostIoIo *io, const ChunksAddressAddress *address,
								  ChunksTrailerLocator locator, TreesNodeFlat *result, ChunksErrorError *error)
{
	return trees_store_Reader_read(self, io, address, locator, result, error);
}

/*
 * trees_store_Reader_NodeStore_prefetch is Reader's NodeStore prefetch: it
 * advises the packs of the node's read unless a node cache holds the copy.
 */
static bool
trees_store_Reader_NodeStore_prefetch(void *self, HostIoIo *io, TreesStoreTree tree, ChunksErrorError *error)
{
	const TreesStoreReader *reader = self;
	bool		held = false;

	if (chunks_trailer_Locator_is_known(&tree.locator))
	{
		HostError	host_error;
		HostCacheKey key = trees_store_key(reader->packs, &tree.address, tree.locator);

		if (!host_cache_contains(io, reader->placement, &key, &held, &host_error))
		{
			*error = chunks_error_Error_FromError_from(&host_error);
			return false;
		}
	}
	if (held)
		return true;
	return chunks_packs_backend_Packs_advise(reader->packs, io, reader->reading, tree.locator, error);
}

/* trees_store_Reader_NodeStore_read_record is Reader's NodeStore read_record: Reader's own. */
static bool
trees_store_Reader_NodeStore_read_record(void *self, HostIoIo *io, const ChunksAddressAddress *address,
										 ChunksTrailerLocator locator, ChunksChunkChunk *result,
										 ChunksErrorError *error)
{
	return trees_store_Reader_read_record(self, io, address, locator, result, error);
}

/* trees_store_Reader_NodeStore_write is Reader's NodeStore write, which a reader refuses. */
static bool
trees_store_Reader_NodeStore_write(void *self, HostIoIo *io, StdVec *hashed, const ChunksTrailerLocator *locators,
								   size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTreeRef *result,
								   ChunksErrorError *error)
{
	std_vec_free_Checked(hashed, 1, 1);
	*error = chunks_error_Error_corrupt_RefStr("a reader cannot write tree nodes",
											   strlen("a reader cannot write tree nodes"));
	return false;
}

/* TREES_STORE_READER_NODE_STORE_OPS is Reader's NodeStore table. */
const TreesStoreNodeStoreOps TREES_STORE_READER_NODE_STORE_OPS = {
	.read = trees_store_Reader_NodeStore_read,
	.read_record = trees_store_Reader_NodeStore_read_record,
	.write = trees_store_Reader_NodeStore_write,
	.as_dyn = trees_store_Reader_NodeStore_as_dyn,
	.prefetch = trees_store_Reader_NodeStore_prefetch,
	.locate = trees_store_NodeStore_locate,
};

/* trees_TupleAddressLocated_t is an (Address, Located), an entry of a writer's located map. */
typedef struct trees_TupleAddressLocated_t
{
	ChunksAddressAddress f0;
	TreesStoreLocated f1;
} trees_TupleAddressLocated_t;

/* TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE is the table of a HashMap<Address, Located, AddressState>. */
static const StdHashMapType TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE = {
	.size = sizeof(trees_TupleAddressLocated_t),
	.align = _Alignof(trees_TupleAddressLocated_t),
	.value_offset = offsetof(trees_TupleAddressLocated_t, f1),
	.key_hash = chunks_address_Address_Hash_hash,
	.key_eq = chunks_address_Address_PartialEq_eq,
	.hasher = &FOLDHASH_FAST_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = foldhash_fast_RandomState_Default_default,
};

/* trees_store_located_get returns where a writer's located map has the last copy of a chunk, or NULL. */
static TreesStoreLocated *
trees_store_located_get(const StdHashMap *located, const ChunksAddressAddress *address)
{
	uint8	   *elem = std_HashMap_get(located, &TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE, address);

	return elem == NULL ? NULL : (TreesStoreLocated *) (elem + offsetof(trees_TupleAddressLocated_t, f1));
}

/* trees_TupleLocatorVecU8_t is a (Locator, Vec<u8>), an entry of a temporary writer's decoded map. */
typedef struct trees_TupleLocatorVecU8_t
{
	ChunksTrailerLocator f0;
	StdVec		f1;
} trees_TupleLocatorVecU8_t;

/* trees_store_Locator_eq is Locator's PartialEq over untyped keys. */
static bool
trees_store_Locator_eq(const void *a, const void *b)
{
	return chunks_trailer_Locator_PartialEq_eq(a, b);
}

/* trees_store_StdState_Default_default is StdState's Default: a RandomState::new. */
static void
trees_store_StdState_Default_default(void *result)
{
	*(StdRandomState *) result = std_RandomState_new();
}

/* TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE is the table of a HashMap<Locator, Vec<u8>, StdState>. */
static const StdHashMapType TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE = {
	.size = sizeof(trees_TupleLocatorVecU8_t),
	.align = _Alignof(trees_TupleLocatorVecU8_t),
	.value_offset = offsetof(trees_TupleLocatorVecU8_t, f1),
	.key_hash = chunks_trailer_Locator_Hash_hash,
	.key_eq = trees_store_Locator_eq,
	.value_drop = trees_store_VecU8_drop,
	.hasher = &STD_RANDOM_STATE_BUILD_HASHER_OPS,
	.hasher_default = trees_store_StdState_Default_default,
};

/* taken at Decoded and IoMutex<Decoded>, a temporary writer's decoded nodes. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DEFINE
#define HOST_LOCK_TAKEN_TS Decoded
#define HOST_LOCK_TAKEN_T StdHashMap
#define HOST_LOCK_TAKEN_DEFAULT(place) std_HashMap_Default_default((place), &TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE)
#define HOST_LOCK_TAKEN_DROP(place) std_HashMap_drop((place), &TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE)
#define HOST_LOCK_TAKEN_TYPE TREES_STORE_DECODED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Decoded
#define HOST_LOCK_T StdHashMap
#define HOST_LOCK_DEFAULT(place) std_HashMap_Default_default((place), &TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE)
#include "doltgres/host/lock_template.h"

/* trees_store_Logged_drop is Logged's drop glue: its hashed bytes and its trailer. */
void
trees_store_Logged_drop(trees_TupleAddressVecU8VecU8_t *self)
{
	std_vec_free_Checked(&self->f1, 1, 1);
	std_vec_free_Checked(&self->f2, 1, 1);
}

/* trees_store_VecLogged_drop is Vec<Logged>'s drop glue. */
static void
trees_store_VecLogged_drop(StdVec *self)
{
	for (size_t k = 0, k_len = self->len; k < k_len; k++)
		trees_store_Logged_drop(&STD_VEC_AT(self, trees_TupleAddressVecU8VecU8_t, k));
	std_vec_free_Checked(self, sizeof(trees_TupleAddressVecU8VecU8_t), _Alignof(trees_TupleAddressVecU8VecU8_t));
}

/* TREES_STORE_WRITER_DECODED_AT is the site of with_pack's IoMutex::default(), the decoded map's. */
static const StdLocation TREES_STORE_WRITER_DECODED_AT = {RUST_FILE, 659, 22};

/* trees_store_Writer_with_pack returns a writer of a pack in a tablespace, temporary when the pack is a scratch file's. */
static void
trees_store_Writer_with_pack(TreesStoreReader reader, bool scratch, const TreesStorePackWriter *pack, uint32 space,
							 TreesStoreWriterCore *result)
{
	result->reader = reader;
	result->space = space;
	result->scratch = scratch;
	result->has_pack = true;
	result->pack = *pack;
	std_HashMap_Default_default(&result->located, &TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE);
	host_lock_IoMutexDecoded_Default_default(&result->decoded, &TREES_STORE_WRITER_DECODED_AT);
	result->trains = false;
	result->has_shared = false;
	std_vec_new(&result->closed, sizeof(ChunksPacksWritersWriting), _Alignof(ChunksPacksWritersWriting));
	std_vec_new(&result->finished, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	result->has_logged = false;
}

/* trees_store_Writer_new returns a writer of new packs in a tablespace of the reader's repository, whose roots others' roots name. */
bool
trees_store_Writer_new(HostIoIo *io, TreesStoreReader reader, uint32 space, TreesStoreWriter *result,
					   ChunksErrorError *error)
{
	TreesStorePackWriter pack;

	if (!chunks_packs_backend_Packs_writer(reader.packs, io, reader.reading, space, &pack.in_dir, error))
		return false;
	trees_store_Writer_with_pack(reader, false, &pack, space, &result->core);
	return true;
}

/*
 * trees_store_Writer_temporary returns a writer that keeps the nodes it
 * writes in a pack of its own in a scratch file, removed when the writer is
 * dropped, for trees whose addresses are wanted and whose nodes are not.
 */
bool
trees_store_Writer_temporary(HostIoIo *io, TreesStoreReader reader, TreesStoreTemporaryWriter *result,
							 ChunksErrorError *error)
{
	TreesStorePackWriter pack;

	if (!chunks_pack_PackWriterScratchFile_scratch(io, &pack.scratch, error))
		return false;
	trees_store_Writer_with_pack(reader, true, &pack, CHUNKS_PACKS_DEFAULT_SPACE, &result->core);
	return true;
}

/* trees_store_Writer_logging makes the writer keep each node it writes from here on for `logged`. */
void
trees_store_Writer_logging(TreesStoreWriter *writer)
{
	TreesStoreWriterCore *self = &writer->core;

	if (self->has_logged)
		trees_store_VecLogged_drop(&self->logged);
	std_vec_new(&self->logged, sizeof(trees_TupleAddressVecU8VecU8_t), _Alignof(trees_TupleAddressVecU8VecU8_t));
	self->has_logged = true;
}

/*
 * trees_store_Writer_brand returns the writer, which it takes, as one whose
 * roots from here on it holds and publishes, writing the same packs.
 */
void
trees_store_Writer_brand(TreesStoreWriter *self, TreesStoreBrandedWriter *result)
{
	result->core = self->core;
	std_vec_new(&result->fresh, sizeof(TreesStoreOptionFresh), _Alignof(TreesStoreOptionFresh));
	std_vec_new(&result->held, sizeof(TreesPublishedHeld), _Alignof(TreesPublishedHeld));
}

/* TREES_STORE_BRANDED_WRITER_BRANDED is a branded writer's own Branded, which only its top-level writes here take. */
#define TREES_STORE_BRANDED_WRITER_BRANDED(x) ((TreesStoreBranded) {&TREES_STORE_WRITER_BRANDED_OPS, &(x)->core})

/* trees_store_BrandedWriter_kept keeps a root the writer's own top-level write returned, giving its place. */
static size_t
trees_store_BrandedWriter_kept(TreesStoreBrandedWriter *self, TreesStoreFresh root)
{
	TreesStoreOptionFresh kept = {true, root};

	std_vec_push_Checked(&self->fresh, &kept, sizeof(kept), _Alignof(TreesStoreOptionFresh));
	return std_wrapping_sub_usize(self->fresh.len, 1);
}

/* trees_store_BrandedWriter_empty is trees' empty through the writer: the empty tree's root it wrote, kept, by its place. */
bool
trees_store_BrandedWriter_empty(TreesStoreBrandedWriter *self, HostIoIo *io, size_t *result, ChunksErrorError *error)
{
	TreesStoreFresh root;

	std_vec_reserve_Checked(&self->fresh, 1, sizeof(TreesStoreOptionFresh), _Alignof(TreesStoreOptionFresh));
	if (!trees_empty(io, TREES_STORE_BRANDED_WRITER_BRANDED(self), &root, error))
		return false;
	*result = trees_store_BrandedWriter_kept(self, root);
	return true;
}

/* trees_store_BrandedWriter_mutate is trees' mutate through the writer, which takes the edits: the root it wrote, kept, by its place. */
bool
trees_store_BrandedWriter_mutate(TreesStoreBrandedWriter *self, HostIoIo *io, TreesStoreNamed tree, uint32 target,
								 StdVec *edits, const TreesShape *shape, size_t *result, ChunksErrorError *error)
{
	TreesStoreFresh root;

	std_vec_reserve_Checked(&self->fresh, 1, sizeof(TreesStoreOptionFresh), _Alignof(TreesStoreOptionFresh));
	if (!trees_mutate_mutate(io, TREES_STORE_BRANDED_WRITER_BRANDED(self), tree, target, edits, shape, &root, error))
		return false;
	*result = trees_store_BrandedWriter_kept(self, root);
	return true;
}

/* trees_store_BrandedWriter_write_root is write_root through the writer, which takes the hashed bytes: the root it wrote, kept, by its place. */
bool
trees_store_BrandedWriter_write_root(TreesStoreBrandedWriter *self, HostIoIo *io, StdVec *hashed,
									 const ChunksTrailerLocator *locators, size_t locators_len,
									 const ChunksTrailerStamps *stamps, size_t *result, ChunksErrorError *error)
{
	TreesStoreFresh root;

	std_vec_reserve_Checked(&self->fresh, 1, sizeof(TreesStoreOptionFresh), _Alignof(TreesStoreOptionFresh));
	if (!trees_store_write_root(io, TREES_STORE_BRANDED_WRITER_BRANDED(self), hashed, locators, locators_len, stamps,
								&root, error))
		return false;
	*result = trees_store_BrandedWriter_kept(self, root);
	return true;
}

/* trees_store_BrandedWriter_shaped returns a builder of a tree of a shape whose nodes the writer writes, borrowing the writer until it finishes. */
void
trees_store_BrandedWriter_shaped(TreesStoreBrandedWriter *self, uint32 target, const TreesShape *shape,
								 TreesStoreBrandedBuilder *result)
{
	trees_build_Builder_shaped(TREES_STORE_WRITER_NODE_STORE(&self->core), target, shape, &result->builder);
	result->writer = self;
}

/* trees_store_BrandedBuilder_finish is Builder's finish through its writer, which takes the builder: the root it wrote, kept, by its place. */
bool
trees_store_BrandedBuilder_finish(TreesStoreBrandedBuilder *self, HostIoIo *io, size_t *result,
								  ChunksErrorError *error)
{
	TreesStoreFresh root;

	std_vec_reserve_Checked(&self->writer->fresh, 1, sizeof(TreesStoreOptionFresh),
							_Alignof(TreesStoreOptionFresh));
	if (!trees_build_Builder_finish(&self->builder, io, &TREES_STORE_WRITER_BRANDED_OPS, &root, error))
		return false;
	*result = trees_store_BrandedWriter_kept(self->writer, root);
	return true;
}

/* trees_store_BrandedWriter_finish_merger is a Merger's finish through the writer, which takes the merger: the root it wrote, kept, by its place. */
bool
trees_store_BrandedWriter_finish_merger(TreesStoreBrandedWriter *self, TreesMutateMerger *merger, HostIoIo *io,
										size_t *result, ChunksErrorError *error)
{
	TreesStoreFresh root;

	std_vec_reserve_Checked(&self->fresh, 1, sizeof(TreesStoreOptionFresh), _Alignof(TreesStoreOptionFresh));
	if (!trees_mutate_Merger_finish(merger, io, TREES_STORE_BRANDED_WRITER_BRANDED(self), &root, error))
		return false;
	*result = trees_store_BrandedWriter_kept(self, root);
	return true;
}

/*
 * trees_store_BrandedWriter_place returns the place of a root the writer's
 * top-level write returned, which a place past the list or a root a hold
 * moved out panics at, as an index past a slice and an unwrap of None do.
 */
static TreesStoreOptionFresh *
trees_store_BrandedWriter_place(const TreesStoreBrandedWriter *self, size_t root)
{
	TreesStoreOptionFresh *place;

	std_index_check(root, self->fresh.len, RUST_FILE, 597, 34);
	place = &STD_VEC_AT(&self->fresh, TreesStoreOptionFresh, root);
	if (!place->has)
		STD_OPTION_UNWRAP_FAILED(597, 34);
	return place;
}

/* trees_store_BrandedWriter_root returns the name of a root the writer's top-level write returned and no hold moved out, by its place. */
TreesStoreTreeRef
trees_store_BrandedWriter_root(const TreesStoreBrandedWriter *self, size_t root)
{
	return trees_store_Fresh_as_ref(&trees_store_BrandedWriter_place(self, root)->value);
}

/*
 * trees_store_BrandedWriter_hold registers a root the writer wrote, named by
 * its place among the roots its top-level writes returned, with the packs it
 * adds to as unpublished while the writer still writes, before it adds the
 * pack that holds the root, so that no collection judges the root dead
 * before something durable names it, and keeps it for its publish.
 */
bool
trees_store_BrandedWriter_hold(TreesStoreBrandedWriter *self, HostIoTakesShort io, ChunksPacksRootsRootSlot *slot,
							   size_t root, ChunksErrorError *error)
{
	TreesStoreOptionFresh *place = trees_store_BrandedWriter_place(self, root);
	TreesStoreTreeRef parts = trees_store_Fresh_as_ref(&place->value);
	TreesPublishedHeld held;

	std_vec_reserve_Checked(&self->held, 1, sizeof(TreesPublishedHeld), _Alignof(TreesPublishedHeld));
	place->has = false;
	if (!chunks_packs_roots_Packs_pin(self->core.reader.packs, io, slot, parts.address, parts.locator, &held.pin,
									  error))
		return false;
	held.root = place->value;
	held.repo = trees_published_RepoId_of(self->core.reader.packs);
	std_vec_push_Checked(&self->held, &held, sizeof(held), _Alignof(TreesPublishedHeld));
	return true;
}

/* trees_store_Writer_next_pack starts the writer's next pack, which shares the writer's dictionary or trains one of its own as the writer's packs do. */
static bool
trees_store_Writer_next_pack(const TreesStoreWriterCore *self, HostIoIo *io, ChunksPackPackWriterInDir *result,
							 ChunksErrorError *error)
{
	if (!chunks_packs_backend_Packs_writer(self->reader.packs, io, self->reader.reading, self->space, result, error))
		return false;
	if (self->has_shared)
	{
		ChunksPackSharedDictionary shared = std_arc_clone(self->shared);

		chunks_pack_PackWriterInDir_share(result, &shared);
	}
	else if (self->trains)
		chunks_pack_PackWriterInDir_train(result);
	return true;
}

/* trees_store_Writer_logged takes the nodes a logging writer kept, children before parents, leaving it none. */
void
trees_store_Writer_logged(TreesStoreWriterCore *self, StdVec *result)
{
	std_vec_new(result, sizeof(trees_TupleAddressVecU8VecU8_t), _Alignof(trees_TupleAddressVecU8VecU8_t));
	if (self->has_logged)
	{
		StdVec		taken = self->logged;

		self->logged = *result;
		*result = taken;
	}
}

/* trees_store_Writer_train_dictionaries makes every pack the writer writes train a dictionary of its own. */
void
trees_store_Writer_train_dictionaries(TreesStoreWriterCore *self)
{
	if (self->has_pack)
	{
		if (self->scratch)
			chunks_pack_PackWriterScratchFile_train(&self->pack.scratch);
		else
			chunks_pack_PackWriterInDir_train(&self->pack.in_dir);
	}
	self->trains = true;
}

/* trees_store_Writer_share_dictionary makes every pack the writer writes share a dictionary, which it takes, trained once for all that share it. */
void
trees_store_Writer_share_dictionary(TreesStoreWriterCore *self, ChunksPackSharedDictionary shared)
{
	if (self->has_pack)
	{
		ChunksPackSharedDictionary copy = std_arc_clone(shared);

		if (self->scratch)
			chunks_pack_PackWriterScratchFile_share(&self->pack.scratch, &copy);
		else
			chunks_pack_PackWriterInDir_share(&self->pack.in_dir, &copy);
	}
	if (self->has_shared)
		chunks_pack_SharedDictionary_drop(self->shared);
	self->shared = shared;
	self->has_shared = true;
}

/* trees_store_Writer_reader returns the reader the writer reads existing nodes through. */
TreesStoreReader
trees_store_Writer_reader(const TreesStoreWriterCore *self)
{
	return self->reader;
}

/* trees_store_Writer_space returns the tablespace whose directory the writer's packs go in. */
uint32
trees_store_Writer_space(const TreesStoreWriterCore *self)
{
	return self->space;
}

/* trees_store_Writer_holds reports whether the writer wrote a chunk to the pack it is writing, where a read of it by its address alone finds the last copy. */
bool
trees_store_Writer_holds(const TreesStoreWriterCore *self, const ChunksAddressAddress *address)
{
	return std_HashMap_contains_key(&self->located, &TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE, address);
}

/* trees_store_Writer_drop_pack drops the pack being written, when the writer has it. */
static void
trees_store_Writer_drop_pack(TreesStoreWriterCore *self)
{
	if (!self->has_pack)
		return;
	self->has_pack = false;
	if (self->scratch)
		chunks_pack_PackWriterScratchFile_drop(&self->pack.scratch);
	else
		chunks_pack_PackWriterInDir_drop(&self->pack.in_dir);
}

/* trees_store_WriterCore_drop is a Writer's core's drop glue: its pack, its maps, its shared dictionary, its closed packs, and what it finished and logged. */
static void
trees_store_WriterCore_drop(TreesStoreWriterCore *self)
{
	trees_store_Writer_drop_pack(self);
	std_HashMap_drop(&self->located, &TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE);
	std_HashMap_drop(&self->decoded.value, &TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE);
	if (self->has_shared)
	{
		self->has_shared = false;
		chunks_pack_SharedDictionary_drop(self->shared);
	}
	for (size_t k = 0, k_len = self->closed.len; k < k_len; k++)
		chunks_packs_writers_Writing_drop(&STD_VEC_AT(&self->closed, ChunksPacksWritersWriting, k));
	std_vec_free_Checked(&self->closed, sizeof(ChunksPacksWritersWriting), _Alignof(ChunksPacksWritersWriting));
	std_vec_free_Checked(&self->finished, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	if (self->has_logged)
	{
		self->has_logged = false;
		trees_store_VecLogged_drop(&self->logged);
	}
}

/* trees_store_BrandedWriter_drop is a branded Writer's drop glue: its writer, then the roots it holds. */
void
trees_store_BrandedWriter_drop(TreesStoreBrandedWriter *self)
{
	trees_store_WriterCore_drop(&self->core);
	std_vec_free_Checked(&self->fresh, sizeof(TreesStoreOptionFresh), _Alignof(TreesStoreOptionFresh));
	for (size_t k = 0, k_len = self->held.len; k < k_len; k++)
		trees_published_Held_drop(&STD_VEC_AT(&self->held, TreesPublishedHeld, k));
	std_vec_free_Checked(&self->held, sizeof(TreesPublishedHeld), _Alignof(TreesPublishedHeld));
}

/* trees_store_Writer_finish_frame_0_t is finish's state across its pad: the writer it takes, and its finished pack, while live. */
typedef struct trees_store_Writer_finish_frame_0_t
{
	TreesStoreWriterCore *self;
	HostIoTakesShort io;
	StdVec	   *result;
	ChunksErrorError *error;
	bool		ok;
	bool		self_live;
	ChunksPackPack pack;
	bool		pack_live;
} trees_store_Writer_finish_frame_0_t;

/* trees_store_Writer_finish_pad_0 is finish's pad: the finished pack, then the writer, while live. */
static void
trees_store_Writer_finish_pad_0(void *frame_)
{
	trees_store_Writer_finish_frame_0_t *frame = frame_;

	if (frame->pack_live)
	{
		frame->pack_live = false;
		chunks_pack_Pack_drop(&frame->pack);
	}
	if (frame->self_live)
	{
		frame->self_live = false;
		trees_store_WriterCore_drop(frame->self);
	}
}

/* trees_store_Writer_finish_body_0 is finish's body. */
static void
trees_store_Writer_finish_body_0(void *frame_)
{
	trees_store_Writer_finish_frame_0_t *frame = frame_;
	TreesStoreWriterCore *self = frame->self;

	frame->self_live = true;
	if (self->has_pack)
	{
		self->has_pack = false;
		if (chunks_pack_PackWriterInDir_is_empty(&self->pack.in_dir))
		{
			if (!chunks_pack_PackWriterInDir_abandon(&self->pack.in_dir, frame->io, frame->error))
			{
				trees_store_Writer_finish_pad_0(frame);
				return;
			}
		}
		else
		{
			if (!chunks_pack_PackWriterInDir_finish(&self->pack.in_dir, frame->io, &frame->pack, frame->error))
			{
				trees_store_Writer_finish_pad_0(frame);
				return;
			}
			frame->pack_live = true;
			if (!std_vec_try_reserve(&self->finished, 1, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId)))
			{
				HostError	host_error = host_Error_out_of_memory();

				*frame->error = chunks_error_Error_FromError_from(&host_error);
				trees_store_Writer_finish_pad_0(frame);
				return;
			}
			std_vec_push_Checked(&self->finished, &frame->pack.id, sizeof(ChunksPackPackId),
								 _Alignof(ChunksPackPackId));
			frame->pack_live = false;
			if (!chunks_packs_backend_Packs_add_deferred(self->reader.packs, frame->io, self->reader.reading,
														 &frame->pack, frame->error))
			{
				trees_store_Writer_finish_pad_0(frame);
				return;
			}
		}
	}
	*frame->result = self->finished;
	std_vec_new(&self->finished, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	frame->ok = true;
	trees_store_Writer_finish_pad_0(frame);
}

/*
 * trees_store_Writer_finish finishes the pack and adds it to the
 * repository, or removes it when nothing was written, and returns the ids of
 * every pack the writer finished, in order, taking the writer.
 */
static bool
trees_store_WriterCore_finish(TreesStoreWriterCore *self, HostIoTakesShort io, StdVec *result,
							  ChunksErrorError *error)
{
	trees_store_Writer_finish_frame_0_t frame = {.self = self, .io = io, .result = result, .error = error};

	std_landing(&frame, trees_store_Writer_finish_body_0, trees_store_Writer_finish_pad_0);
	return frame.ok;
}

/* trees_store_BrandedWriter_publish_frame_0_t is publish's state across its pad: the roots from `at` on, the published ones before it, and the ids, while live. */
typedef struct trees_store_BrandedWriter_publish_frame_0_t
{
	TreesStoreBrandedWriter *self;
	HostIoTakesShort io;
	StdVec	   *published;
	StdVec	   *finished;
	ChunksErrorError *error;
	bool		ok;
	size_t		at;
	bool		published_live;
	bool		finished_live;
} trees_store_BrandedWriter_publish_frame_0_t;

/* trees_store_BrandedWriter_publish_pad_0 is publish's pad: the held roots not yet published, the published ones, and the ids, while live. */
static void
trees_store_BrandedWriter_publish_pad_0(void *frame_)
{
	trees_store_BrandedWriter_publish_frame_0_t *frame = frame_;
	StdVec	   *held = &frame->self->held;
	size_t		at = frame->at;

	frame->at = held->len;
	for (size_t k = 0, k_len = std_wrapping_sub_usize(held->len, at); k < k_len; k++)
		trees_published_Held_drop(&STD_VEC_AT(held, TreesPublishedHeld, std_wrapping_add_usize(at, k)));
	std_vec_free_Checked(held, sizeof(TreesPublishedHeld), _Alignof(TreesPublishedHeld));
	std_vec_free_Checked(&frame->self->fresh, sizeof(TreesStoreOptionFresh), _Alignof(TreesStoreOptionFresh));
	if (frame->published_live)
	{
		frame->published_live = false;
		for (size_t k = 0, k_len = frame->published->len; k < k_len; k++)
			trees_published_Published_drop(&STD_VEC_AT(frame->published, TreesPublishedPublished, k));
		std_vec_free_Checked(frame->published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished));
	}
	if (frame->finished_live)
	{
		frame->finished_live = false;
		std_vec_free_Checked(frame->finished, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	}
}

/* trees_store_BrandedWriter_publish_body_0 is publish's body. */
static void
trees_store_BrandedWriter_publish_body_0(void *frame_)
{
	trees_store_BrandedWriter_publish_frame_0_t *frame = frame_;
	StdVec	   *held = &frame->self->held;

	if (!trees_store_WriterCore_finish(&frame->self->core, frame->io, frame->finished, frame->error))
	{
		trees_store_BrandedWriter_publish_pad_0(frame);
		return;
	}
	frame->finished_live = true;
	std_vec_with_capacity_Checked(frame->published, held->len, sizeof(TreesPublishedPublished),
								  _Alignof(TreesPublishedPublished));
	frame->published_live = true;
	for (; frame->at < held->len; frame->at = std_wrapping_add_usize(frame->at, 1))
	{
		TreesPublishedPublished published = trees_published_Held_published(STD_VEC_AT(held, TreesPublishedHeld,
																					  frame->at));

		std_vec_push_Checked(frame->published, &published, sizeof(published), _Alignof(TreesPublishedPublished));
	}
	frame->published_live = false;
	frame->finished_live = false;
	frame->ok = true;
	trees_store_BrandedWriter_publish_pad_0(frame);
}

/*
 * trees_store_BrandedWriter_publish finishes the writer, which it takes, and
 * publishes the roots it held, once it added the packs that hold them, and
 * returns them in the order it held them with the ids of the packs it
 * finished.
 */
bool
trees_store_BrandedWriter_publish(TreesStoreBrandedWriter *self, HostIoTakesShort io, StdVec *published,
								  StdVec *finished, ChunksErrorError *error)
{
	trees_store_BrandedWriter_publish_frame_0_t frame = {.self = self, .io = io, .published = published,
	.finished = finished, .error = error};

	std_landing(&frame, trees_store_BrandedWriter_publish_body_0, trees_store_BrandedWriter_publish_pad_0);
	return frame.ok;
}

/* trees_store_PackReader_drop is the drop of the PackReader an Arc holds. */
static void
trees_store_PackReader_drop(void *reader)
{
	chunks_pack_PackReader_drop(reader);
}

/*
 * trees_store_Writer_own reports where the pack being written holds the copy
 * of a chunk a locator names: the locator itself when it names that pack,
 * none when it names a pack the repository holds, whose reader reads that
 * copy, and the last copy written for an unknown locator or one into a pack
 * no repository holds, such as another temporary writer's.  An unknown
 * locator of a chunk whose copies' stamps differ is refused.
 */
static bool
trees_store_Writer_own(const TreesStoreWriterCore *self, HostIoIo *io, const ChunksAddressAddress *address,
					   ChunksTrailerLocator locator, bool *some, ChunksTrailerLocator *result,
					   ChunksErrorError *error)
{
	const TreesStoreLocated *located = trees_store_located_get(&self->located, address);
	ChunksTrailerLocator last;
	bool		has;
	StdArcInner *reader;
	ChunksTrailerLocator forwarded;

	*some = false;
	if (located == NULL)
		return true;
	last = located->last;
	if (!chunks_trailer_Locator_is_known(&locator))
	{
		if (located->several)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0),
			STD_FMT_LIT(" was written with other stamps, so its address alone names no one copy")};
			StdFmtArg	args[] = {{address, chunks_address_Address_Display_fmt}};
			StdFmtArguments arguments = {pieces, 3, args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*error = chunks_error_Error_corrupt(&message);
			return false;
		}
		*some = true;
		*result = last;
		return true;
	}
	if (locator.pack == last.pack)
	{
		*some = true;
		*result = locator;
		return true;
	}
	if (!chunks_packs_backend_Packs_pack_reader(self->reader.packs, io, self->reader.reading, locator.pack, &has,
												&reader, error))
		return false;
	if (has)
	{
		std_arc_drop(reader, sizeof(ChunksPackPackReader), _Alignof(ChunksPackPackReader),
					 trees_store_PackReader_drop);
		return true;
	}
	if (!chunks_packs_backend_Packs_forwarded(self->reader.packs, io, self->reader.reading, locator, &has, &forwarded,
											  error))
		return false;
	*some = !has;
	*result = last;
	return true;
}

/* trees_store_Writer_pack_read reads the stored bytes of the copy of a chunk at a place in the pack being written. */
static bool
trees_store_Writer_pack_read(const TreesStoreWriterCore *self, HostIoIo *io, ChunksTrailerLocator at,
							 const ChunksAddressAddress *address, ChunksChunkChunk *result, ChunksErrorError *error)
{
	if (self->scratch)
		return chunks_pack_PackWriterScratchFile_read(&self->pack.scratch, io, at, address, result, error);
	return chunks_pack_PackWriterInDir_read(&self->pack.in_dir, io, at, address, result, error);
}

/* trees_store_Writer_stored_frame_0_t is stored's state across its pad: the first read's error, while it waits on the second read. */
typedef struct trees_store_Writer_stored_frame_0_t
{
	const TreesStoreWriterCore *self;
	HostIoIo   *io;
	const ChunksAddressAddress *address;
	ChunksTrailerLocator at;
	ChunksTrailerLocator last;
	ChunksTrailerLocator *result_0;
	ChunksChunkChunk *result_1;
	ChunksErrorError *error;
	bool		ok;
	ChunksErrorError why;
	bool		why_live;
} trees_store_Writer_stored_frame_0_t;

/* trees_store_Writer_stored_pad_0 is stored's pad: the first read's error, while live. */
static void
trees_store_Writer_stored_pad_0(void *frame_)
{
	trees_store_Writer_stored_frame_0_t *frame = frame_;

	if (frame->why_live)
	{
		frame->why_live = false;
		chunks_error_Error_drop(&frame->why);
	}
}

/*
 * trees_store_Writer_stored_body_0 is stored's read: the copy at its place,
 * or, when that place is corrupt and isn't the last copy's, the last copy,
 * the first read's corruption kept when the last copy is corrupt too.
 */
static void
trees_store_Writer_stored_body_0(void *frame_)
{
	trees_store_Writer_stored_frame_0_t *frame = frame_;

	if (trees_store_Writer_pack_read(frame->self, frame->io, frame->at, frame->address, frame->result_1,
									 &frame->why))
	{
		*frame->result_0 = frame->at;
		frame->ok = true;
		return;
	}
	if (frame->why.tag != CHUNKS_ERROR_ERROR_CORRUPT || chunks_trailer_Locator_PartialEq_eq(&frame->at, &frame->last))
	{
		*frame->error = frame->why;
		return;
	}
	frame->why_live = true;
	if (trees_store_Writer_pack_read(frame->self, frame->io, frame->last, frame->address, frame->result_1,
									 frame->error))
	{
		*frame->result_0 = frame->last;
		frame->ok = true;
		trees_store_Writer_stored_pad_0(frame);
		return;
	}
	if (frame->error->tag == CHUNKS_ERROR_ERROR_CORRUPT)
	{
		chunks_error_Error_drop(frame->error);
		*frame->error = frame->why;
		frame->why_live = false;
		return;
	}
	trees_store_Writer_stored_pad_0(frame);
}

/*
 * trees_store_Writer_stored reports where the pack being written holds the
 * copy of a chunk a locator names, with its stored bytes, if it is there, or
 * the last copy written when the locator names a place in a pack of the same
 * number that holds no copy of the chunk, as another temporary writer's
 * does.
 */
static bool
trees_store_Writer_stored(const TreesStoreWriterCore *self, HostIoIo *io, const ChunksAddressAddress *address,
						  ChunksTrailerLocator locator, bool *some, ChunksTrailerLocator *result_0,
						  ChunksChunkChunk *result_1, ChunksErrorError *error)
{
	trees_store_Writer_stored_frame_0_t frame = {.self = self, .io = io, .address = address, .result_0 = result_0,
	.result_1 = result_1, .error = error};
	const TreesStoreLocated *located;

	if (!trees_store_Writer_own(self, io, address, locator, some, &frame.at, error))
		return false;
	if (!*some)
		return true;
	if (!self->has_pack)
	{
		*some = false;
		return true;
	}
	located = trees_store_located_get(&self->located, address);
	frame.last = located == NULL ? frame.at : located->last;
	std_landing(&frame, trees_store_Writer_stored_body_0, trees_store_Writer_stored_pad_0);
	return frame.ok;
}

/* trees_store_Writer_Branded_wrote is Writer's Branded wrote: whether the locator names its pack being written or one it closed. */
static bool
trees_store_Writer_Branded_wrote(void *self, ChunksTrailerLocator locator)
{
	const TreesStoreWriterCore *writer = self;

	if (writer->has_pack && (writer->scratch ? chunks_pack_PackWriterScratchFile_number(&writer->pack.scratch) :
							 chunks_pack_PackWriterInDir_number(&writer->pack.in_dir)) == locator.pack)
		return true;
	for (size_t k = 0, k_len = writer->closed.len; k < k_len; k++)
		if (chunks_packs_writers_Writing_number(&STD_VEC_AT(&writer->closed, ChunksPacksWritersWriting, k)) ==
			locator.pack)
			return true;
	return false;
}

/* trees_store_Writer_NodeStore_as_dyn is Writer's NodeStore as_dyn: itself as a NodeStore. */
static TreesStoreNodeStore
trees_store_Writer_NodeStore_as_dyn(void *self)
{
	return TREES_STORE_WRITER_NODE_STORE(self);
}

/* trees_store_moved_t is a chunk a closure moves out to its taker, with where its owner keeps whether it still holds it. */
typedef struct trees_store_moved_t
{
	ChunksChunkChunk *chunk;
	bool	   *live;
} trees_store_moved_t;

/* trees_store_moved_closure_0 is |_| Ok(stored): the chunk, moved out of its owner. */
static bool
trees_store_moved_closure_0(void *captures, HostIoIo *io, ChunksChunkChunk *result, ChunksErrorError *error)
{
	trees_store_moved_t *moved = captures;

	*result = *moved->chunk;
	*moved->live = false;
	return true;
}

/* trees_store_Writer_read_frame_0_t is the NodeStore read's state across its pad: the decoded map's guard, the stored copy, and the flat bytes, while live. */
typedef struct trees_store_Writer_read_frame_0_t
{
	TreesStoreWriterCore *self;
	HostIoIo   *io;
	const ChunksAddressAddress *address;
	ChunksTrailerLocator locator;
	TreesNodeFlat *result;
	ChunksErrorError *error;
	bool		ok;
	HostLockLockedRwLockWriteGuard guard;
	bool		guard_live;
	ChunksChunkChunk stored;
	bool		stored_live;
	StdVec		flat;
	bool		flat_live;
} trees_store_Writer_read_frame_0_t;

/* trees_store_Writer_read_pad_0 is the read's pad: the guard, the stored copy, and the flat bytes, while live. */
static void
trees_store_Writer_read_pad_0(void *frame_)
{
	trees_store_Writer_read_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
	}
	if (frame->stored_live)
	{
		frame->stored_live = false;
		chunks_chunk_Chunk_drop(&frame->stored);
	}
	if (frame->flat_live)
	{
		frame->flat_live = false;
		std_vec_free_Checked(&frame->flat, 1, 1);
	}
}

/* trees_store_Writer_read_decoded takes the guard of a temporary writer's decoded nodes and returns them. */
static StdHashMap *
trees_store_Writer_read_decoded(trees_store_Writer_read_frame_0_t *frame)
{
	frame->guard = host_lock_IoMutex_lock(&frame->self->decoded.header, &TREES_STORE_DECODED_LOCK_TYPE,
										  &frame->io->held);
	frame->guard_live = true;
	return host_lock_LockedRwLockWriteGuard_DerefMut_deref_mut(&frame->guard);
}

/* trees_store_Writer_read_body_0 is the NodeStore read's body. */
static void
trees_store_Writer_read_body_0(void *frame_)
{
	trees_store_Writer_read_frame_0_t *frame = frame_;
	TreesStoreWriterCore *self = frame->self;
	HostError	host_error;
	ChunksTrailerLocator at;
	bool		some;

	if (!host_io_Io_check(frame->io, &host_error))
	{
		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return;
	}
	if (!trees_store_Writer_own(self, frame->io, frame->address, frame->locator, &some, &at, frame->error))
		return;
	if (!some)
	{
		frame->ok = trees_store_Reader_read(&self->reader, frame->io, frame->address, frame->locator, frame->result,
											frame->error);
		return;
	}
	if (self->scratch)
	{
		StdHashMap *decoded = trees_store_Writer_read_decoded(frame);
		const uint8 *elem = std_HashMap_get(decoded, &TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE, &at);
		const StdVec *kept = elem == NULL ? NULL : (const StdVec *) (elem + offsetof(trees_TupleLocatorVecU8_t, f1));

		if (kept != NULL)
		{
			TreesNodeNode node = trees_node_Node_view(kept->ptr, kept->len);
			ChunksAddressAddress address = trees_node_Node_address(&node);

			if (chunks_address_Address_PartialEq_eq(&address, frame->address))
			{
				std_vec_clone(&frame->flat, kept, 1, 1);
				frame->flat_live = true;
			}
		}
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
		if (frame->flat_live)
		{
			frame->result->f0.tag = HOST_CACHE_READ_UNCACHED;
			frame->result->f0.u.uncached = frame->flat;
			frame->flat_live = false;
			frame->ok = true;
			return;
		}
	}
	if (!trees_store_Writer_stored(self, frame->io, frame->address, frame->locator, &some, &at, &frame->stored,
								   frame->error))
		return;
	if (!some)
	{
		frame->ok = trees_store_Reader_read(&self->reader, frame->io, frame->address, frame->locator, frame->result,
											frame->error);
		return;
	}
	frame->stored_live = true;
	if (self->scratch)
	{
		StdHashMap *decoded;
		StdVec		copy;
		trees_TupleLocatorVecU8_t old;

		if (!trees_node_flatten(&frame->stored, &frame->flat, frame->error))
		{
			trees_store_Writer_read_pad_0(frame);
			return;
		}
		frame->flat_live = true;
		decoded = trees_store_Writer_read_decoded(frame);
		std_vec_clone(&copy, &frame->flat, 1, 1);
		if (std_HashMap_insert(decoded, &TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE, &at, &copy, &old.f1))
			std_vec_free_Checked(&old.f1, 1, 1);
		frame->guard_live = false;
		host_lock_LockedRwLockWriteGuard_drop(&frame->guard);
		frame->result->f0.tag = HOST_CACHE_READ_UNCACHED;
		frame->result->f0.u.uncached = frame->flat;
		frame->flat_live = false;
		frame->ok = true;
		trees_store_Writer_read_pad_0(frame);
		return;
	}
	{
		HostCacheKey key = trees_store_key(self->reader.packs, frame->address, at);
		trees_store_moved_t moved = {&frame->stored, &frame->stored_live};

		frame->ok = trees_store_cached(frame->io, self->reader.placement, &key, trees_store_moved_closure_0, &moved,
									   frame->result, frame->error);
	}
	trees_store_Writer_read_pad_0(frame);
}

/*
 * trees_store_Writer_NodeStore_read is Writer's NodeStore read: a copy it
 * wrote from its own pack, a temporary writer's from the flat bytes it keeps,
 * since the read owns what it returns, and any other through its reader.
 */
static bool
trees_store_Writer_NodeStore_read(void *self, HostIoIo *io, const ChunksAddressAddress *address,
								  ChunksTrailerLocator locator, TreesNodeFlat *result, ChunksErrorError *error)
{
	trees_store_Writer_read_frame_0_t frame = {.self = self, .io = io, .address = address, .locator = locator,
	.result = result, .error = error};

	std_landing(&frame, trees_store_Writer_read_body_0, trees_store_Writer_read_pad_0);
	return frame.ok;
}

/* trees_store_Writer_NodeStore_read_record is Writer's NodeStore read_record: a copy it wrote from its own pack, any other through its reader. */
static bool
trees_store_Writer_NodeStore_read_record(void *self, HostIoIo *io, const ChunksAddressAddress *address,
										 ChunksTrailerLocator locator, ChunksChunkChunk *result,
										 ChunksErrorError *error)
{
	TreesStoreWriterCore *writer = self;
	ChunksTrailerLocator at;
	bool		some;

	if (!trees_store_Writer_stored(writer, io, address, locator, &some, &at, result, error))
		return false;
	if (some)
		return true;
	return trees_store_Reader_read_record(&writer->reader, io, address, locator, result, error);
}

/* trees_store_Writer_write_tree_frame_0_t is write_tree's state across its pad: what it holds while it writes, each while live. */
typedef struct trees_store_Writer_write_tree_frame_0_t
{
	TreesStoreWriterCore *self;
	HostIoIo   *io;
	const ChunksTrailerLocator *locators;
	size_t		locators_len;
	const ChunksTrailerStamps *stamps;
	TreesStoreTree *result;
	ChunksErrorError *error;
	bool		ok;
	StdVec		hashed;
	bool		hashed_live;
	StdVec		current;
	bool		current_live;
	ChunksChunkChunk stored;
	bool		stored_live;
	StdVec		trailer;
	bool		trailer_live;
	ChunksPackPackWriterInDir full;
	ChunksPackPack pack;
	bool		pack_live;
	ChunksPacksWritersWriting writing;
	bool		writing_live;
} trees_store_Writer_write_tree_frame_0_t;

/* trees_store_Writer_write_tree_pad_0 is write_tree's pad: what it holds, each while live. */
static void
trees_store_Writer_write_tree_pad_0(void *frame_)
{
	trees_store_Writer_write_tree_frame_0_t *frame = frame_;

	if (frame->writing_live)
	{
		frame->writing_live = false;
		chunks_packs_writers_Writing_drop(&frame->writing);
	}
	if (frame->pack_live)
	{
		frame->pack_live = false;
		chunks_pack_Pack_drop(&frame->pack);
	}
	if (frame->trailer_live)
	{
		frame->trailer_live = false;
		std_vec_free_Checked(&frame->trailer, 1, 1);
	}
	if (frame->stored_live)
	{
		frame->stored_live = false;
		chunks_chunk_Chunk_drop(&frame->stored);
	}
	if (frame->current_live)
	{
		frame->current_live = false;
		std_vec_free_Checked(&frame->current, sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator));
	}
	if (frame->hashed_live)
	{
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
	}
}

/*
 * trees_store_Writer_write_tree_rolled is InDir's rolled: once the pack
 * being written reaches the target size, the writer closes it, adding it as
 * still being written, and starts its next.
 */
static bool
trees_store_Writer_write_tree_rolled(trees_store_Writer_write_tree_frame_0_t *frame)
{
	TreesStoreWriterCore *self = frame->self;
	ChunksPackPackWriterInDir next;

	if (!self->has_pack || chunks_pack_PackWriterInDir_size(&self->pack.in_dir) < CHUNKS_PACK_PACK_TARGET)
		return true;
	if (!trees_store_Writer_next_pack(self, frame->io, &next, frame->error))
		return false;
	frame->full = self->pack.in_dir;
	self->pack.in_dir = next;
	if (!chunks_pack_PackWriterInDir_finish(&frame->full, HOST_IO_IO_TAKES_SHORT(frame->io), &frame->pack,
											frame->error))
		return false;
	frame->pack_live = true;
	if (!std_vec_try_reserve(&self->finished, 1, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId)))
	{
		HostError	host_error = host_Error_out_of_memory();

		*frame->error = chunks_error_Error_FromError_from(&host_error);
		return false;
	}
	std_vec_push_Checked(&self->finished, &frame->pack.id, sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId));
	frame->pack_live = false;
	if (!chunks_packs_backend_Packs_add_still_writing(self->reader.packs, frame->io, self->reader.reading,
													  &frame->pack, &frame->writing, frame->error))
		return false;
	frame->writing_live = true;
	std_vec_push_Checked(&self->closed, &frame->writing, sizeof(ChunksPacksWritersWriting),
						 _Alignof(ChunksPacksWritersWriting));
	frame->writing_live = false;
	std_HashMap_clear(&self->located, &TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE);
	return true;
}

/* trees_store_Writer_write_tree_body_0 is write_tree's body. */
static void
trees_store_Writer_write_tree_body_0(void *frame_)
{
	trees_store_Writer_write_tree_frame_0_t *frame = frame_;
	TreesStoreWriterCore *self = frame->self;
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	TreesStoreLocated *located;
	bool		has_current = false;
	uint64		sum;
	uint8		kind;

	for (size_t k = 0, k_len = self->scratch ? 0 : frame->locators_len; k < k_len; k++)
	{
		if (chunks_trailer_Locator_is_known(&frame->locators[k]) && frame->locators[k].pack == 0)
		{
			ChunksAddressAddress named = chunks_address_Address_of(frame->hashed.ptr, frame->hashed.len);
			StdFmtPiece pieces[] = {STD_FMT_LIT("I1b: node "), STD_FMT_ARG(0),
			STD_FMT_LIT(" names a temporary writer's chunk")};
			StdFmtArg	args[] = {{&named, chunks_address_Address_Display_fmt}};
			StdFmtArguments arguments = {pieces, 3, args};

			frame->error->tag = CHUNKS_ERROR_ERROR_INVARIANT;
			std_fmt_format_Checked(&frame->error->u.invariant, &arguments);
			trees_store_Writer_write_tree_pad_0(frame);
			return;
		}
	}
	if (!self->scratch)
	{
		if (!chunks_packs_backend_Packs_current(self->reader.packs, frame->io, self->reader.reading, frame->locators,
												frame->locators_len, &has_current, &frame->current, frame->error))
		{
			trees_store_Writer_write_tree_pad_0(frame);
			return;
		}
		frame->current_live = has_current;
	}
	chunks_chunk_Chunk_stored(frame->hashed.ptr, frame->hashed.len,
							  has_current ? (const ChunksTrailerLocator *) frame->current.ptr : frame->locators,
							  has_current ? frame->current.len : frame->locators_len, frame->stamps, &frame->stored);
	frame->stored_live = true;
	address = chunks_chunk_Chunk_address(&frame->stored);
	if (!self->has_pack)
	{
		*frame->error = chunks_error_Error_corrupt_RefStr("a finished writer cannot write",
														  strlen("a finished writer cannot write"));
		trees_store_Writer_write_tree_pad_0(frame);
		return;
	}
	std_index_check(0, frame->hashed.len, RUST_FILE, 808, 63);
	kind = ((const uint8 *) frame->hashed.ptr)[0];
	{
		StdBytes	bytes = chunks_chunk_Chunk_bytes(&frame->stored);
		bool		written = self->scratch ?
			chunks_pack_PackWriterScratchFile_write(&self->pack.scratch, frame->io, address, bytes.ptr, bytes.len,
													kind == CHUNKS_HEADER_TREE_LEAF, &locator, frame->error) :
			chunks_pack_PackWriterInDir_write(&self->pack.in_dir, frame->io, address, bytes.ptr, bytes.len,
											  kind == CHUNKS_HEADER_TREE_LEAF, &locator, frame->error);

		if (!written)
		{
			trees_store_Writer_write_tree_pad_0(frame);
			return;
		}
	}
	chunks_trailer_store(NULL, 0, NULL, 0, frame->stamps, &frame->trailer);
	frame->trailer_live = true;
	sum = XXH3_64bits(frame->trailer.ptr, frame->trailer.len);
	frame->trailer_live = false;
	std_vec_free_Checked(&frame->trailer, 1, 1);
	located = trees_store_located_get(&self->located, &address);
	if (located == NULL)
	{
		TreesStoreLocated first = {locator, sum, false};
		TreesStoreLocated old;

		std_HashMap_insert(&self->located, &TREES_STORE_HASH_MAP_ADDRESS_LOCATED_TYPE, &address, &first, &old);
		located = trees_store_located_get(&self->located, &address);
	}
	located->several |= located->sum != sum;
	located->last = locator;
	located->sum = sum;
	if (self->scratch)
	{
		*frame->result = trees_store_Tree_at(address, locator);
		frame->ok = true;
		trees_store_Writer_write_tree_pad_0(frame);
		return;
	}
	if (kind == CHUNKS_HEADER_TREE_INTERNAL)
	{
		HostCacheKey key = trees_store_key(self->reader.packs, &address, locator);
		trees_store_moved_t moved = {&frame->stored, &frame->stored_live};
		TreesNodeFlat flat;

		if (!trees_store_cached(frame->io, self->reader.placement, &key, trees_store_moved_closure_0, &moved, &flat,
								frame->error))
		{
			trees_store_Writer_write_tree_pad_0(frame);
			return;
		}
		trees_node_Flat_drop(&flat);
	}
	if (!trees_store_Writer_write_tree_rolled(frame))
	{
		trees_store_Writer_write_tree_pad_0(frame);
		return;
	}
	if (self->has_logged)
	{
		trees_TupleAddressVecU8VecU8_t logged;

		chunks_trailer_store(NULL, 0, NULL, 0, frame->stamps, &frame->trailer);
		frame->trailer_live = true;
		logged.f0 = address;
		logged.f1 = frame->hashed;
		logged.f2 = frame->trailer;
		std_vec_push_Checked(&self->logged, &logged, sizeof(logged), _Alignof(trees_TupleAddressVecU8VecU8_t));
		frame->hashed_live = false;
		frame->trailer_live = false;
	}
	*frame->result = trees_store_Tree_at(address, locator);
	frame->ok = true;
	trees_store_Writer_write_tree_pad_0(frame);
}

/* trees_store_Writer_write_tree is `write`, which takes the hashed bytes, returning the chunk's name as a tree. */
static bool
trees_store_Writer_write_tree(TreesStoreWriterCore *self, HostIoIo *io, StdVec *hashed,
							  const ChunksTrailerLocator *locators, size_t locators_len,
							  const ChunksTrailerStamps *stamps, TreesStoreTree *result, ChunksErrorError *error)
{
	trees_store_Writer_write_tree_frame_0_t frame = {.self = self, .io = io, .locators = locators,
		.locators_len = locators_len, .stamps = stamps, .result = result, .error = error, .hashed = *hashed,
	.hashed_live = true};

	std_landing(&frame, trees_store_Writer_write_tree_body_0, trees_store_Writer_write_tree_pad_0);
	return frame.ok;
}

/* trees_store_Writer_NodeStore_write is Writer's NodeStore write: write_tree's name as one a writer wrote. */
static bool
trees_store_Writer_NodeStore_write(void *self, HostIoIo *io, StdVec *hashed, const ChunksTrailerLocator *locators,
								   size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTreeRef *result,
								   ChunksErrorError *error)
{
	TreesStoreTree tree;

	if (!trees_store_Writer_write_tree(self, io, hashed, locators, locators_len, stamps, &tree, error))
		return false;
	*result = trees_store_TreeRef_of(tree);
	return true;
}

/* trees_store_Writer_finish is a plain Writer's finish, the core's. */
bool
trees_store_Writer_finish(TreesStoreWriter *self, HostIoTakesShort io, StdVec *result, ChunksErrorError *error)
{
	return trees_store_WriterCore_finish(&self->core, io, result, error);
}

/* trees_store_Writer_drop is a plain Writer's drop glue, its core's. */
void
trees_store_Writer_drop(TreesStoreWriter *self)
{
	trees_store_WriterCore_drop(&self->core);
}

/* trees_store_TemporaryWriter_drop is a temporary Writer's drop glue, its core's, which removes its scratch pack. */
void
trees_store_TemporaryWriter_drop(TreesStoreTemporaryWriter *self)
{
	trees_store_WriterCore_drop(&self->core);
}

/* TREES_STORE_WRITER_BRANDED_OPS is Writer's Branded table, with its NodeStore table first. */
const TreesStoreBrandedOps TREES_STORE_WRITER_BRANDED_OPS = {
	.node_store = {
		.read = trees_store_Writer_NodeStore_read,
		.read_record = trees_store_Writer_NodeStore_read_record,
		.write = trees_store_Writer_NodeStore_write,
		.as_dyn = trees_store_Writer_NodeStore_as_dyn,
		.prefetch = trees_store_NodeStore_prefetch,
		.locate = trees_store_NodeStore_locate,
	},
	.wrote = trees_store_Writer_Branded_wrote,
};

#ifdef DOLTGRES_TEST_TREES

/*
 * test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_frame_0_t
 * is the test's state across its pad: an unwrap's error, the leaves' sizes,
 * the values walk, the key being made, the builder, the store, and the test
 * Io, while live.
 */
typedef struct test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	TreesStoreMemoryStore store;
	bool		store_live;
	TreesBuildBuilder builder;
	bool		builder_live;
	StdVec		key;
	bool		key_live;
	chunks_hash_HashMapAddressVecU8AddressState_values_iter values;
	bool		values_live;
	StdVec		leaves;
	bool		leaves_live;
	ChunksErrorError error;
	bool		error_live;
} test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_frame_0_t;

/* test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_body_0 is the test's body. */
static void
test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_body_0(void *frame_)
{
	test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_frame_0_t *frame = frame_;
	HostIoIo   *io;
	uint32		target = 8192;
	TreesStoreFresh fresh;
	const StdVec *flat;
	bool		first = true;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	io = &frame->test_io.io;
	frame->store = trees_store_MemoryStore_new();
	frame->store_live = true;
	trees_build_Builder_new(TREES_STORE_MEMORY_STORE_NODE_STORE(&frame->store), target, &frame->builder);
	frame->builder_live = true;
	for (uint64 n = 0; n < 2000; n = std_forward_unchecked_u64_Checked(n, 1))
	{
		uint8		be[8];
		TreesBuildEntry entry;

		std_vec_u8_from_elem(&frame->key, 'x', 2048);
		frame->key_live = true;
		std_u64_to_be_bytes(n, be);
		std_vec_extend_from_slice_Checked(&frame->key, be, sizeof(be), 1, 1);
		std_u64_to_be_bytes(n, be);
		frame->key_live = false;
		trees_build_Entry_new_VecU8Array8U8(&frame->key, be, &entry);
		if (!trees_build_Builder_add(&frame->builder, io, &entry, &frame->error))
		{
			frame->error_live = true;
			STD_RESULT_EXPECT_FAILED(948, 70, "called `Result::unwrap()` on an `Err` value", &frame->error,
									 chunks_error_Error_Debug_fmt);
		}
	}
	frame->builder_live = false;
	if (!trees_build_Builder_finish(&frame->builder, io, &TREES_STORE_MEMORY_STORE_BRANDED_OPS, &fresh,
									&frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(950, 33, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->values = chunks_hash_HashMapAddressVecU8AddressState_values(&frame->store.nodes);
	frame->values_live = true;
	while (chunks_hash_HashMapAddressVecU8AddressState_values_iter_next(&frame->values, &flat))
	{
		TreesNodeNode node = trees_node_Node_view(flat->ptr, flat->len);
		size_t		len;

		if (!trees_node_Node_is_leaf(&node))
			continue;
		len = flat->len;
		if (first)
		{
			std_vec_collect_first(&frame->leaves, &len, 0, sizeof(size_t), _Alignof(size_t));
			frame->leaves_live = true;
			first = false;
		}
		else
			std_vec_extend_one(&frame->leaves, &len, 0, sizeof(size_t), _Alignof(size_t));
	}
	if (first)
	{
		std_vec_new(&frame->leaves, sizeof(size_t), _Alignof(size_t));
		frame->leaves_live = true;
	}
	frame->values_live = false;
	std_VecIntoIter_drop(&frame->values.inner);
	if (!(frame->leaves.len > 100))
	{
		size_t		leaves_len = frame->leaves.len;
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("the tree has "), STD_FMT_ARG(0), STD_FMT_LIT(" leaves")};
		StdFmtArg	args[] = {{&leaves_len, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, 953, 9, &arguments);
	}
	for (size_t k = 0, k_len = frame->leaves.len; k < k_len; k++)
	{
		size_t		bytes = STD_VEC_AT(&frame->leaves, size_t, k);
		size_t		times = STD_MUL(usize, 4, (size_t) target, 955, 30);
		size_t		bound = STD_ADD(usize, times, 4096, 955, 30);

		if (!(bytes <= bound))
		{
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("a leaf takes "), STD_FMT_ARG(0),
			STD_FMT_LIT(" bytes decoded")};
			StdFmtArg	args[] = {{&bytes, std_usize_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 955, 13, &arguments);
		}
	}
	frame->leaves_live = false;
	std_vec_free_Checked(&frame->leaves, sizeof(size_t), _Alignof(size_t));
	frame->store_live = false;
	trees_store_MemoryStore_drop(&frame->store);
	frame->test_io_live = false;
	simc_TestIo_drop(&frame->test_io);
}

/* test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_pad_0 is the test's pad: an unwrap's error, then every other value its frame holds, in reverse order. */
static void
test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_pad_0(void *frame_)
{
	test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_frame_0_t *frame = frame_;

	if (frame->error_live)
		chunks_error_Error_drop(&frame->error);
	if (frame->leaves_live)
		std_vec_free_Checked(&frame->leaves, sizeof(size_t), _Alignof(size_t));
	if (frame->values_live)
		std_VecIntoIter_drop(&frame->values.inner);
	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
	if (frame->builder_live)
		trees_build_Builder_drop(&frame->builder);
	if (frame->store_live)
		trees_store_MemoryStore_drop(&frame->store);
	if (frame->test_io_live)
		simc_TestIo_drop(&frame->test_io);
}

/*
 * test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded
 * is the test: a leaf of long keys that share a prefix, stored with the
 * prefix once, stays within a few times the target size in its flat bytes,
 * where each key is whole.
 */
void
test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded(void)
{
	test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_frame_0_t frame = {0};

	std_landing(&frame, test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_body_0,
				test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded_pad_0);
}

/* test_trees_store_tests_copies_frame_0_t is the wide numbers test's state across its pad: its capability and its packs, while live. */
typedef struct test_trees_store_tests_copies_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	ChunksErrorError error;
	bool		error_live;
} test_trees_store_tests_copies_frame_0_t;

/* test_trees_store_tests_copies_pad_0 is the wide numbers test's pad: what it holds, while live. */
static void
test_trees_store_tests_copies_pad_0(void *frame_)
{
	test_trees_store_tests_copies_frame_0_t *frame = frame_;

	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	if (frame->test_io_live)
	{
		frame->test_io_live = false;
		simc_TestIo_drop(&frame->test_io);
	}
}

/* test_trees_store_tests_copies_body_0 is the wide numbers test's body. */
static void
test_trees_store_tests_copies_body_0(void *frame_)
{
	test_trees_store_tests_copies_frame_0_t *frame = frame_;
	ChunksAddressAddress address;
	HostCacheKey low;
	HostCacheKey wide;
	HostCacheKey same;
	StdBytes	low_bytes;
	StdBytes	other_bytes;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (!chunks_packs_open_Packs_create(&frame->test_io.io, 7801, HOST_PG_DEFAULT, &frame->packs, &frame->error))
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(992, 71, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
	frame->packs_live = true;
	address = chunks_address_Address_of((const uint8 *) "node", 4);
	low = trees_store_key(&frame->packs, &address, chunks_trailer_Locator_at(1, 5, 9));
	wide = trees_store_key(&frame->packs, &address, chunks_trailer_Locator_at(((uint64) 1 << 32) + 1, 5, 9));
	low_bytes.ptr = host_cache_Key_bytes(&low);
	low_bytes.len = HOST_CACHE_KEY;
	other_bytes.ptr = host_cache_Key_bytes(&wide);
	other_bytes.len = HOST_CACHE_KEY;
	if (memcmp(low_bytes.ptr, other_bytes.ptr, HOST_CACHE_KEY) == 0)
		std_assert_failed(RUST_FILE, 995, 9, "!=", &low_bytes, std_bytes_Debug_fmt, &other_bytes,
						  std_bytes_Debug_fmt, NULL);
	same = trees_store_key(&frame->packs, &address, chunks_trailer_Locator_at(1, 5, 9));
	other_bytes.ptr = host_cache_Key_bytes(&same);
	STD_ASSERT_EQ(996, 9, memcmp(low_bytes.ptr, other_bytes.ptr, HOST_CACHE_KEY) == 0, &low_bytes,
				  std_bytes_Debug_fmt, &other_bytes, std_bytes_Debug_fmt);
	test_trees_store_tests_copies_pad_0(frame);
}

/*
 * test_trees_store_tests_copies_in_packs_with_wide_numbers_have_their_own_keys:
 * two stored copies of one address whose locators differ only above their
 * packs' numbers' low 32 bits have different keys in the node cache.
 */
void
test_trees_store_tests_copies_in_packs_with_wide_numbers_have_their_own_keys(void)
{
	test_trees_store_tests_copies_frame_0_t frame = {0};

	std_landing(&frame, test_trees_store_tests_copies_body_0, test_trees_store_tests_copies_pad_0);
}

/* test_trees_store_tests_writer_frame_0_t is a Writer test's state across its pad: what it holds, each while live. */
typedef struct test_trees_store_tests_writer_frame_0_t
{
	SimcTestIo	test_io;
	bool		test_io_live;
	HostSharedReader reading;
	bool		reading_live;
	ChunksPacksBackendPacks packs;
	bool		packs_live;
	TreesStoreWriter writer;
	bool		writer_live;
	StdVec		hashed;
	bool		hashed_live;
	StdVec		got;
	bool		got_live;
	StdVec		want;
	bool		want_live;
	ChunksErrorError error;
	bool		error_live;
} test_trees_store_tests_writer_frame_0_t;

/* test_trees_store_tests_writer_pad_0 is a Writer test's pad: what it holds, while live. */
static void
test_trees_store_tests_writer_pad_0(void *frame_)
{
	test_trees_store_tests_writer_frame_0_t *frame = frame_;

	if (frame->error_live)
	{
		frame->error_live = false;
		chunks_error_Error_drop(&frame->error);
	}
	if (frame->want_live)
	{
		frame->want_live = false;
		std_vec_free_Checked(&frame->want, 1, 1);
	}
	if (frame->got_live)
	{
		frame->got_live = false;
		std_vec_free_Checked(&frame->got, 1, 1);
	}
	if (frame->hashed_live)
	{
		frame->hashed_live = false;
		std_vec_free_Checked(&frame->hashed, 1, 1);
	}
	if (frame->writer_live)
	{
		frame->writer_live = false;
		trees_store_Writer_drop(&frame->writer);
	}
	if (frame->packs_live)
	{
		frame->packs_live = false;
		chunks_packs_backend_Packs_drop(&frame->packs);
	}
	if (frame->reading_live)
	{
		frame->reading_live = false;
		host_shared_Reader_drop(&frame->reading);
	}
	if (frame->test_io_live)
	{
		frame->test_io_live = false;
		simc_TestIo_drop(&frame->test_io);
	}
}

/* test_trees_store_tests_unwrap fails a Writer test at a site of its Rust when a step returned an error. */
static void
test_trees_store_tests_unwrap(test_trees_store_tests_writer_frame_0_t *frame, bool ok, uint32 line, uint32 col)
{
	if (!ok)
	{
		frame->error_live = true;
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &frame->error,
								 chunks_error_Error_Debug_fmt);
	}
}

/* test_trees_store_tests_writer_start makes a Writer test's capability, reader's guard, packs, and writer. */
static void
test_trees_store_tests_writer_start(test_trees_store_tests_writer_frame_0_t *frame, uint32 read_line,
									uint32 read_col, uint32 writer_line, uint32 writer_col)
{
	HostError	host_error;
	TreesStoreReader reader;

	frame->test_io = simc_io();
	frame->test_io_live = true;
	if (!host_shared_read(&frame->test_io.io, &frame->reading, &host_error))
	{
		frame->error = chunks_error_Error_FromError_from(&host_error);
		test_trees_store_tests_unwrap(frame, false, read_line, read_col);
	}
	frame->reading_live = true;
	test_trees_store_tests_unwrap(frame, chunks_packs_open_Packs_create(&frame->test_io.io, 1, HOST_PG_DEFAULT,
																		&frame->packs, &frame->error),
								  834, 56);
	frame->packs_live = true;
	reader.packs = &frame->packs;
	reader.placement = HOST_CACHE_PLACEMENT_SERVER;
	reader.reading = &frame->reading;
	test_trees_store_tests_unwrap(frame, trees_store_Writer_new(&frame->test_io.io, reader, CHUNKS_PACKS_DEFAULT_SPACE,
																&frame->writer, &frame->error),
								  writer_line, writer_col);
	frame->writer_live = true;
}

/* test_trees_store_tests_stamps returns a stamps of one row written by a transaction. */
static ChunksTrailerStamps
test_trees_store_tests_stamps(uint64 txn)
{
	uint8		stamp[CHUNKS_TRAILER_STAMP_LEN] = {0};
	ChunksTrailerStamp read;
	StdVec		single;

	std_u64_to_le_bytes(txn, stamp);
	read = chunks_trailer_Stamp_read(stamp, sizeof(stamp));
	std_slice_to_vec_Checked(&single, &read, 1, sizeof(read), _Alignof(ChunksTrailerStamp));
	return chunks_trailer_Stamps_FromVecStamp_from(&single);
}

/* test_trees_store_tests_write writes a copy of the hashed bytes with stamps through the writer, unwrapping. */
static TreesStoreTreeRef
test_trees_store_tests_write(test_trees_store_tests_writer_frame_0_t *frame, uint64 txn, uint32 line, uint32 col)
{
	ChunksTrailerStamps stamps = test_trees_store_tests_stamps(txn);
	StdVec		copy;
	TreesStoreTreeRef written;
	bool		ok;

	std_vec_clone(&copy, &frame->hashed, 1, 1);
	ok = TREES_STORE_WRITER_BRANDED_OPS.node_store.write(&frame->writer.core, &frame->test_io.io, &copy, NULL, 0,
														 &stamps, &written, &frame->error);
	chunks_trailer_Stamps_drop(&stamps);
	test_trees_store_tests_unwrap(frame, ok, line, col);
	return written;
}

/* test_trees_store_tests_record_is reads a record through the writer and asserts it is the hashed bytes stored with a transaction's stamps. */
static void
test_trees_store_tests_record_is(test_trees_store_tests_writer_frame_0_t *frame, const ChunksAddressAddress *address,
								 ChunksTrailerLocator locator, uint64 txn, uint32 unwrap_line, uint32 unwrap_col,
								 uint32 line, uint32 col)
{
	ChunksChunkChunk chunk;
	ChunksTrailerStamps stamps = test_trees_store_tests_stamps(txn);
	StdBytes	got;
	StdBytes	want;

	test_trees_store_tests_unwrap(frame, TREES_STORE_WRITER_BRANDED_OPS.node_store.read_record(&frame->writer.core,
																							   &frame->test_io.io,
																							   address, locator,
																							   &chunk,
																							   &frame->error),
								  unwrap_line, unwrap_col);
	chunks_chunk_Chunk_into_bytes(&chunk, &frame->got);
	frame->got_live = true;
	chunks_trailer_store(frame->hashed.ptr, frame->hashed.len, NULL, 0, &stamps, &frame->want);
	chunks_trailer_Stamps_drop(&stamps);
	frame->want_live = true;
	got.ptr = frame->got.ptr;
	got.len = frame->got.len;
	want.ptr = frame->want.ptr;
	want.len = frame->want.len;
	STD_ASSERT_EQ(line, col, got.len == want.len && memcmp(got.ptr, want.ptr, got.len) == 0, &got,
				  std_bytes_Debug_fmt, &want, std_bytes_Debug_fmt);
	frame->want_live = false;
	std_vec_free_Checked(&frame->want, 1, 1);
	frame->got_live = false;
	std_vec_free_Checked(&frame->got, 1, 1);
}

/* test_trees_store_tests_read_by_address_body_0 is the read by address test's body. */
static void
test_trees_store_tests_read_by_address_body_0(void *frame_)
{
	test_trees_store_tests_writer_frame_0_t *frame = frame_;
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF};
	TreesStoreTreeRef first;
	TreesStoreTreeRef folded;
	ChunksChunkChunk chunk;
	TreesNodeFlat flat;
	bool		ok;

	test_trees_store_tests_writer_start(frame, 907, 46, 910, 65);
	header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, (const uint8 *) "one leaf", 8, &frame->hashed);
	chunks_header_Header_drop(&header);
	frame->hashed_live = true;
	first = test_trees_store_tests_write(frame, 0, 917, 71);
	test_trees_store_tests_write(frame, 0, 918, 59);
	test_trees_store_tests_record_is(frame, &first.address, chunks_trailer_Locator_Default_default(), 0,
									 920, 72, 919, 9);
	folded = test_trees_store_tests_write(frame, 200, 923, 74);
	test_trees_store_tests_record_is(frame, &first.address, folded.locator, 200, 925, 68, 924, 9);
	test_trees_store_tests_record_is(frame, &first.address, first.locator, 0, 929, 67, 928, 9);
	ok = TREES_STORE_WRITER_BRANDED_OPS.node_store.read_record(&frame->writer.core, &frame->test_io.io, &first.address,
															   chunks_trailer_Locator_Default_default(), &chunk,
															   &frame->error);
	if (ok)
		chunks_chunk_Chunk_drop(&chunk);
	else
		chunks_error_Error_drop(&frame->error);
	STD_ASSERT(932, 9, !ok, "writer.read_record(io, &first.address, Locator::default()).is_err()");
	ok = TREES_STORE_WRITER_BRANDED_OPS.node_store.read(&frame->writer.core, &frame->test_io.io, &first.address,
														chunks_trailer_Locator_Default_default(), &flat,
														&frame->error);
	if (ok)
		trees_node_Flat_drop(&flat);
	else
		chunks_error_Error_drop(&frame->error);
	STD_ASSERT(933, 9, !ok, "writer.read(io, &first.address, Locator::default()).is_err()");
	test_trees_store_tests_writer_pad_0(frame);
}

/*
 * test_trees_store_tests_a_writers_read_by_address_alone_of_a_chunk_it_wrote_with_other_stamps_is_refused_and_of_equal_copies_answers:
 * a writer's read by address alone of a chunk it wrote twice with the same
 * stamps answers, and of one it wrote with other stamps is refused, while a
 * read at each copy's place gives that copy.
 */
void
test_trees_store_tests_a_writers_read_by_address_alone_of_a_chunk_it_wrote_with_other_stamps_is_refused_and_of_equal_copies_answers(void)
{
	test_trees_store_tests_writer_frame_0_t frame = {0};

	std_landing(&frame, test_trees_store_tests_read_by_address_body_0, test_trees_store_tests_writer_pad_0);
}

/* test_trees_store_tests_failed_read_body_0 is the failed read test's body. */
static void
test_trees_store_tests_failed_read_body_0(void *frame_)
{
	test_trees_store_tests_writer_frame_0_t *frame = frame_;
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF};
	uint8		body[4096];
	TreesStoreTreeRef first;
	ChunksChunkChunk chunk;
	bool		ok;
	bool		failed;

	test_trees_store_tests_writer_start(frame, 965, 46, 968, 65);
	memset(body, 'x', sizeof(body));
	header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, body, sizeof(body), &frame->hashed);
	chunks_header_Header_drop(&header);
	frame->hashed_live = true;
	first = test_trees_store_tests_write(frame, 0, 975, 71);
	test_trees_store_tests_write(frame, 200, 976, 61);
	simc_out_of_memory_at_next_decompress();
	ok = TREES_STORE_WRITER_BRANDED_OPS.node_store.read_record(&frame->writer.core, &frame->test_io.io, &first.address,
															   first.locator, &chunk, &frame->error);
	if (ok)
		chunks_chunk_Chunk_drop(&chunk);
	else
		frame->error_live = true;
	failed = !ok && frame->error.tag == CHUNKS_ERROR_ERROR_HOST &&
		memcmp(frame->error.u.host.sqlstate, "53200", 5) == 0;
	if (!failed)
		STD_ASSERT_MSG(979, 9, false,
					   "matches!(&failed, Err(Error::Host(error)) if error.sqlstate() == b\"53200\")");
	frame->error_live = false;
	chunks_error_Error_drop(&frame->error);
	test_trees_store_tests_record_is(frame, &first.address, first.locator, 0, 981, 67, 980, 9);
	test_trees_store_tests_writer_pad_0(frame);
}

/*
 * test_trees_store_tests_a_writers_failed_read_of_its_own_copy_returns_the_hosts_error_and_not_the_last_copy:
 * a writer's read of a copy it wrote that fails with the host's own error
 * returns that error: only a place that holds no copy of the chunk falls
 * back to the last copy written.
 */
void
test_trees_store_tests_a_writers_failed_read_of_its_own_copy_returns_the_hosts_error_and_not_the_last_copy(void)
{
	test_trees_store_tests_writer_frame_0_t frame = {0};

	std_landing(&frame, test_trees_store_tests_failed_read_body_0, test_trees_store_tests_writer_pad_0);
}

/* test_trees_store_tests_dictionary_frame_0_t is the dictionary test's state across its pad: the writer test's, its other writers, and the pack it trains, each while live. */
typedef struct test_trees_store_tests_dictionary_frame_0_t
{
	test_trees_store_tests_writer_frame_0_t base;
	TreesStoreWriter training;
	bool		training_live;
	TreesStoreWriter sharing;
	bool		sharing_live;
	ChunksPackSharedDictionary shared;
	bool		shared_live;
	ChunksPackPackWriterInDir pack;
	bool		pack_live;
	ChunksPackPack finished;
	bool		finished_live;
	ChunksPackPackReader reader;
	bool		reader_live;
	StdVec		body;
	bool		body_live;
	StdVec		stored;
	bool		stored_live;
	chunks_OptionVecU8_t dictionary;
} test_trees_store_tests_dictionary_frame_0_t;

/* test_trees_store_tests_dictionary_pack_pad drops the pack one trained_dictionary writes and what it reads, while live. */
static void
test_trees_store_tests_dictionary_pack_pad(test_trees_store_tests_dictionary_frame_0_t *frame)
{
	if (frame->stored_live)
	{
		frame->stored_live = false;
		std_vec_free_Checked(&frame->stored, 1, 1);
	}
	if (frame->body_live)
	{
		frame->body_live = false;
		std_vec_free_Checked(&frame->body, 1, 1);
	}
	if (frame->reader_live)
	{
		frame->reader_live = false;
		chunks_pack_PackReader_drop(&frame->reader);
	}
	if (frame->finished_live)
	{
		frame->finished_live = false;
		chunks_pack_Pack_drop(&frame->finished);
	}
	if (frame->pack_live)
	{
		frame->pack_live = false;
		chunks_pack_PackWriterInDir_drop(&frame->pack);
	}
}

/* test_trees_store_tests_dictionary_pad_0 is the dictionary test's pad: what it holds, while live. */
static void
test_trees_store_tests_dictionary_pad_0(void *frame_)
{
	test_trees_store_tests_dictionary_frame_0_t *frame = frame_;

	test_trees_store_tests_dictionary_pack_pad(frame);
	if (frame->dictionary.has)
	{
		frame->dictionary.has = false;
		std_vec_free_Checked(&frame->dictionary.value, 1, 1);
	}
	if (frame->sharing_live)
	{
		frame->sharing_live = false;
		trees_store_Writer_drop(&frame->sharing);
	}
	if (frame->shared_live)
	{
		frame->shared_live = false;
		chunks_pack_SharedDictionary_drop(frame->shared);
	}
	if (frame->training_live)
	{
		frame->training_live = false;
		trees_store_Writer_drop(&frame->training);
	}
	test_trees_store_tests_writer_pad_0(&frame->base);
}

/* test_trees_store_tests_leaf writes a leaf of a body into the pack, the Rust's leaf and pack.write, unwrapping. */
static void
test_trees_store_tests_leaf(test_trees_store_tests_dictionary_frame_0_t *frame, bool leaf, uint32 line, uint32 col)
{
	ChunksHeaderHeader header = {.kind = CHUNKS_HEADER_TREE_LEAF};
	StdVec		hashed;
	ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;

	header.addresses.tag = CHUNKS_HEADER_ADDRESSES_OFFSETS;
	std_vec_new(&header.addresses.u.offsets, sizeof(uint32), _Alignof(uint32));
	chunks_header_Header_write(&header, frame->body.ptr, frame->body.len, &hashed);
	chunks_header_Header_drop(&header);
	address = chunks_address_Address_of(hashed.ptr, hashed.len);
	chunks_trailer_store(hashed.ptr, hashed.len, NULL, 0, &stamps, &frame->stored);
	std_vec_free_Checked(&hashed, 1, 1);
	frame->stored_live = true;
	frame->body_live = false;
	std_vec_free_Checked(&frame->body, 1, 1);
	test_trees_store_tests_unwrap(&frame->base, chunks_pack_PackWriterInDir_write(&frame->pack, &frame->base.test_io.io,
																				  address, frame->stored.ptr,
																				  frame->stored.len, leaf, &locator,
																				  &frame->base.error),
								  line, col);
	frame->stored_live = false;
	std_vec_free_Checked(&frame->stored, 1, 1);
}

/*
 * test_trees_store_tests_trained_dictionary writes 64 leaves to the pack,
 * pads it with 32 MB of incompressible chunks that are not leaves, writes
 * one more leaf, and gives the dictionary the finished pack holds, if any.
 */
static void
test_trees_store_tests_trained_dictionary(test_trees_store_tests_dictionary_frame_0_t *frame)
{
	HostIoIo   *io = &frame->base.test_io.io;
	uint64		state = UINT64CONST(0x9e3779b97f4a7c15);
	StdVec		dictionary;
	ChunksErrorError error;

	for (size_t k = 0, k_len = 65; k < k_len; k++)
	{
		uint32		i = (uint32) k;
		size_t		len = std_wrapping_add_usize(1100, STD_REM_FOLDED(u32, i, 300));

		std_vec_with_capacity_Checked(&frame->body, len, 1, 1);
		frame->body_live = true;
		for (size_t j = 0, j_len = len; j < j_len; j++)
			std_vec_u8_push_Checked(&frame->body,
									std_wrapping_add_u8(STD_REM_FOLDED(u8, (uint8) std_wrapping_add_u32(std_wrapping_mul_u32((uint32) j, 7), i), 13),
														'a'));
		test_trees_store_tests_leaf(frame, true, 851, 52);
		while (i == 63 && chunks_pack_PackWriterInDir_size(&frame->pack) < ((uint64) 32 << 20))
		{
			std_vec_with_capacity_Checked(&frame->body, 1 << 20, 1, 1);
			frame->body_live = true;
			for (size_t n = 0, n_len = 1 << 20; n < n_len; n++)
			{
				state ^= STD_SHL_FOLDED(u64, state, 13);
				state ^= STD_SHR_FOLDED(u64, state, 7);
				state ^= STD_SHL_FOLDED(u64, state, 17);
				std_vec_u8_push_Checked(&frame->body, (uint8) state);
			}
			test_trees_store_tests_leaf(frame, false, 862, 57);
		}
	}
	frame->pack_live = false;
	test_trees_store_tests_unwrap(&frame->base, chunks_pack_PackWriterInDir_finish(&frame->pack, HOST_IO_IO_TAKES_SHORT(io),
																				   &frame->finished, &frame->base.error),
								  865, 40);
	frame->finished_live = true;
	test_trees_store_tests_unwrap(&frame->base, chunks_pack_PackReader_open(HOST_IO_IO_READ_DOORS(io),
																			frame->finished.path.bytes.ptr,
																			frame->finished.path.bytes.len,
																			&frame->reader, &frame->base.error),
								  866, 54);
	frame->reader_live = true;
	frame->dictionary.has = chunks_pack_PackReader_dictionary(&frame->reader, io, 0, &dictionary, &error);
	if (frame->dictionary.has)
		frame->dictionary.value = dictionary;
	else
		chunks_error_Error_drop(&error);
	test_trees_store_tests_dictionary_pack_pad(frame);
}

/* test_trees_store_tests_option_bytes_Debug_fmt is the Debug of an Option<Vec<u8>>. */
static bool
test_trees_store_tests_option_bytes_Debug_fmt(const void *value, StdFormatter *f)
{
	const chunks_OptionVecU8_t *option = value;
	StdBytes	bytes = {option->value.ptr, option->value.len};
	StdOptionRef shown = {option->has, {&bytes, std_bytes_Debug_fmt}};

	return std_option_Debug_fmt(&shown, f);
}

/* test_trees_store_tests_next_pack starts a writer's next pack for trained_dictionary, unwrapping. */
static void
test_trees_store_tests_next_pack(test_trees_store_tests_dictionary_frame_0_t *frame, TreesStoreWriter *writer,
								 uint32 line, uint32 col)
{
	test_trees_store_tests_unwrap(&frame->base, trees_store_Writer_next_pack(&writer->core, &frame->base.test_io.io,
																			 &frame->pack, &frame->base.error),
								  line, col);
	frame->pack_live = true;
	test_trees_store_tests_trained_dictionary(frame);
}

/* test_trees_store_tests_dictionary_body_0 is the dictionary test's body. */
static void
test_trees_store_tests_dictionary_body_0(void *frame_)
{
	test_trees_store_tests_dictionary_frame_0_t *frame = frame_;
	TreesStoreReader reader;
	chunks_OptionVecU8_t none = {0};
	const chunks_OptionVecU8_t *got;
	chunks_OptionVecU8_t flattened = {0};

	test_trees_store_tests_writer_start(&frame->base, 873, 46, 876, 60);
	reader = trees_store_Writer_reader(&frame->base.writer.core);
	test_trees_store_tests_next_pack(frame, &frame->base.writer, 879, 48);
	STD_ASSERT_EQ(877, 9, !frame->dictionary.has, &frame->dictionary,
				  test_trees_store_tests_option_bytes_Debug_fmt, &none, test_trees_store_tests_option_bytes_Debug_fmt);
	test_trees_store_tests_unwrap(&frame->base, trees_store_Writer_new(&frame->base.test_io.io, reader,
																	   CHUNKS_PACKS_DEFAULT_SPACE, &frame->training,
																	   &frame->base.error),
								  884, 67);
	frame->training_live = true;
	trees_store_Writer_train_dictionaries(&frame->training.core);
	test_trees_store_tests_next_pack(frame, &frame->training, 888, 51);
	STD_ASSERT(886, 9, frame->dictionary.has,
			   "{ let next = training.next_pack(io).unwrap(); trained_dictionary(io, next) }.is_some()");
	frame->dictionary.has = false;
	std_vec_free_Checked(&frame->dictionary.value, 1, 1);
	frame->shared = chunks_pack_SharedDictionary_Default_default();
	frame->shared_live = true;
	test_trees_store_tests_unwrap(&frame->base, trees_store_Writer_new(&frame->base.test_io.io, reader,
																	   CHUNKS_PACKS_DEFAULT_SPACE, &frame->sharing,
																	   &frame->base.error),
								  894, 66);
	frame->sharing_live = true;
	trees_store_Writer_share_dictionary(&frame->sharing.core, std_arc_clone(frame->shared));
	test_trees_store_tests_next_pack(frame, &frame->sharing, 897, 46);
	STD_ASSERT(900, 9, frame->dictionary.has, "dictionary.is_some()");
	got = chunks_pack_SharedDictionary_get(frame->shared);
	if (got != NULL)
		flattened = *got;
	STD_ASSERT_EQ(901, 9,
				  flattened.has == frame->dictionary.has &&
				  (!flattened.has || (flattened.value.len == frame->dictionary.value.len &&
									  memcmp(flattened.value.ptr, frame->dictionary.value.ptr,
											 flattened.value.len) == 0)),
				  &flattened, test_trees_store_tests_option_bytes_Debug_fmt, &frame->dictionary,
				  test_trees_store_tests_option_bytes_Debug_fmt);
	test_trees_store_tests_dictionary_pad_0(frame);
}

/* test_trees_store_tests_a_writers_next_pack_trains_or_shares_a_dictionary_as_its_packs_do: a writer's next pack trains a dictionary of its own or shares one as the writer's packs do. */
void
test_trees_store_tests_a_writers_next_pack_trains_or_shares_a_dictionary_as_its_packs_do(void)
{
	test_trees_store_tests_dictionary_frame_0_t frame = {0};

	std_landing(&frame, test_trees_store_tests_dictionary_body_0, test_trees_store_tests_dictionary_pad_0);
}

#endif							/* DOLTGRES_TEST_TREES */
